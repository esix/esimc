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
    std::vector<ParamSpec> result;
    for (auto& n : names) {
        int type = VarDeclaration::INTEGER; // default
        /* Find this name in the specs */
        for (auto& sp : specs) {
            for (auto& sn : sp.second) {
                /* Case-insensitive comparison */
                if (sn.size() == n.size()) {
                    bool match = true;
                    for (size_t i = 0; i < n.size(); i++) {
                        if (tolower((unsigned char)sn[i]) != tolower((unsigned char)n[i])) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        type = sp.first;
                        goto found;
                    }
                }
            }
        }
        found:
        result.push_back({n, type});
    }
    return result;
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
}

/* Keywords */
%token T_BEGIN T_END
%token T_INTEGER T_REAL T_BOOLEAN T_TEXT T_CHARACTER T_SHORT T_LONG
%token T_IF T_THEN T_ELSE
%token T_WHILE T_DO
%token T_FOR T_STEP T_UNTIL
%token T_TRUE T_FALSE
%token T_NOT T_AND T_OR
%token T_PROCEDURE T_CLASS T_NEW T_THIS
%token T_REF T_NONE T_IS T_IN
%token T_VIRTUAL
%token T_INSPECT T_WHEN T_OTHERWISE
%token T_DETACH T_RESUME T_CALL
%token T_ARRAY T_LABEL T_GOTO
%token T_NOTEXT
/* T_VALUE and T_NAME removed — handled as identifiers contextually */
%token T_OUTINT T_OUTREAL T_OUTFIX T_OUTTEXT T_OUTIMAGE
%token T_ININT T_INREAL T_INCHAR T_INIMAGE

/* Operators and punctuation */
%token T_ASSIGN T_REFASSIGN T_COLON T_SEMI T_COMMA T_DOT T_AMP
%token T_LPAREN T_RPAREN
%token T_PLUS T_MINUS T_STAR T_SLASH T_IDIV
%token T_EQ T_NE T_LT T_LE T_GT T_GE

/* Literals */
%token <ival> T_INTLIT T_CHARLIT
%token <dval> T_REALLIT
%token <sval> T_IDENT T_TEXTLIT

/* Non-terminal types */
%type <expr> expr comparison additive multiplicative unary postfix primary
%type <stmt> statement block
%type <stmt> declaration array_decl ref_declaration
%type <stmt> label_decl labeled_stmt goto_stmt
%type <stmt> if_stmt while_stmt for_stmt
%type <stmt> procedure_decl class_decl
%type <stmt> inspect_stmt
%type <stmt> detach_stmt resume_stmt call_stmt
%type <stmt> outint_stmt outreal_stmt outfix_stmt outtext_stmt outimage_stmt inimage_stmt
%type <stmt> expr_stmt
%type <stmtlist> stmt_list class_body
%type <exprlist> arg_list arg_list_ne
%type <namelist> ident_list
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
    ;

block
    : T_BEGIN stmt_list T_END end_names {
        $$ = new Block(std::move(*$2));
        delete $2;
      }
    ;

end_names
    : /* empty */
    | end_names T_IDENT  /* silently consume identifiers after END */
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
    ;

statement
    : declaration
    | array_decl
    | ref_declaration
    | label_decl
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
    | outint_stmt
    | outreal_stmt
    | outfix_stmt
    | outtext_stmt
    | outimage_stmt
    | inimage_stmt
    | block
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
    ;

