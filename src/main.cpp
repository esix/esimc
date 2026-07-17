#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <unistd.h>
#include "ast.h"
#include "codegen.h"
#include "sema.h"

extern FILE* yyin;
extern int yyparse();
extern Program* programRoot;

namespace fs = std::filesystem;

static std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Locate a .sim file in the same directory whose top-level "CLASS NAME;"
// declaration matches the external class name (case-insensitive).
static std::string findExternalClassFile(const std::string& className,
                                         const fs::path& srcDir) {
    auto needle = toLower(className);
    if (!fs::exists(srcDir)) return "";
    for (auto& entry : fs::directory_iterator(srcDir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".sim") continue;
        auto content = readFile(entry.path().string());
        // Strip Simula comments (! ... ;)
        std::string stripped;
        bool inComment = false;
        for (char c : content) {
            if (!inComment && c == '!') { inComment = true; continue; }
            if (inComment) {
                if (c == ';') inComment = false;
                continue;
            }
            stripped += c;
        }
        // Look for "CLASS <name>" at the start (after whitespace/COMMENT)
        std::istringstream iss(stripped);
        std::string tok;
        while (iss >> tok) {
            auto lt = toLower(tok);
            if (lt == "comment") {
                // Skip until ;
                std::string rest;
                std::getline(iss, rest, ';');
                continue;
            }
            if (lt == "class") {
                std::string nameTok;
                if (iss >> nameTok) {
                    // Strip trailing ; or (
                    while (!nameTok.empty() && (nameTok.back() == ';' || nameTok.back() == '('))
                        nameTok.pop_back();
                    if (toLower(nameTok) == needle) {
                        return entry.path().string();
                    }
                }
                break;
            }
            break;
        }
    }
    return "";
}

// Blank occurrences of `word` used as a block prefix (followed by BEGIN). An
// inlined external class's declarations become top-level, so its prefix block
// must degrade to a plain block (the class itself no longer exists).
static void blankPrefixUse(std::string& src, const std::string& word) {
    std::string lc = toLower(src), lw = toLower(word);
    size_t p = 0;
    while ((p = lc.find(lw, p)) != std::string::npos) {
        bool b1 = p == 0 || !(std::isalnum((unsigned char)lc[p-1]) || lc[p-1] == '_');
        size_t e = p + lw.size();
        bool b2 = e >= lc.size() || !(std::isalnum((unsigned char)lc[e]) || lc[e] == '_');
        if (b1 && b2) {
            size_t q = e;
            while (q < lc.size() && std::isspace((unsigned char)lc[q])) q++;
            if (lc.compare(q, 5, "begin") == 0 &&
                (q + 5 >= lc.size() ||
                 !(std::isalnum((unsigned char)lc[q + 5]) || lc[q + 5] == '_'))) {
                for (size_t k = p; k < e; k++) src[k] = ' ';
            }
        }
        p = e;
    }
}

