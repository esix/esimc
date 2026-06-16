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

// Semantic-analysis pass run between parsing and codegen. It verifies, with
// source line numbers and before codegen:
//   - every class reference (NEW, REF, IS, IN, INSPECT WHEN, prefix, QUA, params)
//     names a declared/standard class
//   - every unqualified identifier resolves to something in scope (local, param,
//     field incl. inherited, global, procedure, label, builtin, or class)
// A scoped symbol table mirrors Simula's block/prefix/closure/INSPECT scoping;
// where the connected class of an INSPECT ... DO cannot be determined, undefined
// checks are suppressed in that body to avoid false positives.
class SemanticAnalyzer {
public:
    bool hadError = false;
    void analyze(Program& program);

private:
    std::set<std::string> classNames;            // declared + standard class names (lowercased)
    std::map<std::string, ClassDecl*> classByName;  // lowercased name -> decl
    std::set<std::string> builtins;              // builtin functions / special identifiers
    std::vector<std::set<std::string>> scopes;   // lexical scope stack (lowercased names)
    int suppressUndef = 0;                        // >0: don't report undefined identifiers

    // pass 1
    void collectClasses(Statement* s);
    // helpers
    void declare(const std::string& name);       // add to current (innermost) scope
    bool resolved(const std::string& name) const;
    void addClassMembers(ClassDecl* cd, std::set<std::string>& out);
    // pass 2
    void checkStmt(Statement* s);
    void checkExpr(Expression* e);
    void checkClassRef(const std::string& name, int line);
    void checkIdent(const std::string& name, int line);
};

#endif // ESIMC_SEMA_H
