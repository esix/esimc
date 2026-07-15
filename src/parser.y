%{
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctype.h>
#include <memory>
#include <vector>
#include <string>
#include <utility>
#include "ast.h"

extern int yylex();
extern int yylineno;
extern FILE* yyin;

void yyerror(const char* s);

Program* programRoot = nullptr;

/* Merge a Simula-style ident_list with param_specs into a single ParamSpec vector.
   Names in the ident_list that don't appear in any spec get type INTEGER by default. */
static std::vector<ParamSpec> mergeParams(
    const std::vector<std::string>& names,
    const std::vector<std::pair<int, std::vector<std::string>>>& specs)
{
    auto matchName = [](const std::string& a, const std::string& b) -> bool {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        return true;
    };
    std::vector<ParamSpec> result;
    for (auto& n : names) {
        int type = VarDeclaration::INTEGER; // default
        std::string refClass;
        bool isArray = false;
        int arrayElem = 0;
        bool isName = false;
        bool isLabel = false;
        bool isValue = false;
        bool isProcedure = false;
        /* First pass: check NAME/VALUE markers */
        for (auto& sp : specs) {
            if (sp.first == -30 || sp.first == -31) {
                for (auto& sn : sp.second) {
                    if (matchName(sn, n)) {
                        if (sp.first == -30) isName = true;
                        else isValue = true;
                        break;
                    }
                }
            }
        }
        /* Check LABEL markers */
        for (auto& sp : specs) {
            if (sp.first == -40) {
                for (auto& sn : sp.second) {
                    if (matchName(sn, n)) { isLabel = true; break; }
                }
            }
        }
        /* Find this name in the specs */
        for (auto& sp : specs) {
            if (sp.first == -30 || sp.first == -31 || sp.first == -1) {
                /* NAME/VALUE/unknown markers — skip in type scan */
                continue;
            }
            if (sp.first == -40) {
                /* LABEL spec: typed as a pointer (TEXT) */
                for (auto& sn : sp.second) {
                    if (matchName(sn, n)) { type = VarDeclaration::TEXT; goto found; }
                }
                continue;
            }
            if (sp.first == -10) {
                /* REF spec: first element is class name, rest are param names */
                for (size_t si = 1; si < sp.second.size(); si++) {
                    auto& sn = sp.second[si];
                    if (sn.size() == n.size()) {
                        bool match = true;
                        for (size_t i = 0; i < n.size(); i++) {
                            if (tolower((unsigned char)sn[i]) != tolower((unsigned char)n[i])) {
                                match = false; break;
                            }
                        }
                        if (match) {
                            type = VarDeclaration::TEXT;
                            refClass = sp.second[0]; // class name
                            goto found;
                        }
                    }
                }
            } else if (sp.first == -12) {
                /* PROCEDURE parameter: a function pointer (TEXT-encoded). */
                for (auto& sn : sp.second) {
                    if (matchName(sn, n)) {
                        type = VarDeclaration::TEXT;
                        isProcedure = true;
                        goto found;
                    }
                }
            } else if (sp.first == -11) {
                /* REF(Class) ARRAY spec: first element is the class name, the rest
                   are param names. Element type is a pointer (TEXT-encoded). */
                for (size_t si = 1; si < sp.second.size(); si++) {
                    if (matchName(sp.second[si], n)) {
                        type = VarDeclaration::TEXT;
                        isArray = true;
                        arrayElem = VarDeclaration::TEXT;
                        refClass = sp.second[0];
                        goto found;
                    }
                }
            } else if (sp.first <= -20) {
                /* Array spec: encode (-20 - elem*1000) */
                int elemTy = (-(sp.first + 20)) / 1000;
                for (auto& sn : sp.second) {
                    if (sn.size() == n.size()) {
                        bool match = true;
                        for (size_t i = 0; i < n.size(); i++) {
                            if (tolower((unsigned char)sn[i]) != tolower((unsigned char)n[i])) {
                                match = false; break;
                            }
                        }
                        if (match) {
                            type = VarDeclaration::TEXT;
                            isArray = true;
                            arrayElem = elemTy;
                            goto found;
                        }
                    }
                }
            } else {
                for (auto& sn : sp.second) {
                    if (sn.size() == n.size()) {
                        bool match = true;
                        for (size_t i = 0; i < n.size(); i++) {
                            if (tolower((unsigned char)sn[i]) != tolower((unsigned char)n[i])) {
                                match = false; break;
                            }
                        }
                        if (match) {
                            type = sp.first;
                            goto found;
                        }
                    }
                }
            }
        }
        found:
        ParamSpec ps;
        ps.name = n; ps.type = type; ps.refClassName = refClass;
        ps.isArray = isArray; ps.arrayElemType = arrayElem;
        ps.isName = isName; ps.isLabel = isLabel; ps.isValue = isValue;
        ps.isProcedure = isProcedure;
        result.push_back(ps);
    }
    return result;
}

/* Deep-copy an expression (used to re-read a chained-assignment middle part).
   Returns nullptr for node kinds not expected in an assignment target/subscript. */
static Expression* cloneExpr(Expression* e);
static ExprList cloneArgs(const ExprList& args) {
    ExprList out;
    for (auto& a : args) out.push_back(ExprPtr(cloneExpr(a.get())));
    return out;
}
static Expression* cloneExpr(Expression* e) {
    if (!e) return nullptr;
    if (auto* x = dynamic_cast<IntegerLiteral*>(e)) return new IntegerLiteral(x->value);
    if (auto* x = dynamic_cast<RealLiteral*>(e))    return new RealLiteral(x->value);
    if (auto* x = dynamic_cast<TextLiteral*>(e))    return new TextLiteral(x->value);
    if (auto* x = dynamic_cast<CharLiteral*>(e))    return new CharLiteral(x->value);
    if (auto* x = dynamic_cast<BooleanLiteral*>(e)) return new BooleanLiteral(x->value);
    if (dynamic_cast<NoneLiteral*>(e))              return new NoneLiteral();
    if (dynamic_cast<ThisExpression*>(e))           return new ThisExpression();
    if (auto* x = dynamic_cast<Identifier*>(e))     return new Identifier(x->name);
    if (auto* x = dynamic_cast<UnaryOp*>(e))        return new UnaryOp(x->op, ExprPtr(cloneExpr(x->operand.get())));
    if (auto* x = dynamic_cast<BinaryOp*>(e))       return new BinaryOp(x->op, ExprPtr(cloneExpr(x->lhs.get())), ExprPtr(cloneExpr(x->rhs.get())));
    if (auto* x = dynamic_cast<ProcedureCall*>(e))  return new ProcedureCall(x->name, cloneArgs(x->args));
    if (auto* x = dynamic_cast<MemberAccess*>(e))   return new MemberAccess(ExprPtr(cloneExpr(x->object.get())), x->member);
    if (auto* x = dynamic_cast<MethodCall*>(e))     return new MethodCall(ExprPtr(cloneExpr(x->object.get())), x->method, cloneArgs(x->args));
    if (auto* x = dynamic_cast<QuaExpression*>(e))  return new QuaExpression(ExprPtr(cloneExpr(x->object.get())), x->className);
    return nullptr;
}

/* Lower `lhs := rhs` to the right assignment node by lhs kind (mirrors the
   single-assignment rule). Takes ownership of lhs and rhs. */
static Statement* lowerAssign(Expression* lhs, Expression* rhs) {
    if (auto* id = dynamic_cast<Identifier*>(lhs)) {
        auto* s = new Assignment(id->name, ExprPtr(rhs)); delete lhs; return s;
    }
    if (auto* call = dynamic_cast<ProcedureCall*>(lhs)) {
        ExprPtr i1(call->args.empty() ? nullptr : call->args[0].release());
        ExprPtr i2(call->args.size() >= 2 ? call->args[1].release() : nullptr);
        ExprPtr i3(call->args.size() >= 3 ? call->args[2].release() : nullptr);
        auto* s = new ArrayAssignment(call->name, std::move(i1), ExprPtr(rhs), false,
                                      std::move(i2), std::move(i3));
        delete lhs; return s;
    }
    if (auto* ma = dynamic_cast<MemberAccess*>(lhs)) {
        auto* s = new MemberAssignment(ExprPtr(ma->object.release()), ma->member,
                                       ExprPtr(rhs), false);
        delete lhs; return s;
    }
    if (auto* mc = dynamic_cast<MethodCall*>(lhs)) {
        ExprPtr i1(mc->args.empty() ? nullptr : mc->args[0].release());
        ExprPtr i2(mc->args.size() >= 2 ? mc->args[1].release() : nullptr);
        auto* s = new MemberArrayAssignment(ExprPtr(mc->object.release()), mc->method,
                                            std::move(i1), ExprPtr(rhs), false, std::move(i2));
        delete lhs; return s;
    }
    delete rhs; delete lhs; return new CompoundStmt(StmtList());
}

/* Desugar a class-prefixed block  C BEGIN ... END  into
     BEGIN REF(C) __pbN; __pbN :- NEW C; INSPECT __pbN DO <block> END
   The prefix object is created (running C's body statements), then the block
   body executes connected to it, so C's attributes are directly visible.
   Exact for prefix classes with no statements after INNER. */
static Statement* makePrefixedBlock(const std::string& cls, Statement* blk) {
    static int pbCounter = 0;
    std::string tmp = "__pb" + std::to_string(pbCounter++);
    StmtList stmts;
    stmts.push_back(StmtPtr(new RefDeclaration(cls, tmp)));
    stmts.push_back(StmtPtr(new RefAssignment(tmp,
        ExprPtr(new NewExpression(cls, ExprList())))));
    stmts.push_back(StmtPtr(new InspectStatement(
        ExprPtr(new Identifier(tmp)),
        std::vector<InspectStatement::WhenClause>(), StmtPtr(blk))));
    return new Block(std::move(stmts));
}
%}

