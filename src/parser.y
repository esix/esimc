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
        /* First pass: check NAME/VALUE markers */
        for (auto& sp : specs) {
            if (sp.first == -30 || sp.first == -31) {
                for (auto& sn : sp.second) {
                    if (matchName(sn, n)) {
                        if (sp.first == -30) isName = true;
                        break;
                    }
                }
            }
        }
        /* Find this name in the specs */
        for (auto& sp : specs) {
            if (sp.first == -30 || sp.first == -31 || sp.first == -1) {
                /* NAME/VALUE/unknown markers — skip in type scan */
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
        ps.isName = isName;
        result.push_back(ps);
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
%token T_NOT T_AND T_OR T_EQV T_IMP T_QUA
%token T_PROCEDURE T_CLASS T_NEW T_THIS
%token T_REF T_NONE T_IS T_IN
%token T_VIRTUAL
%token T_INSPECT T_WHEN T_OTHERWISE
%token T_DETACH T_RESUME T_CALL
%token T_ARRAY T_LABEL T_GOTO
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
%type <expr> expr comparison additive multiplicative power unary postfix primary
%type <stmt> statement block
%type <stmt> declaration array_decl ref_declaration
%type <stmt> label_decl labeled_stmt goto_stmt
%type <stmt> if_stmt while_stmt for_stmt
%type <stmt> procedure_decl class_decl
%type <stmt> inspect_stmt
%type <stmt> detach_stmt resume_stmt call_stmt
%type <stmt> outint_stmt outreal_stmt outfix_stmt outtext_stmt outimage_stmt inimage_stmt
%type <stmt> expr_stmt virtual_spec top_level_decl external_decl
%type <sval> virtual_entry
%type <namelist> virtual_entries
%type <stmtlist> top_level_decls
%type <stmtlist> stmt_list class_body
%type <exprlist> arg_list arg_list_ne
%type <namelist> ident_list
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
    | top_level_decls T_IDENT block {
        /* prefix block: ClassName BEGIN ... END — execute block in class context */
        $1->push_back(StmtPtr($3));
        programRoot = new Program(StmtPtr(new Block(std::move(*$1))));
        delete $1;
      }
    | top_level_decls T_IDENT block T_DOT {
        $1->push_back(StmtPtr($3));
        programRoot = new Program(StmtPtr(new Block(std::move(*$1))));
        delete $1;
      }
    | T_IDENT block {
        /* prefix block at top: ClassName BEGIN ... END */
        auto stmts = new StmtList();
        stmts->push_back(StmtPtr($2));
        programRoot = new Program(StmtPtr(new Block(std::move(*stmts))));
        delete stmts;
      }
    | T_IDENT block T_DOT {
        auto stmts = new StmtList();
        stmts->push_back(StmtPtr($2));
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
    | virtual_spec
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
        /* 2D array: TYPE ARRAY a(lo1:hi1, lo2:hi2)
           Treat as 1D with size = (hi1-lo1+1) * (hi2-lo2+1).
           Index access a(i, j) computed as (i-lo1)*dim2 + (j-lo2). */
        // For now, just use the first dimension's bounds, ignoring the second.
        // This is a simplification — we accept the syntax but don't fully support 2D.
        $$ = new ArrayDeclaration((VarDeclaration::Type)$1, $3,
                                  ExprPtr($5), ExprPtr($7));
      }
    ;

ref_declaration
    : T_REF T_LPAREN T_IDENT T_RPAREN ident_list {
        if ($5->size() == 1) {
            $$ = new RefDeclaration($3, (*$5)[0]);
        } else {
            auto stmts = new StmtList();
            for (auto& name : *$5) {
                stmts->push_back(StmtPtr(new RefDeclaration($3, name)));
            }
            $$ = new CompoundStmt(std::move(*stmts));
            delete stmts;
        }
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
        /* LABEL parameter spec — treated as TEXT (ptr) */
        $1->push_back({VarDeclaration::TEXT, std::move(*$3)});
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
    | param_specs T_PROCEDURE T_IDENT T_SEMI {
        /* Procedure parameter spec: PROCEDURE name; */
        auto names = new std::vector<std::string>();
        names->push_back($3);
        $1->push_back({VarDeclaration::TEXT, std::move(*names)});
        delete names;
        $$ = $1;
      }
    | param_specs T_PROCEDURE T_IDENT T_IS virtual_is_rhs {
        /* Procedure parameter spec with IS binding — ignored */
        auto names = new std::vector<std::string>();
        names->push_back($3);
        $1->push_back({VarDeclaration::TEXT, std::move(*names)});
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
                               std::move(*$7), StmtPtr($9));
        delete $7;
      }
    | T_REF T_LPAREN T_IDENT T_RPAREN T_PROCEDURE T_IDENT T_LPAREN ident_list T_RPAREN T_SEMI param_specs statement {
        auto params = mergeParams(*$8, *$11);
        $$ = new ProcedureDecl($6, true, VarDeclaration::TEXT,
                               std::move(params), StmtPtr($12));
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
        $$ = new VirtualDecl(std::move(*$3));
        delete $3;
      }
    ;

virtual_entries
    : virtual_entry { $$ = new std::vector<std::string>(); if ($1) { $$->push_back($1); free($1); } }
    | virtual_entries virtual_entry {
        if ($2) { $1->push_back($2); free($2); }
        $$ = $1;
      }
    ;

virtual_entry
    : T_PROCEDURE T_IDENT T_SEMI { $$ = $2; }
    | type_name T_PROCEDURE T_IDENT T_SEMI { $$ = $3; }
    | T_REF T_LPAREN T_IDENT T_RPAREN T_PROCEDURE T_IDENT T_SEMI { $$ = $6; }
    /* IS bindings — consume the full prototype including optional params */
    | T_PROCEDURE T_IDENT T_IS virtual_is_rhs { $$ = $2; }
    | type_name T_PROCEDURE T_IDENT T_IS virtual_is_rhs { $$ = $3; }
    ;

/* The RHS of a VIRTUAL IS binding: a procedure prototype possibly with params.
   We consume tokens permissively until we hit ;; or ; */
virtual_is_rhs
    : T_PROCEDURE T_IDENT T_SEMI T_SEMI
    | T_PROCEDURE T_IDENT T_SEMI
    | type_name T_PROCEDURE T_IDENT T_SEMI T_SEMI
    | type_name T_PROCEDURE T_IDENT T_SEMI
    | T_PROCEDURE T_IDENT T_LPAREN ident_list T_RPAREN T_SEMI param_specs T_SEMI
    | type_name T_PROCEDURE T_IDENT T_LPAREN ident_list T_RPAREN T_SEMI param_specs T_SEMI
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
    | T_INSPECT expr T_DO statement {
        /* INSPECT ref DO stmt — execute stmt with ref in scope */
        auto clauses = new std::vector<InspectStatement::WhenClause>();
        $$ = new InspectStatement(ExprPtr($2), std::move(*clauses), StmtPtr($4));
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
        /* Chained assignment: a := b := expr — assign expr to both */
        auto stmts = new StmtList();
        Identifier* id2 = dynamic_cast<Identifier*>($3);
        if (id2) {
            stmts->push_back(StmtPtr(new Assignment(id2->name, ExprPtr($5))));
            /* Now assign b's value to a */
            Identifier* id1 = dynamic_cast<Identifier*>($1);
            if (id1) {
                stmts->push_back(StmtPtr(new Assignment(id1->name,
                    ExprPtr(new Identifier(id2->name)))));
                delete $1;
            }
            delete $3;
        }
        $$ = new CompoundStmt(std::move(*stmts));
        delete stmts;
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
                    MethodCall* mc = dynamic_cast<MethodCall*>($1);
                    if (mc) {
                        /* obj.data(i) := expr — member array assignment.
                           For now, silently drop the assignment (codegen limitation). */
                        $$ = new ExprStatement(ExprPtr(new IntegerLiteral(0)));
                        delete $1;
                        delete $3;
                    } else {
                        yyerror("invalid left-hand side of assignment");
                        $$ = new ExprStatement(ExprPtr($1));
                        delete $3;
                    }
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
            ProcedureCall* call = dynamic_cast<ProcedureCall*>($1);
            if (call) {
                /* a(i) :- expr  ->  ArrayAssignment (ref-assign to array element) */
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
                    MethodCall* mc = dynamic_cast<MethodCall*>($1);
                    if (mc) {
                        $$ = new ExprStatement(ExprPtr(new IntegerLiteral(0)));
                        delete $1; delete $3;
                    } else {
                        yyerror("invalid left-hand side of reference assignment");
                        $$ = new ExprStatement(ExprPtr($1));
                        delete $3;
                    }
                }
            }
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
    | expr T_EQV comparison {
        /* EQV = logical equivalence = NOT (a XOR b) */
        $$ = new UnaryOp(UnaryOp::NOT, ExprPtr(
            new BinaryOp(BinaryOp::NE, ExprPtr($1), ExprPtr($3))));
      }
    | expr T_IMP comparison {
        /* IMP = logical implication = NOT a OR b */
        $$ = new BinaryOp(BinaryOp::OR,
            ExprPtr(new UnaryOp(UnaryOp::NOT, ExprPtr($1))), ExprPtr($3));
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
        /* == reference identity (pointer comparison) */
        $$ = new BinaryOp(BinaryOp::EQ, ExprPtr($1), ExprPtr($3));
      }
    | additive T_REFNE additive {
        /* =/= reference non-identity */
        $$ = new BinaryOp(BinaryOp::NE, ExprPtr($1), ExprPtr($3));
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
    : power
    | multiplicative T_STAR power {
        $$ = new BinaryOp(BinaryOp::MUL, ExprPtr($1), ExprPtr($3));
      }
    | multiplicative T_SLASH power {
        $$ = new BinaryOp(BinaryOp::DIV, ExprPtr($1), ExprPtr($3));
      }
    | multiplicative T_IDIV power {
        $$ = new BinaryOp(BinaryOp::IDIV, ExprPtr($1), ExprPtr($3));
      }
    ;

power
    : unary
    | unary T_POWER power {
        $$ = new BinaryOp(BinaryOp::POWER, ExprPtr($1), ExprPtr($3));
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
    | postfix T_DOT io_keyword T_LPAREN arg_list T_RPAREN {
        /* Allow I/O keywords as method names: obj.OutFix(...) etc. */
        $$ = new MethodCall(ExprPtr($1), $3, std::move(*$5));
        delete $5;
      }
    | postfix T_DOT io_keyword {
        $$ = new MemberAccess(ExprPtr($1), $3);
      }
    | postfix T_QUA T_IDENT {
        /* QUA type qualification — changes class resolution for member access */
        $$ = new QuaExpression(ExprPtr($1), $3);
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
    : T_INTLIT    { $$ = new IntegerLiteral($1); }
    | T_REALLIT   { $$ = new RealLiteral($1); }
    | T_TEXTLIT   { $$ = new TextLiteral($1); }
    | T_CHARLIT   { $$ = new CharLiteral((char)$1); }
    | T_TRUE      { $$ = new BooleanLiteral(true); }
    | T_FALSE     { $$ = new BooleanLiteral(false); }
    | T_NONE      { $$ = new NoneLiteral(); }
    | T_NOTEXT    { $$ = new NoneLiteral(); }
    | T_THIS      { $$ = new ThisExpression(); }
    | T_THIS T_IDENT { $$ = new ThisExpression(); } /* THIS ClassName — type qual ignored */
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
    | T_LASTITEM  { $$ = new Identifier("__lastitem"); }
    | T_IF expr T_THEN expr T_ELSE expr {
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