// Inline EXTERNAL CLASS Name declarations by prepending the referenced file's
// content (with EXTERNAL line replaced by an empty statement). Only handles the
// simple form "EXTERNAL CLASS Name;" at file scope.
static std::string preprocessExternals(const std::string& source,
                                       const fs::path& srcDir) {
    std::string out;
    std::vector<std::string> inlinedClasses;
    out.reserve(source.size());
    size_t i = 0;
    auto skipSpaces = [&](size_t pos) {
        while (pos < source.size() && std::isspace((unsigned char)source[pos])) pos++;
        return pos;
    };
    auto matchKeyword = [&](size_t pos, const std::string& kw) -> size_t {
        if (pos + kw.size() > source.size()) return std::string::npos;
        for (size_t k = 0; k < kw.size(); k++) {
            if (std::tolower((unsigned char)source[pos + k]) !=
                std::tolower((unsigned char)kw[k])) return std::string::npos;
        }
        size_t end = pos + kw.size();
        if (end < source.size() && (std::isalnum((unsigned char)source[end]) ||
                                     source[end] == '_')) return std::string::npos;
        return end;
    };
    while (i < source.size()) {
        // Skip comments to avoid matching EXTERNAL inside them
        if (source[i] == '!') {
            out += source[i++];
            while (i < source.size() && source[i] != ';') out += source[i++];
            continue;
        }
        // Try to match EXTERNAL CLASS Name ;
        size_t after = matchKeyword(i, "EXTERNAL");
        if (after != std::string::npos) {
            size_t p = skipSpaces(after);
            size_t after2 = matchKeyword(p, "CLASS");
            if (after2 != std::string::npos) {
                p = skipSpaces(after2);
                // Read identifier
                size_t nameStart = p;
                while (p < source.size() && (std::isalnum((unsigned char)source[p]) ||
                                              source[p] == '_')) p++;
                if (p > nameStart) {
                    std::string className = source.substr(nameStart, p - nameStart);
                    p = skipSpaces(p);
                    if (p < source.size() && source[p] == ';') {
                        // Found "EXTERNAL CLASS Name;" — find and inline the file
                        auto extFile = findExternalClassFile(className, srcDir);
                        if (!extFile.empty()) {
                            auto extContent = readFile(extFile);
                            // Strip the outer "CLASS Name; BEGIN ... END[.;]" wrapper
                            // so the inner declarations become top-level. This emulates
                            // Simula's prefix-block semantics where the importing file
                            // accesses the class's contents as if at top level.
                            auto extracted = extContent;
                            {
                                std::string lc = toLower(extContent);
                                size_t pos = lc.find("class");
                                while (pos != std::string::npos) {
                                    // Ensure word boundary
                                    bool boundaryBefore = pos == 0 ||
                                        !(std::isalnum((unsigned char)lc[pos-1]) || lc[pos-1] == '_');
                                    bool boundaryAfter = pos + 5 >= lc.size() ||
                                        !(std::isalnum((unsigned char)lc[pos+5]) || lc[pos+5] == '_');
                                    if (boundaryBefore && boundaryAfter) break;
                                    pos = lc.find("class", pos + 1);
                                }
                                if (pos != std::string::npos) {
                                    size_t after = pos + 5;
                                    // Skip space, read class name
                                    while (after < lc.size() && std::isspace((unsigned char)lc[after])) after++;
                                    size_t nameEnd = after;
                                    while (nameEnd < lc.size() && (std::isalnum((unsigned char)lc[nameEnd]) ||
                                                                    lc[nameEnd] == '_')) nameEnd++;
                                    std::string foundName = lc.substr(after, nameEnd - after);
                                    if (foundName == toLower(className)) {
                                        // Skip optional ;
                                        size_t p2 = nameEnd;
                                        while (p2 < lc.size() && std::isspace((unsigned char)lc[p2])) p2++;
                                        if (p2 < lc.size() && lc[p2] == ';') p2++;
                                        while (p2 < lc.size() && std::isspace((unsigned char)lc[p2])) p2++;
                                        // Look for BEGIN
                                        if (lc.compare(p2, 5, "begin") == 0) {
                                            size_t bodyStart = p2 + 5;
                                            // Find matching END at end (last END[.;] in file)
                                            size_t lastEnd = lc.size();
                                            while (lastEnd > 0 && (std::isspace((unsigned char)lc[lastEnd-1]) ||
                                                                    lc[lastEnd-1] == '.' ||
                                                                    lc[lastEnd-1] == ';')) lastEnd--;
                                            if (lastEnd >= 3 && lc.compare(lastEnd - 3, 3, "end") == 0) {
                                                extracted = extContent.substr(bodyStart, (lastEnd - 3) - bodyStart);
                                            }
                                        }
                                    }
                                }
                            }
                            out += "\n";
                            out += extracted;
                            out += "\n";
                            inlinedClasses.push_back(className);
                            i = p + 1;
                            continue;
                        }
                    }
                }
            }
        }
        out += source[i++];
    }
    for (auto& n : inlinedClasses) blankPrefixUse(out, n);
    return out;
}