%locations

%union {
    long long ival;
    double dval;
    char* sval;
    Expression* expr;
    Statement* stmt;
    StmtList* stmtlist;
    ExprList* exprlist;
    std::vector<std::string>* namelist;
    std::vector<ParamSpec>* paramlist;
    std::vector<std::pair<int, std::vector<std::string>>>* speclist;
    std::vector<InspectStatement::WhenClause>* whenclauses;
    int vartype;
    std::pair<std::string, int>* ventry;
    std::vector<std::pair<std::string, int>>* ventries;
}

/* Keywords */
%token T_BEGIN T_END
%token T_INTEGER T_REAL T_BOOLEAN T_TEXT T_CHARACTER T_SHORT T_LONG
%token T_IF T_THEN T_ELSE
%token T_WHILE T_DO
%token T_FOR T_STEP T_UNTIL
%token T_TRUE T_FALSE
%token T_NOT T_AND T_OR T_EQV T_IMP T_QUA T_ANDTHEN T_ORELSE
%token T_PROCEDURE T_CLASS T_NEW T_THIS
%token T_REF T_NONE T_IS T_IN
%token T_VIRTUAL
%token T_INSPECT T_WHEN T_OTHERWISE
%token T_DETACH T_RESUME T_CALL T_INNER
%token T_ACTIVATE T_REACTIVATE T_AT T_DELAY T_BEFORE T_AFTER T_PRIOR
%token T_ARRAY T_LABEL T_GOTO T_SWITCH
%token T_NOTEXT T_EXTERNAL
/* T_VALUE and T_NAME removed — handled as identifiers contextually */
%token T_OUTINT T_OUTREAL T_OUTFIX T_OUTTEXT T_OUTIMAGE
%token T_ININT T_INREAL T_INCHAR T_INIMAGE T_LASTITEM

/* Operators and punctuation */
%token T_ASSIGN T_REFASSIGN T_COLON T_SEMI T_COMMA T_DOT T_AMP
%token T_LPAREN T_RPAREN
%token T_PLUS T_MINUS T_STAR T_SLASH T_IDIV T_POWER
%token T_EQ T_NE T_LT T_LE T_GT T_GE T_REFEQ T_REFNE

/* Literals */
%token <ival> T_INTLIT T_CHARLIT
%token <dval> T_REALLIT
%token <sval> T_IDENT T_TEXTLIT

/* Non-terminal types */
%type <expr> expr or_else_expr and_then_expr eqv_expr imp_expr or_expr and_expr
%type <expr> comparison additive multiplicative power unary postfix primary
%type <stmt> statement block
%type <stmt> declaration array_decl ref_declaration
%type <stmt> label_decl labeled_stmt goto_stmt switch_decl
%type <stmt> if_stmt while_stmt for_stmt
%type <stmt> procedure_decl class_decl
%type <stmt> inspect_stmt
%type <stmt> detach_stmt resume_stmt call_stmt activate_stmt
%type <stmt> outint_stmt outreal_stmt outfix_stmt outtext_stmt outimage_stmt inimage_stmt
%type <stmt> expr_stmt virtual_spec top_level_decl external_decl
%type <ventry> virtual_entry
%type <ventries> virtual_entries
%type <vartype> virtual_is_rhs
%type <stmtlist> top_level_decls
%type <stmtlist> stmt_list class_body
%type <exprlist> arg_list arg_list_ne
%type <namelist> ident_list switch_label_list
%type <sval> io_keyword
%type <paramlist> typed_param_list opt_typed_params
%type <speclist> param_specs
%type <vartype> type_name
%type <whenclauses> when_clauses

/* Dangling ELSE */
%nonassoc T_THEN
%nonassoc T_ELSE

%start program

%%

