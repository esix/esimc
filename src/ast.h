#ifndef ESIMC_AST_H
#define ESIMC_AST_H

#include <string>
#include <vector>
#include <memory>
#include <set>

class CodeGenContext;
namespace llvm { class Value; }

class ASTNode {
public:
    int line = 0;  // 1-based source line (0 = unknown); set by the parser
    virtual ~ASTNode() = default;
    virtual llvm::Value* codegen(CodeGenContext& context) = 0;
    // Collect all identifier names referenced in this subtree
    virtual void collectRefs(std::set<std::string>& refs) const {}
};

class Expression : public ASTNode {};
class Statement : public ASTNode {};

using ExprPtr = std::unique_ptr<Expression>;
using StmtPtr = std::unique_ptr<Statement>;
using StmtList = std::vector<StmtPtr>;
using ExprList = std::vector<ExprPtr>;

struct ParamSpec {
    std::string name;
    int type; // VarDeclaration::Type value
    std::string refClassName; // for REF parameters: the class name
    bool isArray = false; // for ARRAY parameters
    int arrayElemType = 0; // VarDeclaration::Type for array elements
    bool isName = false; // NAME (pass-by-reference) parameter
    bool isLabel = false; // LABEL parameter (non-local goto target)
    bool isValue = false; // VALUE parameter (callee gets a private copy)
    bool isProcedure = false; // formal PROCEDURE parameter (a function pointer)
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

class CharLiteral : public Expression {
public:
    char value;
    explicit CharLiteral(char v) : value(v) {}
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
    enum Op { ADD, SUB, MUL, DIV, IDIV, EQ, NE, LT, LE, GT, GE, AND, OR, CONCAT, POWER,
              ANDTHEN, ORELSE,
              // Reference identity (Simula == / =/=): pointer identity for REF
              // objects; frame+start+length identity for TEXT. Distinct from
              // EQ/NE which are value/content comparisons.
              REF_EQ, REF_NE };
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

class NewExpression : public Expression {
public:
    std::string className;
    ExprList args;
    NewExpression(std::string c, ExprList a)
        : className(std::move(c)), args(std::move(a)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class MemberAccess : public Expression {
public:
    ExprPtr object;
    std::string member;
    MemberAccess(ExprPtr o, std::string m)
        : object(std::move(o)), member(std::move(m)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class MethodCall : public Expression {
public:
    ExprPtr object;
    std::string method;
    ExprList args;
    MethodCall(ExprPtr o, std::string m, ExprList a)
        : object(std::move(o)), method(std::move(m)), args(std::move(a)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class ThisExpression : public Expression {
public:
    llvm::Value* codegen(CodeGenContext& context) override;
};

class IsExpression : public Expression {
public:
    ExprPtr object;
    std::string className;
    IsExpression(ExprPtr o, std::string c)
        : object(std::move(o)), className(std::move(c)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class InExpression : public Expression {
public:
    ExprPtr object;
    std::string className;
    InExpression(ExprPtr o, std::string c)
        : object(std::move(o)), className(std::move(c)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// Conditional expression: IF cond THEN expr1 ELSE expr2
class ConditionalExpr : public Expression {
public:
    ExprPtr condition, thenExpr, elseExpr;
    ConditionalExpr(ExprPtr c, ExprPtr t, ExprPtr e)
        : condition(std::move(c)), thenExpr(std::move(t)),
          elseExpr(std::move(e)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// QUA type qualification: expr QUA ClassName
class QuaExpression : public Expression {
public:
    ExprPtr object;
    std::string className;
    QuaExpression(ExprPtr o, std::string c)
        : object(std::move(o)), className(std::move(c)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// Built-in input expressions
class InIntExpression : public Expression {
public:
    llvm::Value* codegen(CodeGenContext& context) override;
};

class InRealExpression : public Expression {
public:
    llvm::Value* codegen(CodeGenContext& context) override;
};

class InCharExpression : public Expression {
public:
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

// Multiple statements without scope (for multi-var declarations)
class CompoundStmt : public Statement {
public:
    StmtList statements;
    explicit CompoundStmt(StmtList stmts) : statements(std::move(stmts)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// Variable declaration: INTEGER x; REAL y; CHARACTER ch;
class VarDeclaration : public Statement {
public:
    enum Type { INTEGER, REAL, BOOLEAN, TEXT, CHARACTER };
    Type type;
    std::string name;
    ExprPtr init;
    VarDeclaration(Type t, std::string n, ExprPtr i = nullptr)
        : type(t), name(std::move(n)), init(std::move(i)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// ARRAY declaration: INTEGER ARRAY a(1:10)
class ArrayDeclaration : public Statement {
public:
    VarDeclaration::Type elementType;
    std::string name;
    ExprPtr lowerBound, upperBound;
    ExprPtr lowerBound2, upperBound2; // second dimension (nullptr = 1D)
    ExprPtr lowerBound3, upperBound3; // third dimension (nullptr = <=2D)
    std::string refClassName; // for REF arrays: the element class name
    ArrayDeclaration(VarDeclaration::Type t, std::string n, ExprPtr lo, ExprPtr hi,
                     std::string rc = "",
                     ExprPtr lo2 = nullptr, ExprPtr hi2 = nullptr,
                     ExprPtr lo3 = nullptr, ExprPtr hi3 = nullptr)
        : elementType(t), name(std::move(n)),
          lowerBound(std::move(lo)), upperBound(std::move(hi)),
          lowerBound2(std::move(lo2)), upperBound2(std::move(hi2)),
          lowerBound3(std::move(lo3)), upperBound3(std::move(hi3)),
          refClassName(std::move(rc)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// Array element assignment: a(i) := expr  or  a(i,j) := expr  (2D)
class ArrayAssignment : public Statement {
public:
    std::string name;
    ExprPtr index;
    ExprPtr index2; // second dimension index (nullptr = 1D)
    ExprPtr index3; // third dimension index (nullptr = <=2D)
    ExprPtr value;
    bool isRef;     // true for :- (rebind), false for := (in-place for TEXT)
    ArrayAssignment(std::string n, ExprPtr i, ExprPtr v, bool ref,
                    ExprPtr i2 = nullptr, ExprPtr i3 = nullptr)
        : name(std::move(n)), index(std::move(i)), index2(std::move(i2)),
          index3(std::move(i3)), value(std::move(v)), isRef(ref) {}
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
    bool isRef;
    MemberAssignment(ExprPtr o, std::string m, ExprPtr v, bool ref)
        : object(std::move(o)), member(std::move(m)), value(std::move(v)), isRef(ref) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// obj.field(idx) := expr — assign to a member array element.
// Also handles write-through TEXT views: obj.SUB(i,len) := expr (index2 = len).
class MemberArrayAssignment : public Statement {
public:
    ExprPtr object;
    std::string member;
    ExprPtr index;
    ExprPtr index2;   // second argument (e.g. length for .SUB(start,len)); may be null
    ExprPtr value;
    bool isRef;
    MemberArrayAssignment(ExprPtr o, std::string m, ExprPtr i, ExprPtr v, bool ref,
                          ExprPtr i2 = nullptr)
        : object(std::move(o)), member(std::move(m)),
          index(std::move(i)), index2(std::move(i2)),
          value(std::move(v)), isRef(ref) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// var :- expr
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

// LABEL name1, name2;
class LabelDeclaration : public Statement {
public:
    std::vector<std::string> labels;
    explicit LabelDeclaration(std::vector<std::string> l)
        : labels(std::move(l)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// name: statement
class LabeledStatement : public Statement {
public:
    std::string label;
    StmtPtr statement;
    LabeledStatement(std::string l, StmtPtr s)
        : label(std::move(l)), statement(std::move(s)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// GOTO name
class GotoStatement : public Statement {
public:
    std::string label;
    explicit GotoStatement(std::string l) : label(std::move(l)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// INNER — marks where a subclass's body runs inside the prefix class's body.
// Acts as a split point during class-body emission; a bare INNER reached in
// codegen (leaf class) is a no-op.
class InnerStatement : public Statement {
public:
    llvm::Value* codegen(CodeGenContext& context) override;
};

// ACTIVATE/REACTIVATE process scheduling statement
class ActivateStatement : public Statement {
public:
    enum Mode { DIRECT, AT, DELAY, BEFORE, AFTER };
    ExprPtr process;
    Mode mode;
    ExprPtr timeExpr;   // AT/DELAY
    ExprPtr otherProc;  // BEFORE/AFTER
    bool prior = false;
    bool reactivate = false;
    ActivateStatement(ExprPtr p, Mode m, ExprPtr t, ExprPtr o, bool pr, bool re)
        : process(std::move(p)), mode(m), timeExpr(std::move(t)),
          otherProc(std::move(o)), prior(pr), reactivate(re) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// SWITCH S := L1, L2, ..., Ln  (computed goto declaration)
class SwitchDeclaration : public Statement {
public:
    std::string name;
    std::vector<std::string> labels;
    SwitchDeclaration(std::string n, std::vector<std::string> ls)
        : name(std::move(n)), labels(std::move(ls)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// GO TO S(expr)  — computed goto via switch
class ComputedGoto : public Statement {
public:
    std::string switchName;
    ExprPtr index;
    ComputedGoto(std::string s, ExprPtr i)
        : switchName(std::move(s)), index(std::move(i)) {}
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

// FOR var := expr, expr, expr DO body (value list form)
class ForListStatement : public Statement {
public:
    std::string var;
    ExprList values;
    StmtPtr body;
    ForListStatement(std::string v, ExprList vals, StmtPtr b)
        : var(std::move(v)), values(std::move(vals)), body(std::move(b)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// FOR var := <range1>, <range2>, ... DO body (multi-range form)
// Each range is (start, step, limit)
class ForMultiRangeStatement : public Statement {
public:
    struct Range {
        ExprPtr start, step, limit;
    };
    std::string var;
    std::vector<Range> ranges;
    StmtPtr body;
    ForMultiRangeStatement(std::string v, std::vector<Range> r, StmtPtr b)
        : var(std::move(v)), ranges(std::move(r)), body(std::move(b)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// General FOR statement: an ordered list of for-list elements driving one
// controlled variable (Simula 4.4). Elements may freely mix plain values,
// "value WHILE cond", and "A1 STEP A2 UNTIL A3" ranges.
class ForGeneralStatement : public Statement {
public:
    struct Element {
        int kind;            // 0 = value, 1 = value WHILE cond, 2 = step-until
        ExprPtr value;       // the value / A1
        ExprPtr cond;        // kind 1: WHILE condition
        ExprPtr step, limit; // kind 2
    };
    std::string var;
    std::vector<Element> elements;
    StmtPtr body;
    ForGeneralStatement(std::string v, std::vector<Element> els, StmtPtr b)
        : var(std::move(v)), elements(std::move(els)), body(std::move(b)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// PROCEDURE name(params); body
class ProcedureDecl : public Statement {
public:
    std::string name;
    bool hasReturnType;
    VarDeclaration::Type returnType;
    std::string returnRefClass; // for REF(Class) procedures
    std::vector<ParamSpec> params;
    StmtPtr body;
    ProcedureDecl(std::string n, bool hasRet, VarDeclaration::Type ret,
                  std::vector<ParamSpec> p, StmtPtr b, std::string retRefClass = "")
        : name(std::move(n)), hasReturnType(hasRet), returnType(ret),
          returnRefClass(std::move(retRefClass)),
          params(std::move(p)), body(std::move(b)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// CLASS name(params); BEGIN ... END
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
    // Build the class's struct type, body-function and method declarations, and
    // vtable metadata without emitting any bodies. Idempotent. A block runs this
    // for all its classes up-front so they can reference each other regardless of
    // declaration order (Simula sibling classes are mutually visible).
    void declareSkeleton(CodeGenContext& context);
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
    StmtPtr doBody;        // INSPECT e DO s — connected body (NONE skips it)
    StmtPtr otherwiseBody; // OTHERWISE s — unconnected, runs on NONE / no match
    InspectStatement(ExprPtr o, std::vector<WhenClause> w, StmtPtr db = nullptr,
                     StmtPtr ow = nullptr)
        : object(std::move(o)), whenClauses(std::move(w)),
          doBody(std::move(db)), otherwiseBody(std::move(ow)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

// VIRTUAL: method declarations (records method names for vtable)
class VirtualDecl : public Statement {
public:
    std::vector<std::string> methodNames;
    // Declared return type per method, parallel to methodNames.
    // Uses VarDeclaration::Type values; -1 = untyped/void, -2 = REF.
    std::vector<int> returnTypes;
    explicit VirtualDecl(std::vector<std::string> names)
        : methodNames(std::move(names)), returnTypes(methodNames.size(), -1) {}
    VirtualDecl(std::vector<std::string> names, std::vector<int> rets)
        : methodNames(std::move(names)), returnTypes(std::move(rets)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

class DetachStatement : public Statement {
public:
    llvm::Value* codegen(CodeGenContext& context) override;
};

class ResumeStatement : public Statement {
public:
    ExprPtr object;
    explicit ResumeStatement(ExprPtr o) : object(std::move(o)) {}
    llvm::Value* codegen(CodeGenContext& context) override;
};

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

class OutFixStatement : public Statement {
public:
    ExprPtr value;
    ExprPtr decimals;
    ExprPtr width;
    OutFixStatement(ExprPtr v, ExprPtr d, ExprPtr w)
        : value(std::move(v)), decimals(std::move(d)), width(std::move(w)) {}
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

class InImageStatement : public Statement {
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