// Build the INFILE standard class as an AST (bypassing the lexer, since names
// like LASTITEM/IMAGE/INIMAGE are reserved keywords and can't be declared in
// Simula source). Backed by the inopen/inreadline/inclose/finint/finreal/
// flastitem runtime primitives. Standard semantics: OPEN does not read;
// the first INIMAGE loads line 1; reading past EOF sets ENDFILE.
//
//   CLASS INFILE(FILENAME); TEXT FILENAME;
//   BEGIN
//     INTEGER FH; TEXT IMAGE; BOOLEAN ENDFILE;
//     PROCEDURE OPEN(BUF); TEXT BUF;
//     BEGIN FH := INOPEN(FILENAME); IF FH = 0 THEN ENDFILE := TRUE; END;
//     PROCEDURE INIMAGE;
//     BEGIN IMAGE :- INREADLINE(FH, 132);
//           IF IMAGE == NOTEXT THEN ENDFILE := TRUE; END;
//     INTEGER PROCEDURE ININT;    ININT  := FININT(FH);
//     REAL    PROCEDURE INREAL;   INREAL := FINREAL(FH);
//     BOOLEAN PROCEDURE LASTITEM; LASTITEM := FLASTITEM(FH);
//     TEXT PROCEDURE INTEXT(N); INTEGER N;
//     BEGIN IF N < IMAGE.LENGTH THEN INTEXT :- IMAGE.SUB(1, N)
//                               ELSE INTEXT :- IMAGE;
//           INIMAGE; END;
//     PROCEDURE CLOSE; INCLOSE(FH);
//   END;
static StmtPtr buildInfileClass() {
    auto id = [](const std::string& n) -> ExprPtr {
        return ExprPtr(new Identifier(n));
    };
    // OPEN body
    StmtList openBody;
    {
        ExprList opArgs; opArgs.push_back(id("filename"));
        openBody.push_back(StmtPtr(new Assignment(
            "fh", ExprPtr(new ProcedureCall("inopen", std::move(opArgs))))));
        auto cond = ExprPtr(new BinaryOp(BinaryOp::EQ, id("fh"),
                                         ExprPtr(new IntegerLiteral(0))));
        auto thenS = StmtPtr(new Assignment("endfile",
                                            ExprPtr(new BooleanLiteral(true))));
        openBody.push_back(StmtPtr(new IfStatement(std::move(cond),
                                                   std::move(thenS))));
    }
    // INIMAGE body
    StmtList inimageBody;
    {
        ExprList rlArgs;
        rlArgs.push_back(id("fh"));
        rlArgs.push_back(ExprPtr(new IntegerLiteral(132)));
        inimageBody.push_back(StmtPtr(new RefAssignment(
            "image", ExprPtr(new ProcedureCall("inreadtext", std::move(rlArgs))))));
        // EOF is a genuine NULL image (raw pointer test) — NOT image = NOTEXT,
        // which is content equality and true for a blank line (length-0 text).
        ExprList noneArgs; noneArgs.push_back(id("image"));
        auto cond = ExprPtr(new ProcedureCall("__isnone", std::move(noneArgs)));
        auto thenS = StmtPtr(new Assignment("endfile",
                                            ExprPtr(new BooleanLiteral(true))));
        inimageBody.push_back(StmtPtr(new IfStatement(std::move(cond),
                                                      std::move(thenS))));
    }
    // ININT / INREAL / LASTITEM bodies (single assignment each)
    auto oneCallBody = [&](const char* retName, const char* prim) {
        StmtList b;
        ExprList a; a.push_back(ExprPtr(new Identifier("fh")));
        b.push_back(StmtPtr(new Assignment(retName,
            ExprPtr(new ProcedureCall(prim, std::move(a))))));
        return b;
    };
    auto inintBody = oneCallBody("inint", "finint");
    auto inrealBody = oneCallBody("inreal", "finreal");
    auto lastitemBody = oneCallBody("lastitem", "flastitem");
    // INTEXT body
    StmtList intextBody;
    {
        auto imageLen = ExprPtr(new MemberAccess(id("image"), "length"));
        auto cond = ExprPtr(new BinaryOp(BinaryOp::LT, id("n"), std::move(imageLen)));
        ExprList subArgs;
        subArgs.push_back(ExprPtr(new IntegerLiteral(1)));
        subArgs.push_back(id("n"));
        auto thenS = StmtPtr(new RefAssignment("intext",
            ExprPtr(new MethodCall(id("image"), "sub", std::move(subArgs)))));
        auto elseS = StmtPtr(new RefAssignment("intext", id("image")));
        intextBody.push_back(StmtPtr(new IfStatement(std::move(cond),
            std::move(thenS), std::move(elseS))));
        intextBody.push_back(StmtPtr(new ExprStatement(
            ExprPtr(new ProcedureCall("inimage", ExprList())))));
    }
    // CLOSE body
    StmtList closeBody;
    {
        ExprList clArgs; clArgs.push_back(id("fh"));
        closeBody.push_back(StmtPtr(new ExprStatement(
            ExprPtr(new ProcedureCall("inclose", std::move(clArgs))))));
    }

    StmtList body;
    body.push_back(StmtPtr(new VarDeclaration(VarDeclaration::INTEGER, "fh")));
    body.push_back(StmtPtr(new VarDeclaration(VarDeclaration::TEXT, "image")));
    body.push_back(StmtPtr(new VarDeclaration(VarDeclaration::BOOLEAN, "endfile")));
    {
        std::vector<ParamSpec> ps; ParamSpec p; p.name = "buf"; p.type = VarDeclaration::TEXT;
        ps.push_back(p);
        body.push_back(StmtPtr(new ProcedureDecl("open", false, VarDeclaration::INTEGER,
            std::move(ps), StmtPtr(new Block(std::move(openBody))))));
    }
    body.push_back(StmtPtr(new ProcedureDecl("inimage", false, VarDeclaration::INTEGER,
        {}, StmtPtr(new Block(std::move(inimageBody))))));
    body.push_back(StmtPtr(new ProcedureDecl("inint", true, VarDeclaration::INTEGER,
        {}, StmtPtr(new Block(std::move(inintBody))))));
    body.push_back(StmtPtr(new ProcedureDecl("inreal", true, VarDeclaration::REAL,
        {}, StmtPtr(new Block(std::move(inrealBody))))));
    body.push_back(StmtPtr(new ProcedureDecl("lastitem", true, VarDeclaration::BOOLEAN,
        {}, StmtPtr(new Block(std::move(lastitemBody))))));
    {
        std::vector<ParamSpec> ps; ParamSpec p; p.name = "n"; p.type = VarDeclaration::INTEGER;
        ps.push_back(p);
        body.push_back(StmtPtr(new ProcedureDecl("intext", true, VarDeclaration::TEXT,
            std::move(ps), StmtPtr(new Block(std::move(intextBody))))));
    }
    body.push_back(StmtPtr(new ProcedureDecl("close", false, VarDeclaration::INTEGER,
        {}, StmtPtr(new Block(std::move(closeBody))))));

    std::vector<ParamSpec> classParams;
    { ParamSpec p; p.name = "filename"; p.type = VarDeclaration::TEXT; classParams.push_back(p); }
    return StmtPtr(new ClassDecl("infile", "", std::move(classParams), std::move(body)));
}