program
    : block             { programRoot = new Program(StmtPtr($1)); }
    | block T_DOT       { programRoot = new Program(StmtPtr($1)); }
    | block T_SEMI      { programRoot = new Program(StmtPtr($1)); }
    | block T_SEMI T_DOT { programRoot = new Program(StmtPtr($1)); }
    | top_level_decls {
        /* Top-level class/external declarations — wrap in an empty block */
        programRoot = new Program(StmtPtr(new Block(std::move(*$1))));
        delete $1;
      }
    | top_level_decls T_DOT {
        programRoot = new Program(StmtPtr(new Block(std::move(*$1))));
        delete $1;
      }
    | top_level_decls block {
        /* class/procedure defs followed by main block (e.g. after SIMSET injection) */
        $1->push_back(StmtPtr($2));
        programRoot = new Program(StmtPtr(new Block(std::move(*$1))));
        delete $1;
      }
    | top_level_decls block T_DOT {
        $1->push_back(StmtPtr($2));
        programRoot = new Program(StmtPtr(new Block(std::move(*$1))));
        delete $1;
      }
    | top_level_decls block T_SEMI {
        $1->push_back(StmtPtr($2));
        programRoot = new Program(StmtPtr(new Block(std::move(*$1))));
        delete $1;
      }
    | top_level_decls T_IDENT block {
        /* prefix block: ClassName BEGIN ... END — execute block in class context */
        $1->push_back(StmtPtr(makePrefixedBlock($2, $3)));
        programRoot = new Program(StmtPtr(new Block(std::move(*$1))));
        delete $1;
      }
    | top_level_decls T_IDENT block T_DOT {
        $1->push_back(StmtPtr(makePrefixedBlock($2, $3)));
        programRoot = new Program(StmtPtr(new Block(std::move(*$1))));
        delete $1;
      }
    | T_IDENT block {
        /* prefix block at top: ClassName BEGIN ... END */
        auto stmts = new StmtList();
        stmts->push_back(StmtPtr(makePrefixedBlock($1, $2)));
        programRoot = new Program(StmtPtr(new Block(std::move(*stmts))));
        delete stmts;
      }
    | T_IDENT block T_DOT {
        auto stmts = new StmtList();
        stmts->push_back(StmtPtr(makePrefixedBlock($1, $2)));
        programRoot = new Program(StmtPtr(new Block(std::move(*stmts))));
        delete stmts;
      }
    ;

top_level_decls
    : top_level_decl { $$ = new StmtList(); $$->push_back(StmtPtr($1)); }
    | top_level_decls top_level_decl { $1->push_back(StmtPtr($2)); $$ = $1; }
    | top_level_decls T_SEMI { $$ = $1; }
    ;

top_level_decl
    : class_decl
    | external_decl
    | procedure_decl
    ;

external_decl
    : T_EXTERNAL T_CLASS T_IDENT {
        /* EXTERNAL CLASS Name — forward declaration, ignored */
        $$ = new ExprStatement(ExprPtr(new IntegerLiteral(0)));
      }
    | T_EXTERNAL T_CLASS T_IDENT T_EQ T_TEXTLIT {
        /* EXTERNAL CLASS Name = "filename" */
        $$ = new ExprStatement(ExprPtr(new IntegerLiteral(0)));
      }
    | T_EXTERNAL type_name T_PROCEDURE T_IDENT {
        $$ = new ExprStatement(ExprPtr(new IntegerLiteral(0)));
      }
    | T_EXTERNAL T_PROCEDURE T_IDENT {
        $$ = new ExprStatement(ExprPtr(new IntegerLiteral(0)));
      }
    ;

block
    : T_BEGIN stmt_list T_END end_names {
        $$ = new Block(std::move(*$2));
        delete $2;
      }
    ;

end_names
    : /* empty */
    | end_names T_IDENT  /* silently consume tokens after END */
    | end_names T_FOR
    | end_names T_WHILE
    | end_names T_IF
    | end_names T_DO
    | end_names T_INSPECT
    | end_names T_CLASS
    | end_names T_PROCEDURE
    ;

stmt_list
    : /* empty */ { $$ = new StmtList(); }
    | stmt_list statement {
        $1->push_back(StmtPtr($2));
        $$ = $1;
      }
    | stmt_list statement T_SEMI {
        $1->push_back(StmtPtr($2));
        $$ = $1;
      }
    | stmt_list T_SEMI { $$ = $1; } /* empty statement (double semicolons) */
    ;

statement
    : declaration
    | array_decl
    | ref_declaration
    | label_decl
    | switch_decl
    | labeled_stmt
    | goto_stmt
    | if_stmt
    | while_stmt
    | for_stmt
    | procedure_decl
    | class_decl
    | inspect_stmt
    | detach_stmt
    | resume_stmt
    | call_stmt
    | activate_stmt
    | outint_stmt
    | outreal_stmt
    | outfix_stmt
    | outtext_stmt
    | outimage_stmt
    | inimage_stmt
    | virtual_spec
    | block
    | T_IDENT block {
        /* class-prefixed block as a statement: C BEGIN ... END */
        $$ = makePrefixedBlock($1, $2);
      }
    | expr_stmt
    ;

/* ---- Type names ---- */

type_name
    : T_INTEGER          { $$ = VarDeclaration::INTEGER; }
    | T_SHORT T_INTEGER  { $$ = VarDeclaration::INTEGER; }
    | T_LONG T_REAL      { $$ = VarDeclaration::REAL; }
    | T_REAL             { $$ = VarDeclaration::REAL; }
    | T_BOOLEAN          { $$ = VarDeclaration::BOOLEAN; }
    | T_TEXT             { $$ = VarDeclaration::TEXT; }
    | T_CHARACTER        { $$ = VarDeclaration::CHARACTER; }
    ;

/* ---- Identifier list ---- */

ident_list
    : T_IDENT {
        $$ = new std::vector<std::string>();
        $$->push_back($1);
      }
    | ident_list T_COMMA T_IDENT {
        $1->push_back($3);
        $$ = $1;
      }
    ;

/* ---- Declarations ---- */

declaration
    : type_name ident_list {
        auto stmts = new StmtList();
        for (auto& name : *$2) {
            stmts->push_back(StmtPtr(new VarDeclaration((VarDeclaration::Type)$1, name)));
        }
        if (stmts->size() == 1) {
            $$ = stmts->front().release();
            delete stmts;
        } else {
            $$ = new CompoundStmt(std::move(*stmts));
            delete stmts;
        }
        delete $2;
      }
    | type_name T_IDENT T_EQ expr {
        /* Initialized declaration: INTEGER x = 42 */
        $$ = new VarDeclaration((VarDeclaration::Type)$1, $2, ExprPtr($4));
      }
    | type_name T_IDENT T_EQ expr T_COMMA ident_list {
        /* Initialized + more: INTEGER x = 42, y, z */
        auto stmts = new StmtList();
        stmts->push_back(StmtPtr(new VarDeclaration((VarDeclaration::Type)$1, $2, ExprPtr($4))));
        for (auto& name : *$6) {
            stmts->push_back(StmtPtr(new VarDeclaration((VarDeclaration::Type)$1, name)));
        }
        $$ = new CompoundStmt(std::move(*stmts));
        delete stmts; delete $6;
      }
    | type_name T_IDENT T_ASSIGN expr {
        /* Initialized declaration: INTEGER x := 42 */
        $$ = new VarDeclaration((VarDeclaration::Type)$1, $2, ExprPtr($4));
      }
    ;