ref_declaration
    : T_REF T_LPAREN T_IDENT T_RPAREN T_IDENT {
        $$ = new RefDeclaration($3, $5);
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
    | typed_param_list T_COMMA type_name T_IDENT {
        $1->push_back({$4, $3});
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
    | param_specs T_IDENT ident_list T_SEMI {
        /* Handles VALUE/NAME specs: identifier "value" or "name" followed by ident_list.
           These don't change types, so use -1 as marker to skip in mergeParams. */
        $1->push_back({-1, std::move(*$3)});
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
    ;

/* ---- INSPECT/WHEN ---- */

inspect_stmt
    : T_INSPECT expr when_clauses {
        $$ = new InspectStatement(ExprPtr($2), std::move(*$3));
        delete $3;
      }
    | T_INSPECT expr when_clauses T_OTHERWISE statement {
        $$ = new InspectStatement(ExprPtr($2), std::move(*$3), StmtPtr($5));
        delete $3;
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
    | postfix T_ASSIGN expr {
        /* Determine assignment type from LHS node */
        Identifier* ident = dynamic_cast<Identifier*>($1);
        if (ident) {
            $$ = new Assignment(ident->name, ExprPtr($3));
            delete $1;
        } else {
            ProcedureCall* call = dynamic_cast<ProcedureCall*>($1);
            if (call) {
                /* a(i) := expr  ->  ArrayAssignment */
                ExprPtr idx(call->args.empty() ? nullptr : call->args[0].release());
                $$ = new ArrayAssignment(call->name, std::move(idx), ExprPtr($3));
                delete $1;
            } else {
                MemberAccess* ma = dynamic_cast<MemberAccess*>($1);
                if (ma) {
                    $$ = new MemberAssignment(ExprPtr(ma->object.release()),
                                              ma->member, ExprPtr($3));
                    delete $1;
                } else {
                    /* Fallback: treat as assignment error */
                    yyerror("invalid left-hand side of assignment");
                    $$ = new ExprStatement(ExprPtr($1));
                    delete $3;
                }
            }
        }
      }
    | postfix T_REFASSIGN expr {
        Identifier* ident = dynamic_cast<Identifier*>($1);
        if (ident) {
            $$ = new RefAssignment(ident->name, ExprPtr($3));
            delete $1;
        } else {
            yyerror("invalid left-hand side of reference assignment");
            $$ = new ExprStatement(ExprPtr($1));
            delete $3;
        }
      }
    ;

/* ---- Expressions ---- */

expr
    : comparison
    | expr T_AND comparison {
        $$ = new BinaryOp(BinaryOp::AND, ExprPtr($1), ExprPtr($3));
      }
    | expr T_OR comparison {
        $$ = new BinaryOp(BinaryOp::OR, ExprPtr($1), ExprPtr($3));
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
    | additive T_IS T_IDENT {
        $$ = new IsExpression(ExprPtr($1), $3);
      }
    | additive T_IN T_IDENT {
        $$ = new InExpression(ExprPtr($1), $3);
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
      }
    | multiplicative T_IDIV unary {
        $$ = new BinaryOp(BinaryOp::IDIV, ExprPtr($1), ExprPtr($3));
      }
    ;

unary
    : postfix
    | T_MINUS postfix {
        $$ = new UnaryOp(UnaryOp::NEG, ExprPtr($2));
      }
    | T_NOT postfix {
        $$ = new UnaryOp(UnaryOp::NOT, ExprPtr($2));
      }
    ;

postfix
    : primary
    | T_IDENT T_LPAREN arg_list T_RPAREN {
        $$ = new ProcedureCall($1, std::move(*$3));
        delete $3;
      }
    | postfix T_DOT T_IDENT {
        $$ = new MemberAccess(ExprPtr($1), $3);
      }
    | postfix T_DOT T_IDENT T_LPAREN arg_list T_RPAREN {
        $$ = new MethodCall(ExprPtr($1), $3, std::move(*$5));
        delete $5;
      }
    ;

primary
    : T_INTLIT    { $$ = new IntegerLiteral($1); }
    | T_REALLIT   { $$ = new RealLiteral($1); }
    | T_TEXTLIT   { $$ = new TextLiteral($1); }
    | T_CHARLIT   { $$ = new CharLiteral((char)$1); }
    | T_TRUE      { $$ = new BooleanLiteral(true); }
    | T_FALSE     { $$ = new BooleanLiteral(false); }
    | T_NONE      { $$ = new NoneLiteral(); }
    | T_NOTEXT    { $$ = new NoneLiteral(); }
    | T_THIS      { $$ = new ThisExpression(); }
    | T_IDENT     { $$ = new Identifier($1); }
    | T_NEW T_IDENT T_LPAREN arg_list T_RPAREN {
        $$ = new NewExpression($2, std::move(*$4));
        delete $4;
      }
    | T_NEW T_IDENT {
        $$ = new NewExpression($2, ExprList());
      }
    | T_LPAREN expr T_RPAREN { $$ = $2; }
    | T_ININT     { $$ = new InIntExpression(); }
    | T_INREAL    { $$ = new InRealExpression(); }
    | T_INCHAR    { $$ = new InCharExpression(); }
    | T_IF expr T_THEN expr T_ELSE primary {
        $$ = new ConditionalExpr(ExprPtr($2), ExprPtr($4), ExprPtr($6));
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