// Build the OUTFILE standard class as an AST, backed by outopen/fouttext/
// foutint/foutimage/outclose runtime primitives.
//
//   CLASS OUTFILE(FILENAME); TEXT FILENAME;
//   BEGIN
//     INTEGER FH;
//     PROCEDURE OPEN(BUF); TEXT BUF;        FH := OUTOPEN(FILENAME);
//     PROCEDURE OUTTEXT(T); TEXT T;         FOUTTEXT(FH, T);
//     PROCEDURE OUTINT(V, W); INTEGER V, W; FOUTINT(FH, V, W);
//     PROCEDURE OUTIMAGE;                   FOUTIMAGE(FH);
//     PROCEDURE CLOSE;                      OUTCLOSE(FH);
//   END;
static StmtPtr buildOutfileClass() {
    auto id = [](const std::string& n) -> ExprPtr {
        return ExprPtr(new Identifier(n));
    };
    StmtList openBody;
    {
        ExprList a; a.push_back(id("filename"));
        openBody.push_back(StmtPtr(new Assignment(
            "fh", ExprPtr(new ProcedureCall("outopen", std::move(a))))));
    }
    StmtList outtextBody;
    {
        ExprList a; a.push_back(id("fh")); a.push_back(id("t"));
        outtextBody.push_back(StmtPtr(new ExprStatement(
            ExprPtr(new ProcedureCall("fouttext", std::move(a))))));
    }
    StmtList outintBody;
    {
        ExprList a; a.push_back(id("fh")); a.push_back(id("v")); a.push_back(id("w"));
        outintBody.push_back(StmtPtr(new ExprStatement(
            ExprPtr(new ProcedureCall("foutint", std::move(a))))));
    }
    StmtList outimageBody;
    {
        ExprList a; a.push_back(id("fh"));
        outimageBody.push_back(StmtPtr(new ExprStatement(
            ExprPtr(new ProcedureCall("foutimage", std::move(a))))));
    }
    StmtList closeBody;
    {
        ExprList a; a.push_back(id("fh"));
        closeBody.push_back(StmtPtr(new ExprStatement(
            ExprPtr(new ProcedureCall("outclose", std::move(a))))));
    }

    StmtList body;
    body.push_back(StmtPtr(new VarDeclaration(VarDeclaration::INTEGER, "fh")));
    {
        std::vector<ParamSpec> ps; ParamSpec p; p.name = "buf"; p.type = VarDeclaration::TEXT;
        ps.push_back(p);
        body.push_back(StmtPtr(new ProcedureDecl("open", false, VarDeclaration::INTEGER,
            std::move(ps), StmtPtr(new Block(std::move(openBody))))));
    }
    {
        std::vector<ParamSpec> ps; ParamSpec p; p.name = "t"; p.type = VarDeclaration::TEXT;
        ps.push_back(p);
        body.push_back(StmtPtr(new ProcedureDecl("outtext", false, VarDeclaration::INTEGER,
            std::move(ps), StmtPtr(new Block(std::move(outtextBody))))));
    }
    {
        std::vector<ParamSpec> ps;
        ParamSpec p; p.name = "v"; p.type = VarDeclaration::INTEGER; ps.push_back(p);
        ParamSpec q; q.name = "w"; q.type = VarDeclaration::INTEGER; ps.push_back(q);
        body.push_back(StmtPtr(new ProcedureDecl("outint", false, VarDeclaration::INTEGER,
            std::move(ps), StmtPtr(new Block(std::move(outintBody))))));
    }
    body.push_back(StmtPtr(new ProcedureDecl("outimage", false, VarDeclaration::INTEGER,
        {}, StmtPtr(new Block(std::move(outimageBody))))));
    body.push_back(StmtPtr(new ProcedureDecl("close", false, VarDeclaration::INTEGER,
        {}, StmtPtr(new Block(std::move(closeBody))))));

    std::vector<ParamSpec> classParams;
    { ParamSpec p; p.name = "filename"; p.type = VarDeclaration::TEXT; classParams.push_back(p); }
    return StmtPtr(new ClassDecl("outfile", "", std::move(classParams), std::move(body)));
}

