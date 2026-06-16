#ifndef ESIMC_SEMA_H
#define ESIMC_SEMA_H

#include <set>
#include <string>

class Program;
class Statement;
class Expression;

// Semantic-analysis pass run between parsing and codegen. The first increment
// verifies that every class reference (NEW, REF, IS, IN, INSPECT WHEN, class
// prefix, QUA, REF parameters) names a declared class, reporting unknowns with
// source line numbers before codegen runs. The scaffolding (recursive AST walk,
// name collection) is the foundation for fuller type checking.
class SemanticAnalyzer {
public:
    bool hadError = false;
    void analyze(Program& program);

private:
    std::set<std::string> classNames;  // all declared/injected class names (lowercased)

    void collectClasses(Statement* s);
    void checkStmt(Statement* s);
    void checkExpr(Expression* e);
    void checkClassRef(const std::string& name, int line);
};

#endif // ESIMC_SEMA_H