array_decl
    : type_name T_ARRAY T_IDENT T_LPAREN expr T_COLON expr T_RPAREN {
        $$ = new ArrayDeclaration((VarDeclaration::Type)$1, $3,
                                  ExprPtr($5), ExprPtr($7));
      }
    | type_name T_ARRAY T_IDENT T_LPAREN expr T_COLON expr T_RPAREN T_COMMA T_IDENT T_LPAREN expr T_COLON expr T_RPAREN {
        /* Two arrays on one line: TYPE ARRAY a(lo:hi), b(lo:hi) */
        auto stmts = new StmtList();
        stmts->push_back(StmtPtr(new ArrayDeclaration((VarDeclaration::Type)$1, $3,
                                  ExprPtr($5), ExprPtr($7))));
        stmts->push_back(StmtPtr(new ArrayDeclaration((VarDeclaration::Type)$1, $10,
                                  ExprPtr($12), ExprPtr($14))));
        $$ = new CompoundStmt(std::move(*stmts));
        delete stmts;
      }
    | T_REF T_LPAREN T_IDENT T_RPAREN T_ARRAY T_IDENT T_LPAREN expr T_COLON expr T_RPAREN {
        $$ = new ArrayDeclaration(VarDeclaration::TEXT, $6,
                                  ExprPtr($8), ExprPtr($10), $3);
      }
    | type_name T_ARRAY T_IDENT T_LPAREN expr T_COLON expr T_COMMA expr T_COLON expr T_RPAREN {
        /* 2D array: TYPE ARRAY a(lo1:hi1, lo2:hi2) */
        $$ = new ArrayDeclaration((VarDeclaration::Type)$1, $3,
                                  ExprPtr($5), ExprPtr($7),
                                  "", ExprPtr($9), ExprPtr($11));
      }
    | type_name T_ARRAY T_IDENT T_LPAREN expr T_COLON expr T_COMMA expr T_COLON expr T_COMMA expr T_COLON expr T_RPAREN {
        /* 3D array: TYPE ARRAY a(lo1:hi1, lo2:hi2, lo3:hi3) */
        $$ = new ArrayDeclaration((VarDeclaration::Type)$1, $3,
                                  ExprPtr($5), ExprPtr($7),
                                  "", ExprPtr($9), ExprPtr($11),
                                  ExprPtr($13), ExprPtr($15));
      }
    ;

ref_declaration
    : T_REF T_LPAREN T_IDENT T_RPAREN ident_list {
        if ($5->size() == 1) {
            $$ = new RefDeclaration($3, (*$5)[0]);
        } else {
            auto stmts = new StmtList();
            for (auto& name : *$5) {
                auto* rd = new RefDeclaration($3, name);
                rd->line = @1.first_line;
                stmts->push_back(StmtPtr(rd));
            }
            $$ = new CompoundStmt(std::move(*stmts));
            delete stmts;
        }
        $$->line = @1.first_line;
        delete $5;
      }
    ;

label_decl
    : T_LABEL ident_list {
        $$ = new LabelDeclaration(std::move(*$2));
        delete $2;
      }
    ;

labeled_stmt
    : T_IDENT T_COLON statement {
        $$ = new LabeledStatement($1, StmtPtr($3));
      }
    | T_IDENT T_COLON {
        /* Label at end of block (no following statement) */
        $$ = new LabeledStatement($1, StmtPtr(new ExprStatement(
            ExprPtr(new IntegerLiteral(0)))));
      }
    ;

goto_stmt
    : T_GOTO T_IDENT {
        $$ = new GotoStatement($2);
        $$->line = @1.first_line;
      }
    | T_GOTO T_IDENT T_LPAREN expr T_RPAREN {
        /* Computed goto: GO TO S(expr) */
        $$ = new ComputedGoto($2, ExprPtr($4));
        $$->line = @1.first_line;
      }
    ;

switch_decl
    : T_SWITCH T_IDENT T_ASSIGN switch_label_list {
        $$ = new SwitchDeclaration($2, std::move(*$4));
        delete $4;
      }
    ;

switch_label_list
    : T_IDENT {
        $$ = new std::vector<std::string>();
        $$->push_back($1);
      }
    | switch_label_list T_COMMA T_IDENT {
        $$ = $1;
        $$->push_back($3);
      }
    ;

/* ---- Typed parameter list (C-style inline types) ---- */

opt_typed_params
    : /* empty */ { $$ = new std::vector<ParamSpec>(); }
    | T_LPAREN T_RPAREN { $$ = new std::vector<ParamSpec>(); }
    | T_LPAREN typed_param_list T_RPAREN { $$ = $2; }
    ;

typed_param_list
    : type_name T_IDENT {
        $$ = new std::vector<ParamSpec>();
        $$->push_back({$2, $1});
      }
    | type_name T_ARRAY T_IDENT {
        /* Array parameter — passed as pointer */
        $$ = new std::vector<ParamSpec>();
        ParamSpec ps;
        ps.name = $3; ps.type = VarDeclaration::TEXT; ps.isArray = true; ps.arrayElemType = $1;
        $$->push_back(ps);
      }
    | T_REF T_LPAREN T_IDENT T_RPAREN T_IDENT {
        /* REF(Class) parameter — passed as pointer */
        $$ = new std::vector<ParamSpec>();
        $$->push_back({$5, VarDeclaration::TEXT, $3});
      }
    | T_REF T_LPAREN T_IDENT T_RPAREN T_ARRAY T_IDENT {
        /* REF(Class) ARRAY parameter — pointer, REF elements */
        $$ = new std::vector<ParamSpec>();
        ParamSpec ps;
        ps.name = $6; ps.type = VarDeclaration::TEXT; ps.isArray = true;
        ps.arrayElemType = VarDeclaration::TEXT; ps.refClassName = $3;
        $$->push_back(ps);
      }
    | typed_param_list T_COMMA type_name T_IDENT {
        $1->push_back({$4, $3});
        $$ = $1;
      }
    | typed_param_list T_COMMA type_name T_ARRAY T_IDENT {
        ParamSpec ps;
        ps.name = $5; ps.type = VarDeclaration::TEXT; ps.isArray = true; ps.arrayElemType = $3;
        $1->push_back(ps);
        $$ = $1;
      }
    | typed_param_list T_COMMA T_REF T_LPAREN T_IDENT T_RPAREN T_IDENT {
        $1->push_back({$7, VarDeclaration::TEXT, $5});
        $$ = $1;
      }
    | typed_param_list T_COMMA T_REF T_LPAREN T_IDENT T_RPAREN T_ARRAY T_IDENT {
        ParamSpec ps;
        ps.name = $8; ps.type = VarDeclaration::TEXT; ps.isArray = true;
        ps.arrayElemType = VarDeclaration::TEXT; ps.refClassName = $5;
        $1->push_back(ps);
        $$ = $1;
      }
    ;

/* ---- Simula-style parameter specifications ---- */