// If the source uses INFILE/OUTFILE (and doesn't define its own), prepend the
// built-in class to the program's outermost block.
static void maybeInjectInfile(Program* prog, const std::string& source) {
    auto lc = toLower(source);
    auto* blk = dynamic_cast<Block*>(prog->block.get());
    if (!blk) return;
    if (lc.find("infile") != std::string::npos &&
        lc.find("class infile") == std::string::npos) {
        blk->statements.insert(blk->statements.begin(), buildInfileClass());
    }
    if (lc.find("outfile") != std::string::npos &&
        lc.find("class outfile") == std::string::npos) {
        blk->statements.insert(blk->statements.begin(), buildOutfileClass());
    }
}

// SIMSET prelude source: defines LINK and HEAD doubly-linked-list classes.
// Injected verbatim when source starts with "SIMSET" or uses LINK/HEAD.
static const char* SIMSET_PRELUDE = R"SIMSET(
COMMENT Standard SIMSET: LINKAGE with siblings HEAD and LINK, lists as a
        circular two-way ring in which the head is itself a ring node. The
        ishead_ marker replaces the standard's "SUC IN LINK" test so LINKAGE
        need not forward-reference LINK. ;
CLASS LINKAGE;
BEGIN
    REF(LINKAGE) suc_, pred_;
    BOOLEAN ishead_;

    REF(LINK) PROCEDURE SUC;
        IF suc_ =/= NONE AND THEN (NOT suc_.ishead_) THEN SUC :- suc_;

    REF(LINK) PROCEDURE PRED;
        IF pred_ =/= NONE AND THEN (NOT pred_.ishead_) THEN PRED :- pred_;

    REF(LINKAGE) PROCEDURE PREV; PREV :- pred_;
