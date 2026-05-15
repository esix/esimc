#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include "ast.h"
#include "codegen.h"

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

// Inline EXTERNAL CLASS Name declarations by prepending the referenced file's
// content (with EXTERNAL line replaced by an empty statement). Only handles the
// simple form "EXTERNAL CLASS Name;" at file scope.
static std::string preprocessExternals(const std::string& source,
                                       const fs::path& srcDir) {
    std::string out;
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
                            i = p + 1;
                            continue;
                        }
                    }
                }
            }
        }
        out += source[i++];
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: esimc <source.sim> [-o output.ll]\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile = "output.ll";

    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "-o" && i + 1 < argc) {
            outputFile = argv[++i];
        }
    }

    // Preprocess EXTERNAL CLASS references by inlining referenced .sim files.
    // This emulates Simula's separate-compilation model with a simple include.
    auto srcContent = readFile(inputFile);
    auto srcDir = fs::path(inputFile).parent_path();
    auto preprocessed = preprocessExternals(srcContent, srcDir);

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

    CodeGenContext context;
    context.generateCode(*programRoot);
    context.writeIR(outputFile);

    std::cout << "Wrote LLVM IR to " << outputFile << "\n";

    delete programRoot;
    return 0;
}
