%{
#include <string>
#include "ast.h"
#include "parser.tab.hpp"

#define YY_USER_ACTION yylloc.first_line = yylineno;

/* Decode a Simula string token: one or more quoted segments separated by
 * blanks/line breaks (a multi-part string denotes the concatenation). Inside
 * a segment, "" denotes one quote character and !ddd! is an ISO-code. */
static char* simula_string_token(const char* s) {
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    size_t i = 0, j = 0;
    while (i < n) {
        if (s[i] != '"') { i++; continue; }   /* skip separator between segments */
        i++;                                   /* inside a segment */
        while (i < n) {
            if (s[i] == '"') {
                if (i + 1 < n && s[i+1] == '"') { out[j++] = '"'; i += 2; }
                else { i++; break; }           /* closing quote */
            } else if (s[i] == '!') {
                size_t k = i + 1; int v = 0, nd = 0;
                while (k < n && s[k] >= '0' && s[k] <= '9' && nd < 3) {
                    v = v * 10 + (s[k] - '0'); k++; nd++;
                }
                if (nd > 0 && k < n && s[k] == '!') { out[j++] = (char)v; i = k + 1; }
                else out[j++] = s[i++];
            } else out[j++] = s[i++];
        }
    }
    out[j] = 0;
    return out;
}
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
"AND"[ \t\r\n]+"THEN"           return T_ANDTHEN;
"OR"[ \t\r\n]+"ELSE"            return T_ORELSE;
"AND"                           return T_AND;
"OR"                            return T_OR;
"EQV"                           return T_EQV;
"IMP"                           return T_IMP;
"QUA"                           return T_QUA;
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
"INNER"                         return T_INNER;
"ACTIVATE"                      return T_ACTIVATE;
"REACTIVATE"                    return T_REACTIVATE;
"AT"                            return T_AT;
"DELAY"                         return T_DELAY;
"BEFORE"                        return T_BEFORE;
"AFTER"                         return T_AFTER;
"PRIOR"                         return T_PRIOR;
"DETACH"                        return T_DETACH;
"RESUME"                        return T_RESUME;
"CALL"                          return T_CALL;
"ARRAY"                         return T_ARRAY;
"LABEL"                         return T_LABEL;
"SWITCH"                        return T_SWITCH;
"NOTEXT"                        return T_NOTEXT;
"EXTERNAL"                      return T_EXTERNAL;
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
"LASTITEM"                      return T_LASTITEM;

":-"                            return T_REFASSIGN;
":="                            return T_ASSIGN;
":"                             return T_COLON;
"+"                             return T_PLUS;
"-"                             return T_MINUS;
"**"                            return T_POWER;
"*"                             return T_STAR;
"//"                            return T_IDIV;
"/"                             return T_SLASH;
"=="                            return T_REFEQ;
"=/="                           return T_REFNE;
"="                             return T_EQ;
"<>"                            return T_NE;
"<="                            return T_LE;
">="                            return T_GE;
"<"                             return T_LT;
">"                             return T_GT;
"LT"                            return T_LT;  /* letter-form relationals */
"LE"                            return T_LE;
"EQ"                            return T_EQ;
"NE"                            return T_NE;
"GE"                            return T_GE;
"GT"                            return T_GT;
"("                             return T_LPAREN;
")"                             return T_RPAREN;
";"                             return T_SEMI;
","                             return T_COMMA;
"."                             return T_DOT;
"&"                             return T_AMP;

[a-zA-Z_][a-zA-Z0-9_]*         { for (char* p = yytext; *p; p++) *p = tolower(*p);
                                  yylval.sval = strdup(yytext);
                                  return T_IDENT; }
[0-9]+(\.[0-9]+)?(&&|&|[eE])[+-]?[0-9]+ {
                                  /* Simula exponent forms: 1.5&6, 2&&10 (long
                                     real), and the E spelling. Normalize the
                                     exponent marker to 'e' for strtod. */
                                  char buf[64]; size_t j = 0;
                                  for (char* p = yytext; *p && j < 62; p++) {
                                      if (*p == '&') {
                                          if (j == 0 || buf[j-1] != 'e') buf[j++] = 'e';
                                      } else if (*p == 'E') {
                                          buf[j++] = 'e';
                                      } else buf[j++] = *p;
                                  }
                                  buf[j] = 0;
                                  yylval.dval = atof(buf); return T_REALLIT; }
[0-9]+\.[0-9]+                  { yylval.dval = atof(yytext); return T_REALLIT; }
\.[0-9]+((&&|&|[eE])[+-]?[0-9]+)? {
                                  /* Real literal with empty integer part: .5, .25&3 */
                                  char buf[66]; size_t j = 0; buf[j++] = '0';
                                  for (char* p = yytext; *p && j < 62; p++) {
                                      if (*p == '&') {
                                          if (buf[j-1] != 'e') buf[j++] = 'e';
                                      } else if (*p == 'E') buf[j++] = 'e';
                                      else buf[j++] = *p;
                                  }
                                  buf[j] = 0;
                                  yylval.dval = atof(buf); return T_REALLIT; }
(&&|&)[+-]?[0-9]+               {
                                  /* Bare-exponent numeral: &2 = 1e2 (lowten) */
                                  char buf[66]; size_t j = 0; buf[j++] = '1';
                                  for (char* p = yytext; *p && j < 62; p++) {
                                      if (*p == '&') {
                                          if (buf[j-1] != 'e') buf[j++] = 'e';
                                      } else buf[j++] = *p;
                                  }
                                  buf[j] = 0;
                                  yylval.dval = atof(buf); return T_REALLIT; }
[0-9]+                          { yylval.ival = atoll(yytext); return T_INTLIT; }
\"([^"]|\"\")*\"([ \t\r\n]+\"([^"]|\"\")*\")*   { yylval.sval = simula_string_token(yytext);
                                  return T_TEXTLIT; }
'![0-9]{1,3}!'                  { yylval.ival = atoi(yytext + 2); return T_CHARLIT; }
'''                             { yylval.ival = '\''; return T_CHARLIT; }
'[^']'                          { yylval.ival = yytext[1]; return T_CHARLIT; }

.                               { fprintf(stderr, "Lexer error line %d: unexpected '%s'\n",
                                          yylineno, yytext);
                                }

%%