END LINKAGE;

LINKAGE CLASS HEAD;
BEGIN
    REF(LINK) PROCEDURE FIRST; FIRST :- SUC;
    REF(LINK) PROCEDURE LAST;  LAST  :- PRED;

    BOOLEAN PROCEDURE EMPTY; EMPTY := suc_ == THIS HEAD;

    INTEGER PROCEDURE CARDINAL;
    BEGIN
        INTEGER i; REF(LINKAGE) p;
        p :- suc_;
        WHILE p =/= THIS HEAD DO BEGIN i := i + 1; p :- p.suc_ END;
        CARDINAL := i;
    END CARDINAL;

    PROCEDURE CLEAR; WHILE FIRST =/= NONE DO FIRST.OUT;

    ishead_ := TRUE;
    suc_ :- THIS HEAD; pred_ :- THIS HEAD;
END HEAD;

LINKAGE CLASS LINK;
BEGIN
    PROCEDURE OUT;
        IF suc_ =/= NONE THEN
        BEGIN
            suc_.pred_ :- pred_; pred_.suc_ :- suc_;
            suc_ :- NONE; pred_ :- NONE;
        END;

    PROCEDURE FOLLOW(X); REF(LINKAGE) X;
    BEGIN
        OUT;
        IF X =/= NONE AND THEN X.suc_ =/= NONE THEN
        BEGIN
            pred_ :- X; suc_ :- X.suc_;
            suc_.pred_ :- THIS LINK; X.suc_ :- THIS LINK;
        END;
    END FOLLOW;

    PROCEDURE PRECEDE(X); REF(LINKAGE) X;
    BEGIN
        OUT;
        IF X =/= NONE AND THEN X.suc_ =/= NONE THEN
        BEGIN
            suc_ :- X; pred_ :- X.pred_;
            pred_.suc_ :- THIS LINK; X.pred_ :- THIS LINK;
        END;
    END PRECEDE;

    PROCEDURE INTO(S); REF(HEAD) S; PRECEDE(S);
END LINK;
)SIMSET";