param_specs
    : /* empty */ {
        $$ = new std::vector<std::pair<int, std::vector<std::string>>>();
      }
    | param_specs type_name ident_list T_SEMI {
        $1->push_back({$2, std::move(*$3)});
        delete $3;
        $$ = $1;
      }
    | param_specs type_name T_ARRAY ident_list T_SEMI {
        /* Array parameter spec: INTEGER ARRAY a; — use code -20 + (elem type * -1000) */
        $1->push_back({-20 - $2 * 1000, std::move(*$4)});
        delete $4;
        $$ = $1;
      }
    | param_specs T_LABEL ident_list T_SEMI {
        /* LABEL parameter spec — encoded as -40 (carries a non-local goto target) */
        $1->push_back({-40, std::move(*$3)});
        delete $3;
        $$ = $1;
      }
    | param_specs T_REF T_LPAREN T_IDENT T_RPAREN ident_list T_SEMI {
        /* REF parameter spec: REF(Class) x; — use type -10, first name = class */
        std::vector<std::string> names;
        names.push_back(std::string($4)); // class name as first element
        for (auto& s : *$6) names.push_back(s); // param names
        $1->push_back({-10, std::move(names)});
        delete $6;
        $$ = $1;
      }
    | param_specs T_REF T_LPAREN T_IDENT T_RPAREN T_ARRAY ident_list T_SEMI {
        /* REF(Class) ARRAY parameter spec — code -11, first name = class */
        std::vector<std::string> names;
        names.push_back(std::string($4));
        for (auto& s : *$7) names.push_back(s);
        $1->push_back({-11, std::move(names)});
        delete $7;
        $$ = $1;
      }
    | param_specs T_PROCEDURE T_IDENT T_SEMI {
        /* Procedure parameter spec: PROCEDURE name; — code -12 (function pointer) */
        auto names = new std::vector<std::string>();
        names->push_back($3);
        $1->push_back({-12, std::move(*names)});
        delete names;
        $$ = $1;
      }
    | param_specs T_PROCEDURE T_IDENT T_IS virtual_is_rhs {
        /* Procedure parameter spec with IS binding — ignored */
        auto names = new std::vector<std::string>();
        names->push_back($3);
        $1->push_back({-12, std::move(*names)});
        delete names;
        $$ = $1;
      }
    | param_specs type_name T_PROCEDURE T_IDENT T_SEMI {
        /* Typed procedure parameter: INTEGER PROCEDURE name; */
        auto names = new std::vector<std::string>();
        names->push_back($4);
        $1->push_back({VarDeclaration::TEXT, std::move(*names)});
        delete names;
        $$ = $1;
      }
    | param_specs T_IDENT ident_list T_SEMI {
        /* VALUE/NAME specs — encode as type -30 (NAME) or -31 (VALUE) for downstream */
        std::string spec($2);
        for (auto& c : spec) c = tolower(c);
        int code = -1; // unknown
        if (spec == "name") code = -30;
        else if (spec == "value") code = -31;
        $1->push_back({code, std::move(*$3)});
        delete $3;
        $$ = $1;
      }
    ;

/* ---- Control flow ---- */

if_stmt
    : T_IF expr T_THEN statement {
        $$ = new IfStatement(ExprPtr($2), StmtPtr($4));
      }
    | T_IF expr T_THEN statement T_ELSE statement {
        $$ = new IfStatement(ExprPtr($2), StmtPtr($4), StmtPtr($6));
      }
    ;

while_stmt
    : T_WHILE expr T_DO statement {
        $$ = new WhileStatement(ExprPtr($2), StmtPtr($4));
      }
    ;

for_stmt
    : T_FOR T_IDENT T_ASSIGN expr T_STEP expr T_UNTIL expr T_DO statement {
        $$ = new ForStatement($2, ExprPtr($4), ExprPtr($6),
                              ExprPtr($8), StmtPtr($10));
      }
    | T_FOR T_IDENT T_ASSIGN expr T_STEP expr T_UNTIL expr T_COMMA expr T_STEP expr T_UNTIL expr T_DO statement {
        /* FOR var := r1, r2 DO body — multi-range form (2 ranges) */
        std::vector<ForMultiRangeStatement::Range> ranges;
        ranges.push_back({ExprPtr($4), ExprPtr($6), ExprPtr($8)});
        ranges.push_back({ExprPtr($10), ExprPtr($12), ExprPtr($14)});
        $$ = new ForMultiRangeStatement($2, std::move(ranges), StmtPtr($16));
      }
    | T_FOR T_IDENT T_ASSIGN arg_list_ne T_DO statement {
        /* FOR var := expr, expr, ... DO body — value list form */
        $$ = new ForListStatement($2, std::move(*$4), StmtPtr($6));
        delete $4;
      }
    ;

/* ---- Procedures ---- */

procedure_decl
    : T_PROCEDURE T_IDENT opt_typed_params T_SEMI statement {
        $$ = new ProcedureDecl($2, false, VarDeclaration::INTEGER,
                               std::move(*$3), StmtPtr($5));
        delete $3;
      }
    | type_name T_PROCEDURE T_IDENT opt_typed_params T_SEMI statement {
        $$ = new ProcedureDecl($3, true, (VarDeclaration::Type)$1,
                               std::move(*$4), StmtPtr($6));
        delete $4;
      }
    | T_PROCEDURE T_IDENT T_LPAREN ident_list T_RPAREN T_SEMI param_specs statement {
        auto params = mergeParams(*$4, *$7);
        $$ = new ProcedureDecl($2, false, VarDeclaration::INTEGER,
                               std::move(params), StmtPtr($8));
        delete $4; delete $7;
      }
    | type_name T_PROCEDURE T_IDENT T_LPAREN ident_list T_RPAREN T_SEMI param_specs statement {
        auto params = mergeParams(*$5, *$8);
        $$ = new ProcedureDecl($3, true, (VarDeclaration::Type)$1,
                               std::move(params), StmtPtr($9));
        delete $5; delete $8;
      }
    | T_REF T_LPAREN T_IDENT T_RPAREN T_PROCEDURE T_IDENT opt_typed_params T_SEMI statement {
        /* REF(Class) PROCEDURE name(...); — returns a pointer */
        $$ = new ProcedureDecl($6, true, VarDeclaration::TEXT,
                               std::move(*$7), StmtPtr($9), $3);
        delete $7;
      }
    | T_REF T_LPAREN T_IDENT T_RPAREN T_PROCEDURE T_IDENT T_LPAREN ident_list T_RPAREN T_SEMI param_specs statement {
        auto params = mergeParams(*$8, *$11);
        $$ = new ProcedureDecl($6, true, VarDeclaration::TEXT,
                               std::move(params), StmtPtr($12), $3);
        delete $8; delete $11;
      }
    ;

/* ---- Classes ---- */

class_decl
    : T_CLASS T_IDENT opt_typed_params T_SEMI class_body {
        $$ = new ClassDecl($2, "", std::move(*$3), std::move(*$5));
        delete $3; delete $5;
      }
    | T_IDENT T_CLASS T_IDENT opt_typed_params T_SEMI class_body {
        $$ = new ClassDecl($3, $1, std::move(*$4), std::move(*$6));
        delete $4; delete $6;
      }
    | T_CLASS T_IDENT T_LPAREN ident_list T_RPAREN T_SEMI param_specs class_body {
        auto params = mergeParams(*$4, *$7);
        $$ = new ClassDecl($2, "", std::move(params), std::move(*$8));
        delete $4; delete $7; delete $8;
      }
    | T_IDENT T_CLASS T_IDENT T_LPAREN ident_list T_RPAREN T_SEMI param_specs class_body {
        auto params = mergeParams(*$5, *$8);
        $$ = new ClassDecl($3, $1, std::move(params), std::move(*$9));
        delete $5; delete $8; delete $9;
      }
    ;

class_body
    : T_BEGIN stmt_list T_END end_names { $$ = $2; }
    | virtual_spec class_body {
        /* Prepend virtual_spec so ClassDecl can see it */
        auto stmts = $2;
        stmts->insert(stmts->begin(), StmtPtr($1));
        $$ = stmts;
      }
    | /* empty — class with no body */ { $$ = new StmtList(); }
    ;

/* ---- VIRTUAL specification (parsed and ignored) ---- */

virtual_spec
    : T_VIRTUAL T_COLON virtual_entries {
        std::vector<std::string> names;
        std::vector<int> rets;
        for (auto& e : *$3) { names.push_back(e.first); rets.push_back(e.second); }
        $$ = new VirtualDecl(std::move(names), std::move(rets));
        delete $3;
      }
    ;

