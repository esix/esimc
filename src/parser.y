%{
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    std::vector<ParamSpec>* paramlist;
    std::vector<InspectStatement::WhenClause>* whenclauses;
    int vartype;
}

/* Keywords */
%token T_BEGIN T_END
%token T_INTEGER T_REAL T_BOOLEAN T_TEXT
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
%token T_OUTINT T_OUTREAL T_OUTTEXT T_OUTIMAGE

/* Operators and punctuation */
%token T_ASSIGN T_REFASSIGN T_SEMI T_COMMA T_DOT
%token T_LPAREN T_RPAREN
%token T_PLUS T_MINUS T_STAR T_SLASH T_IDIV
%token T_EQ T_NE T_LT T_LE T_GT T_GE

/* Literals */
%token <ival> T_INTLIT
%token <dval> T_REALLIT
%token <sval> T_IDENT T_TEXTLIT

/* Non-terminal types */
%type <expr> expr comparison additive multiplicative unary postfix primary
%type <stmt> statement block assignment if_stmt while_stmt for_stmt
%type <stmt> outint_stmt outreal_stmt outtext_stmt outimage_stmt
%type <stmt> declaration ref_declaration ref_assignment
%type <stmt> procedure_decl class_decl
%type <stmt> member_assignment
%type <stmt> inspect_stmt
%type <stmt> detach_stmt resume_stmt call_stmt
%type <stmt> expr_stmt
%type <stmtlist> stmt_list class_body_stmts
%type <exprlist> arg_list arg_list_ne
%type <paramlist> typed_param_list opt_typed_params
%type <vartype> type_name
%type <whenclauses> when_clauses

/* Dangling ELSE */
%nonassoc T_THEN
%nonassoc T_ELSE

%start program

%%

program
    : block { programRoot = new Program(StmtPtr($1)); }
    ;

block
    : T_BEGIN stmt_list T_END {
        $$ = new Block(std::move(*$2));
        delete $2;
      }
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
    | ref_declaration
    | assignment
    | member_assignment
    | ref_assignment
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
    | outtext_stmt
    | outimage_stmt
    | block
    | expr_stmt
    ;

/* ---- Declarations ---- */

declaration
    : type_name T_IDENT {
        $$ = new VarDeclaration((VarDeclaration::Type)$1, $2);
        free($2);
      }
    ;

ref_declaration
    : T_REF T_LPAREN T_IDENT T_RPAREN T_IDENT {
        $$ = new RefDeclaration($3, $5);
        free($3); free($5);
      }
    ;

type_name
    : T_INTEGER { $$ = VarDeclaration::INTEGER; }
    | T_REAL    { $$ = VarDeclaration::REAL; }
    | T_BOOLEAN { $$ = VarDeclaration::BOOLEAN; }
    | T_TEXT    { $$ = VarDeclaration::TEXT; }
    ;

/* ---- Typed parameter list (C-style inline types) ---- */

opt_typed_params
    : /* empty */ { $$ = new std::vector<ParamSpec>(); }
    | T_LPAREN typed_param_list T_RPAREN { $$ = $2; }
    ;

typed_param_list
    : type_name T_IDENT {
        $$ = new std::vector<ParamSpec>();
        $$->push_back({$2, $1});
        free($2);
      }
    | typed_param_list T_COMMA type_name T_IDENT {
        $1->push_back({$4, $3});
        free($4);
        $$ = $1;
      }
    ;

/* ---- Assignment ---- */

assignment
    : T_IDENT T_ASSIGN expr {
        $$ = new Assignment($1, ExprPtr($3));
        free($1);
      }
    ;

member_assignment
    : postfix T_DOT T_IDENT T_ASSIGN expr {
        $$ = new MemberAssignment(ExprPtr($1), $3, ExprPtr($5));
        free($3);
      }
    ;

ref_assignment
    : T_IDENT T_REFASSIGN expr {
        $$ = new RefAssignment($1, ExprPtr($3));
        free($1);
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
        $$ = new ForStatement($2, ExprPtr($4), ExprPtr($6), ExprPtr($8), StmtPtr($10));
        free($2);
      }
    ;

/* ---- Procedures ---- */

procedure_decl
    : T_PROCEDURE T_IDENT opt_typed_params T_SEMI statement {
        $$ = new ProcedureDecl($2, false, VarDeclaration::INTEGER,
                               std::move(*$3), StmtPtr($5));
        free($2); delete $3;
      }
    | type_name T_PROCEDURE T_IDENT opt_typed_params T_SEMI statement {
        $$ = new ProcedureDecl($3, true, (VarDeclaration::Type)$1,
                               std::move(*$4), StmtPtr($6));
        free($3); delete $4;
      }
    ;

/* ---- Classes ---- */

class_decl
    : T_CLASS T_IDENT opt_typed_params T_SEMI class_body_stmts {
        $$ = new ClassDecl($2, "", std::move(*$3), std::move(*$5));
        free($2); delete $3; delete $5;
      }
    | T_IDENT T_CLASS T_IDENT opt_typed_params T_SEMI class_body_stmts {
        $$ = new ClassDecl($3, $1, std::move(*$4), std::move(*$6));
        free($1); free($3); delete $4; delete $6;
      }
    ;

class_body_stmts
    : T_BEGIN stmt_list T_END { $$ = $2; }
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
        free($2);
      }
    | when_clauses T_WHEN T_IDENT T_DO statement {
        $1->push_back({$3, StmtPtr($5)});
        free($3);
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

outtext_stmt
    : T_OUTTEXT T_LPAREN expr T_RPAREN {
        $$ = new OutTextStatement(ExprPtr($3));
      }
    ;

outimage_stmt
    : T_OUTIMAGE { $$ = new OutImageStatement(); }
    ;

expr_stmt
    : postfix {
        $$ = new ExprStatement(ExprPtr($1));
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
        free($3);
      }
    | additive T_IN T_IDENT {
        $$ = new InExpression(ExprPtr($1), $3);
        free($3);
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
        free($1); delete $3;
      }
    | postfix T_DOT T_IDENT {
        $$ = new MemberAccess(ExprPtr($1), $3);
        free($3);
      }
    | postfix T_DOT T_IDENT T_LPAREN arg_list T_RPAREN {
        $$ = new MethodCall(ExprPtr($1), $3, std::move(*$5));
        free($3); delete $5;
      }
    ;

primary
    : T_INTLIT   { $$ = new IntegerLiteral($1); }
    | T_REALLIT  { $$ = new RealLiteral($1); }
    | T_TEXTLIT  { $$ = new TextLiteral($1); free($1); }
    | T_TRUE     { $$ = new BooleanLiteral(true); }
    | T_FALSE    { $$ = new BooleanLiteral(false); }
    | T_NONE     { $$ = new NoneLiteral(); }
    | T_THIS     { $$ = new ThisExpression(); }
    | T_IDENT    { $$ = new Identifier($1); free($1); }
    | T_NEW T_IDENT T_LPAREN arg_list T_RPAREN {
        $$ = new NewExpression($2, std::move(*$4));
        free($2); delete $4;
      }
    | T_NEW T_IDENT {
        $$ = new NewExpression($2, ExprList());
        free($2);
      }
    | T_LPAREN expr T_RPAREN { $$ = $2; }
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