// PROCESS prelude for SIMULATION: a LINK subclass whose body detaches at
// creation (processes start passive), runs the user body at INNER, then
// terminates through the scheduler.
static const char* PROCESS_PRELUDE = R"PROC(
LINK CLASS PROCESS;
BEGIN
    DETACH;
    INNER;
    SIMTERM(THIS PROCESS);
END PROCESS;
)PROC";

// Is source[pos..pos+len) a standalone word outside any string literal?
static bool isWordAt(const std::string& src, size_t pos, size_t len) {
    bool pb = (pos == 0) || !(std::isalnum((unsigned char)src[pos-1]) || src[pos-1] == '_');
    bool nb = (pos+len >= src.size()) ||
              !(std::isalnum((unsigned char)src[pos+len]) || src[pos+len] == '_');
    if (!pb || !nb) return false;
    // Count unclosed quotes before pos (Simula strings use "...")
    bool inString = false;
    for (size_t i = 0; i < pos; i++)
        if (src[i] == '"') inString = !inString;
    return !inString;
}

// Case-insensitive standalone-word search outside string literals.
static size_t findWord(const std::string& lcSrc, const std::string& word, size_t from = 0) {
    size_t p = from;
    while ((p = lcSrc.find(word, p)) != std::string::npos) {
        if (isWordAt(lcSrc, p, word.size())) return p;
        p += word.size();
    }
    return std::string::npos;
}

// Blank every standalone occurrence of word (outside strings); word is given
// in lowercase, source may be any case.
static void blankWord(std::string& src, const std::string& word) {
    auto lc = toLower(src);
    size_t p = 0;
    while ((p = findWord(lc, word, p)) != std::string::npos) {
        for (size_t i = 0; i < word.size(); i++) src[p + i] = ' ';
        p += word.size();
    }
}

// Detect SIMSET/SIMULATION prefix usage and prepend the matching preludes.
// SIMULATION implies SIMSET (it is prefixed by it in the standard).
static std::string maybeInjectSimset(const std::string& source) {
    auto lc = toLower(source);
    bool wantsSimulation = findWord(lc, "simulation") != std::string::npos &&
                           lc.find("class process") == std::string::npos;
    bool wantsSimset = findWord(lc, "simset") != std::string::npos;
    if (!wantsSimset && !wantsSimulation) return source;
    if (lc.find("class link") != std::string::npos) return source; // user-defined
    if (lc.find("class head") != std::string::npos) return source;

    std::string result = source;
    blankWord(result, "simset");
    if (wantsSimulation) blankWord(result, "simulation");

    std::string prelude = SIMSET_PRELUDE;
    if (wantsSimulation) prelude += PROCESS_PRELUDE;
    return prelude + "\n" + result;
}

// Shell-quote a path for use inside a system() command line.
static std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Directory containing the running esimc executable.
static fs::path exeDir(const char* argv0) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(argv0), ec);
    if (ec || p.empty()) p = fs::absolute(fs::path(argv0));
    return p.parent_path();
}