virtual_entries
    : virtual_entry {
        $$ = new std::vector<std::pair<std::string,int>>();
        if ($1) { $$->push_back(*$1); delete $1; }
      }
    | virtual_entries virtual_entry {
        if ($2) { $1->push_back(*$2); delete $2; }
        $$ = $1;
      }
    ;

/* Each entry yields (method name, declared return type). Return type uses
   VarDeclaration::Type values; -1 = untyped/void, -2 = REF. */
virtual_entry
    : T_PROCEDURE T_IDENT T_SEMI { $$ = new std::pair<std::string,int>($2, -1); free($2); }
    | type_name T_PROCEDURE T_IDENT T_SEMI { $$ = new std::pair<std::string,int>($3, $1); free($3); }
    | T_REF T_LPAREN T_IDENT T_RPAREN T_PROCEDURE T_IDENT T_SEMI { $$ = new std::pair<std::string,int>($6, -2); free($3); free($6); }
    /* IS bindings — the return type comes from the bound prototype's RHS */
    | T_PROCEDURE T_IDENT T_IS virtual_is_rhs { $$ = new std::pair<std::string,int>($2, $4); free($2); }
    | type_name T_PROCEDURE T_IDENT T_IS virtual_is_rhs { $$ = new std::pair<std::string,int>($3, $1); free($3); }
    ;

/* The RHS of a VIRTUAL IS binding: a procedure prototype possibly with params.
   We consume tokens permissively until we hit ;; or ;. The value is the
   declared return type (VarDeclaration::Type, -1 void, -2 REF). */
virtual_is_rhs
    : T_PROCEDURE T_IDENT T_SEMI T_SEMI { $$ = -1; }
    | T_PROCEDURE T_IDENT T_SEMI { $$ = -1; }
    | type_name T_PROCEDURE T_IDENT T_SEMI T_SEMI { $$ = $1; }
    | type_name T_PROCEDURE T_IDENT T_SEMI { $$ = $1; }
    | T_PROCEDURE T_IDENT T_LPAREN ident_list T_RPAREN T_SEMI param_specs T_SEMI { $$ = -1; }
    | type_name T_PROCEDURE T_IDENT T_LPAREN ident_list T_RPAREN T_SEMI param_specs T_SEMI { $$ = $1; }
    ;

/* ---- INSPECT/WHEN ---- */

inspect_stmt
    : T_INSPECT expr when_clauses {
        $$ = new InspectStatement(ExprPtr($2), std::move(*$3));
        delete $3;
      }
    | T_INSPECT expr when_clauses T_OTHERWISE statement {
        $$ = new InspectStatement(ExprPtr($2), std::move(*$3), nullptr, StmtPtr($5));
        delete $3;
      }
    | T_INSPECT expr T_DO statement {
        /* INSPECT ref DO stmt — connected statement, skipped when ref is NONE */
        auto clauses = new std::vector<InspectStatement::WhenClause>();
        $$ = new InspectStatement(ExprPtr($2), std::move(*clauses), StmtPtr($4));
        delete clauses;
      }
    | T_INSPECT expr T_DO statement T_OTHERWISE statement {
        /* OTHERWISE runs (unconnected) when ref is NONE */
        auto clauses = new std::vector<InspectStatement::WhenClause>();
        $$ = new InspectStatement(ExprPtr($2), std::move(*clauses), StmtPtr($4),
                                  StmtPtr($6));
        delete clauses;
      }
    ;

when_clauses
    : T_WHEN T_IDENT T_DO statement {
        $$ = new std::vector<InspectStatement::WhenClause>();
        $$->push_back({$2, StmtPtr($4)});
      }
    | when_clauses T_WHEN T_IDENT T_DO statement {
        $1->push_back({$3, StmtPtr($5)});
        $$ = $1;
      }
    ;

/* ---- Coroutines ---- */

detach_stmt
    : T_DETACH { $$ = new DetachStatement(); }
    | T_INNER  { $$ = new InnerStatement(); }
    ;

activate_stmt
    : T_ACTIVATE expr {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::DIRECT,
                                   nullptr, nullptr, false, false);
      }
    | T_ACTIVATE expr T_AT expr {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::AT,
                                   ExprPtr($4), nullptr, false, false);
      }
    | T_ACTIVATE expr T_AT expr T_PRIOR {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::AT,
                                   ExprPtr($4), nullptr, true, false);
      }
    | T_ACTIVATE expr T_DELAY expr {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::DELAY,
                                   ExprPtr($4), nullptr, false, false);
      }
    | T_ACTIVATE expr T_DELAY expr T_PRIOR {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::DELAY,
                                   ExprPtr($4), nullptr, true, false);
      }
    | T_ACTIVATE expr T_BEFORE expr {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::BEFORE,
                                   nullptr, ExprPtr($4), false, false);
      }
    | T_ACTIVATE expr T_AFTER expr {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::AFTER,
                                   nullptr, ExprPtr($4), false, false);
      }
    | T_REACTIVATE expr {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::DIRECT,
                                   nullptr, nullptr, false, true);
      }
    | T_REACTIVATE expr T_AT expr {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::AT,
                                   ExprPtr($4), nullptr, false, true);
      }
    | T_REACTIVATE expr T_AT expr T_PRIOR {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::AT,
                                   ExprPtr($4), nullptr, true, true);
      }
    | T_REACTIVATE expr T_DELAY expr {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::DELAY,
                                   ExprPtr($4), nullptr, false, true);
      }
    | T_REACTIVATE expr T_DELAY expr T_PRIOR {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::DELAY,
                                   ExprPtr($4), nullptr, true, true);
      }
    | T_REACTIVATE expr T_BEFORE expr {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::BEFORE,
                                   nullptr, ExprPtr($4), false, true);
      }
    | T_REACTIVATE expr T_AFTER expr {
        $$ = new ActivateStatement(ExprPtr($2), ActivateStatement::AFTER,
                                   nullptr, ExprPtr($4), false, true);
      }
    ;

resume_stmt
    : T_RESUME T_LPAREN expr T_RPAREN {
        $$ = new ResumeStatement(ExprPtr($3));
      }
    ;

call_stmt
    : T_CALL T_LPAREN expr T_RPAREN {
        $$ = new CallStatement(ExprPtr($3));
      }
    ;

/* ---- I/O ---- */

outint_stmt
    : T_OUTINT T_LPAREN expr T_COMMA expr T_RPAREN {
        $$ = new OutIntStatement(ExprPtr($3), ExprPtr($5));
      }
    ;

outreal_stmt
    : T_OUTREAL T_LPAREN expr T_COMMA expr T_COMMA expr T_RPAREN {
        $$ = new OutRealStatement(ExprPtr($3), ExprPtr($5), ExprPtr($7));
      }
    ;

outfix_stmt
    : T_OUTFIX T_LPAREN expr T_COMMA expr T_COMMA expr T_RPAREN {
        $$ = new OutFixStatement(ExprPtr($3), ExprPtr($5), ExprPtr($7));
      }
    ;

outtext_stmt
    : T_OUTTEXT T_LPAREN expr T_RPAREN {
        $$ = new OutTextStatement(ExprPtr($3));
      }
    ;

outimage_stmt
    : T_OUTIMAGE { $$ = new OutImageStatement(); }
    ;

