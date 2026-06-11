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

/* File output (OUTFILE) and item-level file input */
int64_t simula_outopen(const char* name);
void simula_outclose(int64_t handle);
void simula_file_outtext(int64_t handle, const char* t);
void simula_file_outint(int64_t handle, int64_t v, int64_t w);
void simula_file_outimage(int64_t handle);
int64_t simula_file_inint(int64_t handle);
double simula_file_inreal(int64_t handle);
int64_t simula_file_lastitem(int64_t handle);

/* SYSOUT/SYSIN editing-style I/O */
void simula_outreal(double v, int64_t d, int64_t w);
void simula_outfrac(int64_t v, int64_t d, int64_t w);
int64_t simula_infrac(void);

/* SIMULATION event scheduling */
double simula_sim_time(void);
void* simula_sim_current(void);
int64_t simula_sim_idle(void* obj);
int64_t simula_sim_terminated(void* obj);
double simula_sim_evtime(void* obj);
void simula_sim_cancel(void* obj);
void simula_sim_activate(void* obj, double t, int64_t prior, int64_t reactivate);
void simula_sim_activate_now(void* obj, int64_t reactivate);
void simula_sim_activate_rel(void* obj, void* other, int64_t before, int64_t reactivate);
void simula_sim_hold(double dt);
void simula_sim_passivate(void);
void simula_sim_terminate(void* obj);

/* Random drawing procedures (seed passed by reference, advanced per draw) */
int64_t simula_randint(int64_t a, int64_t b, int64_t* u);
double simula_uniform(double a, double b, int64_t* u);
double simula_normal(double m, double s, int64_t* u);
double simula_negexp(double lambda, int64_t* u);
int64_t simula_poisson(double m, int64_t* u);
double simula_erlang(double a, double b, int64_t* u);
int64_t simula_draw(double p, int64_t* u);
int64_t simula_discrete(const double* a, int64_t lo, int64_t n, int64_t* u);
double simula_linear(const double* a, const double* b, int64_t n, int64_t* u);
int64_t simula_histd(const double* a, int64_t lo, int64_t n, int64_t* u);
void simula_histo(double* a, int64_t an, const double* b, int64_t bn,
                  double c, double w);

/* TEXT editing / de-editing */
int64_t simula_text_getint(const char* t);
double simula_text_getreal(const char* t);
int64_t simula_text_getfrac(const char* t);
int64_t simula_text_getint_at(const char* t, int64_t* pos0);
double simula_text_getreal_at(const char* t, int64_t* pos0);
int64_t simula_text_getfrac_at(const char* t, int64_t* pos0);
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
