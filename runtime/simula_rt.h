#ifndef SIMULA_RT_H
#define SIMULA_RT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SIMULA_CORO_STACK_SIZE (32 * 1024)

typedef struct SimulaCoro SimulaCoro;

/* Memory allocation */
void* simula_alloc(int64_t size);

/* Text operations */
char* simula_blanks(int64_t n);
char* simula_text_copy(const char* s);
char* simula_text_concat(const char* a, const char* b);
int64_t simula_text_length(const char* s);
char* simula_text_strip(const char* s);
char* simula_text_sub(const char* s, int64_t start, int64_t len);
int64_t simula_text_eq(const char* a, const char* b);

/* SYSIN LASTITEM: skip whitespace and report whether end-of-file was reached
 * (look-ahead; the next non-blank char is pushed back so a later read sees it). */
int64_t simula_lastitem(void);

/* Numeric helpers */
int64_t simula_ipow(int64_t base, int64_t exp);
void simula_outint(int64_t v, int64_t w);

/* TEXT editing / de-editing */
int64_t simula_text_getint(const char* t);
double simula_text_getreal(const char* t);
int64_t simula_text_getfrac(const char* t);
void simula_text_putint(char* t, int64_t v);
void simula_text_putfix(char* t, double r, int64_t d);
void simula_text_putreal(char* t, double r, int64_t d);
void simula_text_putfrac(char* t, int64_t v, int64_t d);

/* Coroutine management */
SimulaCoro* simula_coro_create(void);
void simula_coro_start(SimulaCoro* coro, void (*func)(void*), void* arg);
void simula_coro_detach(SimulaCoro* coro);
void simula_coro_resume(SimulaCoro* coro);
void simula_coro_free(SimulaCoro* coro);

#ifdef __cplusplus
}
#endif

#endif /* SIMULA_RT_H */