inimage_stmt
    : T_INIMAGE { $$ = new InImageStatement(); }
    ;

/* ---- Expression statement and unified assignment ---- */

expr_stmt
    : postfix {
        $$ = new ExprStatement(ExprPtr($1));
      }
    | postfix T_ASSIGN postfix T_ASSIGN expr {
        /* Chained assignment X1 := X2 := E: evaluate E once (into X2), then read
           X2 into X1. Any left-part kind (variable, array element, field) works. */
        StmtList stmts;
        Expression* readX2 = cloneExpr($3);        /* fresh read of X2 */
        stmts.push_back(StmtPtr(lowerAssign($3, $5)));           /* X2 := E */
        if (readX2)
            stmts.push_back(StmtPtr(lowerAssign($1, readX2)));  /* X1 := X2 */
        else { delete $1; }
        $$ = new CompoundStmt(std::move(stmts));
      }
    | postfix T_ASSIGN expr {
        /* Determine assignment type from LHS node */
        Identifier* ident = dynamic_cast<Identifier*>($1);
        if (ident) {
            $$ = new Assignment(ident->name, ExprPtr($3));
            delete $1;
        } else {
            ProcedureCall* call = dynamic_cast<ProcedureCall*>($1);
            if (call) {
                /* a(i) := expr or a(i,j) := expr -> ArrayAssignment */
                ExprPtr idx(call->args.empty() ? nullptr : call->args[0].release());
                ExprPtr idx2(call->args.size() >= 2 ? call->args[1].release() : nullptr);
                ExprPtr idx3(call->args.size() >= 3 ? call->args[2].release() : nullptr);
                $$ = new ArrayAssignment(call->name, std::move(idx), ExprPtr($3), false, std::move(idx2), std::move(idx3));
                delete $1;
            } else {
                MemberAccess* ma = dynamic_cast<MemberAccess*>($1);
                if (ma) {
                    $$ = new MemberAssignment(ExprPtr(ma->object.release()),
                                              ma->member, ExprPtr($3), false);
                    delete $1;
                } else {
                    MethodCall* mc = dynamic_cast<MethodCall*>($1);
                    if (mc) {
                        /* obj.data(i) := expr — member array assignment;
                           also obj.SUB(start,len) := expr — write-through view */
                        ExprPtr idx(mc->args.empty() ? nullptr : mc->args[0].release());
                        ExprPtr idx2(mc->args.size() >= 2 ? mc->args[1].release() : nullptr);
                        $$ = new MemberArrayAssignment(ExprPtr(mc->object.release()),
                                                       mc->method, std::move(idx),
                                                       ExprPtr($3), false, std::move(idx2));
                        delete $1;
                    } else {
                        yyerror("invalid left-hand side of assignment");
                        $$ = new ExprStatement(ExprPtr($1));
                        delete $3;
                    }
                }
            }
        }
        if ($$) $$->line = @1.first_line;
      }
    | postfix T_REFASSIGN expr {
        Identifier* ident = dynamic_cast<Identifier*>($1);
        if (ident) {
            $$ = new RefAssignment(ident->name, ExprPtr($3));
            delete $1;
        } else {
            ProcedureCall* call = dynamic_cast<ProcedureCall*>($1);
            if (call) {
                /* a(i) :- expr or a(i,j) :- expr -> ArrayAssignment (ref-assign) */
                ExprPtr idx(call->args.empty() ? nullptr : call->args[0].release());
                ExprPtr idx2(call->args.size() >= 2 ? call->args[1].release() : nullptr);
                ExprPtr idx3(call->args.size() >= 3 ? call->args[2].release() : nullptr);
                $$ = new ArrayAssignment(call->name, std::move(idx), ExprPtr($3), true, std::move(idx2), std::move(idx3));
                delete $1;
            } else {
                MemberAccess* ma = dynamic_cast<MemberAccess*>($1);
                if (ma) {
                    $$ = new MemberAssignment(ExprPtr(ma->object.release()),
                                              ma->member, ExprPtr($3), true);
                    delete $1;
                } else {
                    MethodCall* mc = dynamic_cast<MethodCall*>($1);
                    if (mc) {
                        /* obj.data(i) :- expr — member array element ref-assignment */
                        ExprPtr idx(mc->args.empty() ? nullptr : mc->args[0].release());
                        ExprPtr idx2(mc->args.size() >= 2 ? mc->args[1].release() : nullptr);
                        $$ = new MemberArrayAssignment(ExprPtr(mc->object.release()),
                                                       mc->method, std::move(idx),
                                                       ExprPtr($3), true, std::move(idx2));
                        delete $1;
                    } else {
                        yyerror("invalid left-hand side of reference assignment");
                        $$ = new ExprStatement(ExprPtr($1));
                        delete $3;
                    }
                }
            }
        }
        if ($$) $$->line = @1.first_line;
      }
    ;


/* ---- Expressions ---- */

/* Boolean operator precedence, tightest to loosest (Simula Standard Boolean
   grammar): NOT (in `unary`) > AND > OR > IMP > EQV > AND THEN > OR ELSE.
   Each level is left-associative. */
expr
    : or_else_expr
    ;
or_else_expr
    : and_then_expr
    | or_else_expr T_ORELSE and_then_expr {
        $$ = new BinaryOp(BinaryOp::ORELSE, ExprPtr($1), ExprPtr($3));
      }
    ;
and_then_expr
    : eqv_expr
    | and_then_expr T_ANDTHEN eqv_expr {
        $$ = new BinaryOp(BinaryOp::ANDTHEN, ExprPtr($1), ExprPtr($3));
      }
    ;
eqv_expr
    : imp_expr
    | eqv_expr T_EQV imp_expr {
        /* EQV = logical equivalence = NOT (a XOR b) */
        $$ = new UnaryOp(UnaryOp::NOT, ExprPtr(
            new BinaryOp(BinaryOp::NE, ExprPtr($1), ExprPtr($3))));
      }
    ;
imp_expr
    : or_expr
    | imp_expr T_IMP or_expr {
        /* IMP = logical implication = NOT a OR b */
        $$ = new BinaryOp(BinaryOp::OR,
            ExprPtr(new UnaryOp(UnaryOp::NOT, ExprPtr($1))), ExprPtr($3));
      }
    ;
or_expr
    : and_expr
    | or_expr T_OR and_expr {
        $$ = new BinaryOp(BinaryOp::OR, ExprPtr($1), ExprPtr($3));
      }
    ;
and_expr
    : comparison
    | and_expr T_AND comparison {
        $$ = new BinaryOp(BinaryOp::AND, ExprPtr($1), ExprPtr($3));
      }
    ;

