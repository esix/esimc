%{
#include <string>
#include "ast.h"
#include "parser.tab.hpp"

#define YY_USER_ACTION yylloc.first_line = yylineno;
%}

%option noyywrap
%option yylineno
%option caseless

%%

[ \t\r]+                        ; /* skip whitespace */
\n                              ; /* skip newlines */
"!"[^\n]*                       ; /* comment: ! to end of line */
"COMMENT"[^;]*";"               ; /* COMMENT ... ; */

"BEGIN"                         return T_BEGIN;
"END"                           return T_END;
"INTEGER"                       return T_INTEGER;
"REAL"                          return T_REAL;
"BOOLEAN"                       return T_BOOLEAN;
"TEXT"                          return T_TEXT;
"CHARACTER"                     return T_CHARACTER;
"SHORT"                         return T_SHORT;
"LONG"                          return T_LONG;
"IF"                            return T_IF;
"THEN"                          return T_THEN;
"ELSE"                          return T_ELSE;
"WHILE"                         return T_WHILE;
"DO"                            return T_DO;
"FOR"                           return T_FOR;
"STEP"                          return T_STEP;
"UNTIL"                         return T_UNTIL;
"TRUE"                          return T_TRUE;
"FALSE"                         return T_FALSE;
"NOT"                           return T_NOT;
"AND"                           return T_AND;
"OR"                            return T_OR;
"PROCEDURE"                     return T_PROCEDURE;
"CLASS"                         return T_CLASS;
"NEW"                           return T_NEW;
"THIS"                          return T_THIS;
"REF"                           return T_REF;
"NONE"                          return T_NONE;
"IS"                            return T_IS;
"IN"                            return T_IN;
"VIRTUAL"                       return T_VIRTUAL;
"INSPECT"                       return T_INSPECT;
"WHEN"                          return T_WHEN;
"OTHERWISE"                     return T_OTHERWISE;
"DETACH"                        return T_DETACH;
"RESUME"                        return T_RESUME;
"CALL"                          return T_CALL;
"ARRAY"                         return T_ARRAY;
"LABEL"                         return T_LABEL;
"NOTEXT"                        return T_NOTEXT;
"GO"[ \t]+"TO"                  return T_GOTO;
"GOTO"                          return T_GOTO;
"OUTINT"                        return T_OUTINT;
"OUTREAL"                       return T_OUTREAL;
"OUTFIX"                        return T_OUTFIX;
"OUTTEXT"                       return T_OUTTEXT;
"OUTIMAGE"                      return T_OUTIMAGE;
"ININT"                         return T_ININT;
"INREAL"                        return T_INREAL;
"INCHAR"                        return T_INCHAR;
"INIMAGE"                       return T_INIMAGE;

":-"                            return T_REFASSIGN;
":="                            return T_ASSIGN;
":"                             return T_COLON;
"+"                             return T_PLUS;
"-"                             return T_MINUS;
"*"                             return T_STAR;
"//"                            return T_IDIV;
"/"                             return T_SLASH;
"="                             return T_EQ;
"<>"                            return T_NE;
"<="                            return T_LE;
">="                            return T_GE;
"<"                             return T_LT;
">"                             return T_GT;
"("                             return T_LPAREN;
")"                             return T_RPAREN;
";"                             return T_SEMI;
","                             return T_COMMA;
"."                             return T_DOT;
"&"                             return T_AMP;

[a-zA-Z_][a-zA-Z0-9_]*         { for (char* p = yytext; *p; p++) *p = tolower(*p);
                                  yylval.sval = strdup(yytext);
                                  return T_IDENT; }
[0-9]+\.[0-9]+                  { yylval.dval = atof(yytext); return T_REALLIT; }
[0-9]+                          { yylval.ival = atoll(yytext); return T_INTLIT; }
\"([^"\\]|\\.)*\"               { yytext[yyleng-1] = '\0';
                                  yylval.sval = strdup(yytext+1);
                                  return T_TEXTLIT; }
'[^']'                          { yylval.ival = yytext[1]; return T_CHARLIT; }

.                               { fprintf(stderr, "Lexer error line %d: unexpected '%s'\n",
                                          yylineno, yytext);
                                }

%%
