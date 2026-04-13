#ifndef ESIMC_AST_H
#define ESIMC_AST_H

#include <string>
#include <vector>
#include <memory>

class CodeGenContext;
namespace llvm { class Value; }

class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual llvm::Value* codegen(CodeGenContext& context) = 0;
};

class Expression : public ASTNode {};
class Statement : public ASTNode {};

using ExprPtr = std::unique_ptr<Expression>;
using StmtPtr = std::unique_ptr<Statement>;
using StmtList = std::vector<StmtPtr>;
using ExprList = std::vector<ExprPtr>;

// Parameter specification for procedures and class constructors
struct ParamSpec {
    std::string name;
    int type; // VarDeclaration::Type value
};

// ============================================================
// Expressions
// ============================================================

class IntegerLiteral : public Expression {
public:
    long long value;
    explicit IntegerLiteral(long long v) : value(v) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class RealLiteral : public Expression {
public:
    double value;
    explicit RealLiteral(double v) : value(v) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class TextLiteral : public Expression {
public:
    std::string value;
    explicit TextLiteral(std::string v) : value(std::move(v)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class BooleanLiteral : public Expression {
public:
    bool value;
    explicit BooleanLiteral(bool v) : value(v) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class NoneLiteral : public Expression {
public:
    llvm::Value* codegen(CodeGenContext& context) override;
};

class Identifier : public Expression {
public:
    std::string name;
    explicit Identifier(std::string n) : name(std::move(n)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class BinaryOp : public Expression {
public:
    enum Op { ADD, SUB, MUL, DIV, IDIV, EQ, NE, LT, LE, GT, GE, AND, OR };
    Op op;
    ExprPtr lhs, rhs;
    BinaryOp(Op o, ExprPtr l, ExprPtr r)
        : op(o), lhs(std::move(l)), rhs(std::move(r)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class UnaryOp : public Expression {
public:
    enum Op { NEG, NOT };
    Op op;
    ExprPtr operand;
    UnaryOp(Op o, ExprPtr e) : op(o), operand(std::move(e)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class ProcedureCall : public Expression {
public:
    std::string name;
    ExprList args;
    ProcedureCall(std::string n, ExprList a)
        : name(std::move(n)), args(std::move(a)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// NEW ClassName or NEW ClassName(args)
class NewExpression : public Expression {
public:
    std::string className;
    ExprList args;
    NewExpression(std::string c, ExprList a)
        : className(std::move(c)), args(std::move(a)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// obj.member (field access)
class MemberAccess : public Expression {
public:
    ExprPtr object;
    std::string member;
    MemberAccess(ExprPtr o, std::string m)
        : object(std::move(o)), member(std::move(m)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// obj.method(args) (method call)
class MethodCall : public Expression {
public:
    ExprPtr object;
    std::string method;
    ExprList args;
    MethodCall(ExprPtr o, std::string m, ExprList a)
        : object(std::move(o)), method(std::move(m)), args(std::move(a)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// THIS ClassName
class ThisExpression : public Expression {
public:
    llvm::Value* codegen(CodeGenContext& context) override;
};

// expr IS ClassName
class IsExpression : public Expression {
public:
    ExprPtr object;
    std::string className;
    IsExpression(ExprPtr o, std::string c)
        : object(std::move(o)), className(std::move(c)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// expr IN ClassName (same or derived)
class InExpression : public Expression {
public:
    ExprPtr object;
    std::string className;
    InExpression(ExprPtr o, std::string c)
        : object(std::move(o)), className(std::move(c)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// ============================================================
// Statements
// ============================================================

// BEGIN ... END
class Block : public Statement {
public:
    StmtList statements;
    explicit Block(StmtList stmts) : statements(std::move(stmts)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// INTEGER x; REAL y; BOOLEAN z; TEXT t;
class VarDeclaration : public Statement {
public:
    enum Type { INTEGER, REAL, BOOLEAN, TEXT };
    Type type;
    std::string name;
    ExprPtr init;
    VarDeclaration(Type t, std::string n, ExprPtr i = nullptr)
        : type(t), name(std::move(n)), init(std::move(i)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// REF(ClassName) varname
class RefDeclaration : public Statement {
public:
    std::string className;
    std::string varName;
    RefDeclaration(std::string c, std::string v)
        : className(std::move(c)), varName(std::move(v)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// x := expr
class Assignment : public Statement {
public:
    std::string name;
    ExprPtr value;
    Assignment(std::string n, ExprPtr v)
        : name(std::move(n)), value(std::move(v)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// obj.field := expr
class MemberAssignment : public Statement {
public:
    ExprPtr object;
    std::string member;
    ExprPtr value;
    MemberAssignment(ExprPtr o, std::string m, ExprPtr v)
        : object(std::move(o)), member(std::move(m)), value(std::move(v)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// var :- expr (reference assignment)
class RefAssignment : public Statement {
public:
    std::string name;
    ExprPtr value;
    RefAssignment(std::string n, ExprPtr v)
        : name(std::move(n)), value(std::move(v)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// Expression used as statement
class ExprStatement : public Statement {
public:
    ExprPtr expr;
    explicit ExprStatement(ExprPtr e) : expr(std::move(e)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// IF cond THEN stmt [ELSE stmt]
class IfStatement : public Statement {
public:
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch;
    IfStatement(ExprPtr c, StmtPtr t, StmtPtr e = nullptr)
        : condition(std::move(c)), thenBranch(std::move(t)),
          elseBranch(std::move(e)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// WHILE cond DO stmt
class WhileStatement : public Statement {
public:
    ExprPtr condition;
    StmtPtr body;
    WhileStatement(ExprPtr c, StmtPtr b)
        : condition(std::move(c)), body(std::move(b)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// FOR var := start STEP step UNTIL limit DO body
class ForStatement : public Statement {
public:
    std::string var;
    ExprPtr start, step, limit;
    StmtPtr body;
    ForStatement(std::string v, ExprPtr s, ExprPtr st, ExprPtr l, StmtPtr b)
        : var(std::move(v)), start(std::move(s)), step(std::move(st)),
          limit(std::move(l)), body(std::move(b)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// PROCEDURE name(params); specs; body
class ProcedureDecl : public Statement {
public:
    std::string name;
    bool hasReturnType;
    VarDeclaration::Type returnType;
    std::vector<ParamSpec> params;
    StmtPtr body;
    ProcedureDecl(std::string n, bool hasRet, VarDeclaration::Type ret,
                  std::vector<ParamSpec> p, StmtPtr b)
        : name(std::move(n)), hasReturnType(hasRet), returnType(ret),
          params(std::move(p)), body(std::move(b)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// CLASS name(params); specs; BEGIN ... END
// or: ParentClass CLASS name(params); specs; BEGIN ... END
class ClassDecl : public Statement {
public:
    std::string name;
    std::string parentName;
    std::vector<ParamSpec> params;
    StmtList bodyStmts;
    ClassDecl(std::string n, std::string parent, std::vector<ParamSpec> p,
              StmtList body)
        : name(std::move(n)), parentName(std::move(parent)),
          params(std::move(p)), bodyStmts(std::move(body)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// INSPECT ref WHEN Class DO stmt ...
class InspectStatement : public Statement {
public:
    struct WhenClause {
        std::string className;
        StmtPtr body;
    };
    ExprPtr object;
    std::vector<WhenClause> whenClauses;
    StmtPtr otherwiseBody; // may be null
    InspectStatement(ExprPtr o, std::vector<WhenClause> w, StmtPtr ow = nullptr)
        : object(std::move(o)), whenClauses(std::move(w)),
          otherwiseBody(std::move(ow)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// DETACH
class DetachStatement : public Statement {
public:
    llvm::Value* codegen(CodeGenContext& context) override;
};

// RESUME(expr)
class ResumeStatement : public Statement {
public:
    ExprPtr object;
    explicit ResumeStatement(ExprPtr o) : object(std::move(o)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// CALL(expr)
class CallStatement : public Statement {
public:
    ExprPtr object;
    explicit CallStatement(ExprPtr o) : object(std::move(o)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// ---- I/O statements ----

class OutIntStatement : public Statement {
public:
    ExprPtr value, width;
    OutIntStatement(ExprPtr v, ExprPtr w)
        : value(std::move(v)), width(std::move(w)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class OutRealStatement : public Statement {
public:
    ExprPtr value, width, decimals;
    OutRealStatement(ExprPtr v, ExprPtr w, ExprPtr d)
        : value(std::move(v)), width(std::move(w)), decimals(std::move(d)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class OutTextStatement : public Statement {
public:
    ExprPtr text;
    explicit OutTextStatement(ExprPtr t) : text(std::move(t)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class OutImageStatement : public Statement {
public:
    llvm::Value* codegen(CodeGenContext& context) override;
};

// Top-level program
class Program : public ASTNode {
public:
    StmtPtr block;
    explicit Program(StmtPtr b) : block(std::move(b)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

#endif // ESIMC_AST_H