// Find the prebuilt runtime object (simula_rt.o) to link the executable against.
static std::string findRuntimeObject(const fs::path& exedir) {
    fs::path candidates[] = {
        exedir / "simula_rt.o",
        exedir / ".." / "build" / "simula_rt.o",
        fs::path("build") / "simula_rt.o",
    };
    std::error_code ec;
    for (auto& c : candidates) {
        if (fs::exists(c)) {
            auto canon = fs::weakly_canonical(c, ec);
            return ec ? c.string() : canon.string();
        }
    }
    return "";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: esimc <source.sim> [-o output] [-O0]\n"
                     "  -o NAME      output file; the extension selects the mode:\n"
                     "                 NAME.ll  -> LLVM IR\n"
                     "                 NAME.o   -> native object file\n"
                     "                 NAME     -> linked executable (default mode)\n"
                     "  -O0          disable optimization (default is -O2)\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile = "output.ll";
    bool optimize = true;

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            outputFile = argv[++i];
        } else if (a == "-O0") {
            optimize = false;
        } else if (a == "-O2" || a == "-O") {
            optimize = true;
        }
    }

    // Preprocess EXTERNAL CLASS references by inlining referenced .sim files.
    // This emulates Simula's separate-compilation model with a simple include.
    auto srcContent = readFile(inputFile);
    auto srcDir = fs::path(inputFile).parent_path();
    auto preprocessed = maybeInjectSimset(preprocessExternals(srcContent, srcDir));

    if (preprocessed != srcContent) {
        // Write to a temp file and parse from there
        std::string tmpPath = "/tmp/esimc_preprocessed.sim";
        std::ofstream tmp(tmpPath);
        tmp << preprocessed;
        tmp.close();
        yyin = fopen(tmpPath.c_str(), "r");
    } else {
        yyin = fopen(inputFile.c_str(), "r");
    }
    if (!yyin) {
        std::cerr << "Error: cannot open '" << inputFile << "'\n";
        return 1;
    }

    if (yyparse() != 0) {
        std::cerr << "Parsing failed.\n";
        fclose(yyin);
        return 1;
    }
    fclose(yyin);

    if (!programRoot) {
        std::cerr << "Error: no program parsed.\n";
        return 1;
    }

    // Inject the built-in INFILE class if the program uses it.
    maybeInjectInfile(programRoot, preprocessed);

    // Semantic analysis: catch errors (e.g. unknown classes) before codegen,
    // with source line numbers.
    SemanticAnalyzer sema;
    sema.analyze(*programRoot);
    if (sema.hadError) {
        std::cerr << "Semantic analysis failed; no output written.\n";
        delete programRoot;
        return 1;
    }

    CodeGenContext context;
    context.optimize = optimize;
    context.generateCode(*programRoot);
    if (context.hadError) {
        std::cerr << "Compilation failed with errors; no output written.\n";
        delete programRoot;
        return 1;
    }
    // The output extension selects what we produce: .ll -> LLVM IR,
    // .o -> native object file, anything else -> a linked executable.
    std::string ext = fs::path(outputFile).extension().string();

    if (ext == ".ll") {
        context.writeIR(outputFile);
        bool ok = !context.hadError;
        delete programRoot;
        if (!ok) return 1;
        std::cout << "Wrote LLVM IR to " << outputFile << "\n";
        return 0;
    }

    if (ext == ".o") {
        bool ok = context.emitObject(outputFile);
        delete programRoot;
        if (!ok) return 1;
        std::cout << "Wrote object file to " << outputFile << "\n";
        return 0;
    }

    // Executable: emit a temporary object, then link it against the runtime
    // with the system C compiler.
    fs::path tmpObj = fs::temp_directory_path() /
                      ("esimc-" + std::to_string((long)getpid()) + ".o");
    bool emitted = context.emitObject(tmpObj.string());
    delete programRoot;
    if (!emitted) return 1;

    std::string rt = findRuntimeObject(exeDir(argv[0]));
    if (rt.empty()) {
        std::cerr << "Error: cannot find the runtime (simula_rt.o); "
                     "build it with 'make' first.\n";
        std::error_code ec; fs::remove(tmpObj, ec);
        return 1;
    }

    const char* ccEnv = std::getenv("CC");
    std::string cc = (ccEnv && *ccEnv) ? ccEnv : "cc";
    std::string cmd = shellQuote(cc) + " " + shellQuote(tmpObj.string()) + " " +
                      shellQuote(rt) + " -lm -o " + shellQuote(outputFile);
    int rc = std::system(cmd.c_str());
    std::error_code ec; fs::remove(tmpObj, ec);
    if (rc != 0) {
        std::cerr << "Error: linking failed (" << cc << ").\n";
        return 1;
    }
    std::cout << "Wrote executable to " << outputFile << "\n";
    return 0;
}
