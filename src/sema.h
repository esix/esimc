#ifndef ESIMC_SEMA_H
#define ESIMC_SEMA_H

#include <map>
#include <set>
#include <string>
#include <vector>

class Program;
class Statement;
class Expression;
class ClassDecl;

// What a name denotes in a scope. Lets call sites tell a procedure (check arity)
// from an array (skip — A(I) parses like a call), a param/variable (skip — could
// be a formal procedure), etc.
struct Sym {
    enum Kind { VAR, ARRAY, PROC, KLASS, PARAM, LABEL, SWITCH } kind = VAR;
    int type = -100;          // VarDeclaration::Type, -1 for REF, -100 unknown
    std::string refClass;     // for REF entities
    int arity = -1;           // PROC: param count; -1 if unknown (e.g. formal proc)
};

// Semantic-analysis pass run between parsing and codegen. It verifies, with
// source line numbers and before codegen:
//   - every class reference (NEW, REF, IS, IN, INSPECT WHEN, prefix, QUA, params)
//     names a declared/standard class, and NEW passes the right argument count
//   - every unqualified identifier resolves to something in scope (local, param,
//     field incl. inherited, global, procedure, label, builtin, or class)
//   - procedure calls pass the declared number of arguments
// A scoped symbol table mirrors Simula's block/prefix/closure/INSPECT scoping;
// where the connected class of an INSPECT ... DO cannot be determined, undefined
// checks are suppressed in that body to avoid false positives.
class SemanticAnalyzer {
public:
    bool hadError = false;
    void analyze(Program& program);

private:
    std::set<std::string> classNames;               // declared + standard class names
    std::map<std::string, ClassDecl*> classByName;  // lowercased name -> decl
    std::set<std::string> builtins;                 // builtin functions / special identifiers
    std::vector<std::map<std::string, Sym>> scopes; // lexical scope stack
    int suppressUndef = 0;                           // >0: don't report undefined identifiers

    // pass 1
    void collectClasses(Statement* s);
    // helpers
    void declare(const std::string& name, Sym sym); // add to current (innermost) scope
    bool resolved(const std::string& name) const;
    const Sym* lookup(const std::string& name) const;  // innermost binding, or nullptr
    void addClassMembers(ClassDecl* cd, std::map<std::string, Sym>& out);
    // pass 2
    void checkStmt(Statement* s);
    void checkExpr(Expression* e);
    void checkClassRef(const std::string& name, int line);
    void checkIdent(const std::string& name, int line);
    void checkCallArity(const std::string& name, int argc, int line);
    int  newArity(const std::string& className) const;  // full prefix-chain arity, -1 unknown
};

#endif // ESIMC_SEMA_H