comparison
    : additive
    | additive T_EQ additive {
        $$ = new BinaryOp(BinaryOp::EQ, ExprPtr($1), ExprPtr($3));
      }
    | additive T_NE additive {
        $$ = new BinaryOp(BinaryOp::NE, ExprPtr($1), ExprPtr($3));
      }
    | additive T_LT additive {
        $$ = new BinaryOp(BinaryOp::LT, ExprPtr($1), ExprPtr($3));
      }
    | additive T_LE additive {
        $$ = new BinaryOp(BinaryOp::LE, ExprPtr($1), ExprPtr($3));
      }
    | additive T_GT additive {
        $$ = new BinaryOp(BinaryOp::GT, ExprPtr($1), ExprPtr($3));
      }
    | additive T_GE additive {
        $$ = new BinaryOp(BinaryOp::GE, ExprPtr($1), ExprPtr($3));
      }
    | additive T_REFEQ additive {
        /* == reference identity: pointer identity for REF, frame+start+length
           identity for TEXT (distinct from = content comparison). */
        $$ = new BinaryOp(BinaryOp::REF_EQ, ExprPtr($1), ExprPtr($3));
      }
    | additive T_REFNE additive {
        /* =/= reference non-identity */
        $$ = new BinaryOp(BinaryOp::REF_NE, ExprPtr($1), ExprPtr($3));
      }
    | additive T_IS T_IDENT {
        $$ = new IsExpression(ExprPtr($1), $3);
        $$->line = @1.first_line;
      }
    | additive T_IN T_IDENT {
        $$ = new InExpression(ExprPtr($1), $3);
        $$->line = @1.first_line;
      }
    ;

additive
    : multiplicative
    | additive T_PLUS multiplicative {
        $$ = new BinaryOp(BinaryOp::ADD, ExprPtr($1), ExprPtr($3));
      }
    | additive T_MINUS multiplicative {
        $$ = new BinaryOp(BinaryOp::SUB, ExprPtr($1), ExprPtr($3));
      }
    | additive T_AMP multiplicative {
        $$ = new BinaryOp(BinaryOp::CONCAT, ExprPtr($1), ExprPtr($3));
      }
    ;

multiplicative
    : unary
    | multiplicative T_STAR unary {
        $$ = new BinaryOp(BinaryOp::MUL, ExprPtr($1), ExprPtr($3));
      }
    | multiplicative T_SLASH unary {
        $$ = new BinaryOp(BinaryOp::DIV, ExprPtr($1), ExprPtr($3));
        $$->line = @2.first_line;
      }
    | multiplicative T_IDIV unary {
        $$ = new BinaryOp(BinaryOp::IDIV, ExprPtr($1), ExprPtr($3));
        $$->line = @2.first_line;
      }
    ;

/* Unary minus / NOT bind LOOSER than ** so -2**2 = -(2**2), and ** is left-
   associative so 2**3**2 = (2**3)**2, per the ALGOL/Simula arithmetic grammar. */
unary
    : power
    | T_MINUS unary {
        $$ = new UnaryOp(UnaryOp::NEG, ExprPtr($2));
      }
    | T_NOT unary {
        $$ = new UnaryOp(UnaryOp::NOT, ExprPtr($2));
      }
    ;

power
    : postfix
    | power T_POWER postfix {
        $$ = new BinaryOp(BinaryOp::POWER, ExprPtr($1), ExprPtr($3));
      }
    ;

postfix
    : primary
    | T_IDENT T_LPAREN arg_list T_RPAREN {
        $$ = new ProcedureCall($1, std::move(*$3));
        $$->line = @1.first_line;
        delete $3;
      }
    | postfix T_DOT T_IDENT {
        $$ = new MemberAccess(ExprPtr($1), $3);
        $$->line = @1.first_line;
      }
    | postfix T_DOT T_IDENT T_LPAREN arg_list T_RPAREN {
        $$ = new MethodCall(ExprPtr($1), $3, std::move(*$5));
        $$->line = @1.first_line;
        delete $5;
      }
    | postfix T_DOT io_keyword T_LPAREN arg_list T_RPAREN {
        /* Allow I/O keywords as method names: obj.OutFix(...) etc. */
        $$ = new MethodCall(ExprPtr($1), $3, std::move(*$5));
        $$->line = @1.first_line;
        delete $5;
      }
    | postfix T_DOT io_keyword {
        $$ = new MemberAccess(ExprPtr($1), $3);
        $$->line = @1.first_line;
      }
    | postfix T_QUA T_IDENT {
        /* QUA type qualification — changes class resolution for member access */
        $$ = new QuaExpression(ExprPtr($1), $3);
        $$->line = @1.first_line;
      }
    ;

/* Keywords that can appear as member names after dot */
io_keyword
    : T_OUTINT    { $$ = (char*)"outint"; }
    | T_OUTREAL   { $$ = (char*)"outreal"; }
    | T_OUTFIX    { $$ = (char*)"outfix"; }
    | T_OUTTEXT   { $$ = (char*)"outtext"; }
    | T_OUTIMAGE  { $$ = (char*)"outimage"; }
    | T_ININT     { $$ = (char*)"inint"; }
    | T_INREAL    { $$ = (char*)"inreal"; }
    | T_INCHAR    { $$ = (char*)"inchar"; }
    | T_INIMAGE   { $$ = (char*)"inimage"; }
    | T_LASTITEM  { $$ = (char*)"lastitem"; }
    ;

primary
    : T_INTLIT    { $$ = new IntegerLiteral($1); $$->line = @1.first_line; }
    | T_REALLIT   { $$ = new RealLiteral($1); $$->line = @1.first_line; }
    | T_TEXTLIT   { $$ = new TextLiteral($1); $$->line = @1.first_line; }
    | T_CHARLIT   { $$ = new CharLiteral((char)$1); $$->line = @1.first_line; }
    | T_TRUE      { $$ = new BooleanLiteral(true); $$->line = @1.first_line; }
    | T_FALSE     { $$ = new BooleanLiteral(false); $$->line = @1.first_line; }
    | T_NONE      { $$ = new NoneLiteral(); $$->line = @1.first_line; }
    | T_NOTEXT    { $$ = new NoneLiteral(); $$->line = @1.first_line; }
    | T_THIS      { $$ = new ThisExpression(); $$->line = @1.first_line; }
    | T_THIS T_IDENT { $$ = new ThisExpression(); $$->line = @1.first_line; } /* THIS ClassName — type qual ignored */
    | T_IDENT     { $$ = new Identifier($1); $$->line = @1.first_line; }
    | T_NEW T_IDENT T_LPAREN arg_list T_RPAREN {
        $$ = new NewExpression($2, std::move(*$4));
        $$->line = @1.first_line;
        delete $4;
      }
    | T_NEW T_IDENT {
        $$ = new NewExpression($2, ExprList());
        $$->line = @1.first_line;
      }
    | T_LPAREN expr T_RPAREN { $$ = $2; }
    | T_ININT     { $$ = new InIntExpression(); $$->line = @1.first_line; }
    | T_INREAL    { $$ = new InRealExpression(); $$->line = @1.first_line; }
    | T_INCHAR    { $$ = new InCharExpression(); $$->line = @1.first_line; }
    | T_LASTITEM  { $$ = new Identifier("__lastitem"); $$->line = @1.first_line; }
    | T_IF expr T_THEN expr T_ELSE expr {
        $$ = new ConditionalExpr(ExprPtr($2), ExprPtr($4), ExprPtr($6));
        $$->line = @1.first_line;
      }
    ;

arg_list
    : /* empty */ { $$ = new ExprList(); }
    | arg_list_ne
    ;

arg_list_ne
    : expr {
        $$ = new ExprList();
        $$->push_back(ExprPtr($1));
      }
    | arg_list_ne T_COMMA expr {
        $1->push_back(ExprPtr($3));
        $$ = $1;
      }
    ;

%%

void yyerror(const char* s) {
    fprintf(stderr, "Parse error at line %d: %s\n", yylineno, s);
}
