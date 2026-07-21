#ifndef SIMULA_RT_H
#define SIMULA_RT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SIMULA_CORO_STACK_SIZE (32 * 1024)

typedef struct SimulaCoro SimulaCoro;

/* A Simula TEXT value: a reference to a shared character frame plus the
 * subtext window (start, length) and the read/write cursor (pos). A TEXT
 * variable holds a pointer to one of these; NOTEXT is the null pointer.
 * SUB/STRIP/MAIN return descriptors that ALIAS the parent frame (no copy),
 * so writes through a subtext reach the parent. */
typedef struct SimulaText {
    char*   frame;    /* shared character storage (not NUL-terminated in general) */
    int64_t start;    /* 0-based offset of this text within the frame */
    int64_t length;   /* number of characters in this text */
    int64_t pos;      /* 0-based cursor, in [0, length] */
    int64_t framelen; /* total size of frame (so MAIN recovers the whole text) */
} SimulaText;

/* Memory allocation */
void* simula_alloc(int64_t size);

/* Runtime safety checks (checked mode): print a source-located diagnostic and abort. */
SimulaText* simula_upcase(SimulaText* t);
SimulaText* simula_lowcase(SimulaText* t);

void simula_math_error(int64_t code, int64_t line);
void simula_virtual_missing(int64_t line);
void simula_qua_error(int64_t line);
void simula_expi_error(int64_t line);
void simula_div_zero(int64_t line);
void simula_bounds_error(int64_t idx, int64_t lo, int64_t hi, int64_t line);
void simula_nil_ref(int64_t line);

/* Text constructors / operations (all on SimulaText* descriptors; NULL = NOTEXT) */
SimulaText* simula_text_lit(const char* cstr);            /* wrap (no copy) */
SimulaText* simula_text_dup(const char* cstr);            /* copy into writable frame */
SimulaText* simula_blanks(int64_t n);
SimulaText* simula_text_copy(SimulaText* s);
SimulaText* simula_text_concat(SimulaText* a, SimulaText* b);
int64_t     simula_text_length(SimulaText* s);
SimulaText* simula_text_strip(SimulaText* s);            /* alias, trailing blanks trimmed */
SimulaText* simula_text_sub(SimulaText* s, int64_t start, int64_t len); /* alias */
SimulaText* simula_text_main(SimulaText* s);             /* alias of the whole frame span */
int64_t     simula_text_eq(SimulaText* a, SimulaText* b);    /* content equality (=) */
int64_t     simula_text_ref_eq(SimulaText* a, SimulaText* b);/* reference identity (==) */
SimulaText* simula_text_assign(SimulaText* dst, SimulaText* src); /* := in place, returns binding */
/* Cursor operations */
int64_t simula_text_more(SimulaText* s);
int64_t simula_text_pos(SimulaText* s);                  /* 1-based */
void    simula_text_setpos(SimulaText* s, int64_t p);    /* 1-based */
int8_t  simula_text_getchar(SimulaText* s);
void    simula_text_putchar(SimulaText* s, int8_t c);
void    simula_outtext(SimulaText* s);                   /* write window to stdout */

/* SYSIN LASTITEM: skip whitespace and report whether end-of-file was reached
 * (look-ahead; the next non-blank char is pushed back so a later read sees it). */
int64_t simula_lastitem(void);
int64_t simula_inchar(void);   /* EOF yields the EM character (rank 25) */
double simula_inreal(void);    /* accepts '&' as the lowten exponent mark */

/* Numeric helpers */
int64_t simula_ipow(int64_t base, int64_t exp);
void simula_outint(int64_t v, int64_t w);

/* File output (OUTFILE) and item-level file input */
int64_t simula_outopen(SimulaText* name);
void simula_outclose(int64_t handle);
void simula_file_outtext(int64_t handle, SimulaText* t);
void simula_file_outint(int64_t handle, int64_t v, int64_t w);
void simula_file_outimage(int64_t handle);
int64_t simula_file_inint(int64_t handle);
double simula_file_inreal(int64_t handle);
int64_t simula_file_lastitem(int64_t handle);
int64_t simula_text_moreitem(SimulaText* t);
SimulaText* simula_text_intext(SimulaText* t, int64_t n);
void simula_file_outfix(int64_t handle, double v, int64_t d, int64_t w);
void simula_file_outreal(int64_t handle, double v, int64_t d, int64_t w);
void simula_file_outfrac(int64_t handle, int64_t v, int64_t d, int64_t w);
void simula_file_outchar(int64_t handle, int64_t c);

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
void* simula_sim_nextev(void* obj);
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

/* TEXT editing / de-editing (de-editing advances the descriptor's cursor) */
int64_t simula_text_getint(SimulaText* t);
double simula_text_getreal(SimulaText* t);
int64_t simula_text_getfrac(SimulaText* t);
void simula_text_putint(SimulaText* t, int64_t v);
void simula_text_putfix(SimulaText* t, double r, int64_t d);
void simula_text_putreal(SimulaText* t, double r, int64_t d);
void simula_text_putfrac(SimulaText* t, int64_t v, int64_t d);
SimulaText* simula_inreadtext(int64_t handle, int64_t maxlen); /* returns a TEXT line */

/* Coroutine management */
SimulaCoro* simula_coro_create(void);
void simula_coro_start(SimulaCoro* coro, void (*func)(void*), void* arg);
void simula_coro_detach(SimulaCoro* coro);
void simula_coro_resume(SimulaCoro* coro);
void simula_coro_call(SimulaCoro* coro);
void simula_coro_free(SimulaCoro* coro);

#ifdef __cplusplus
}
#endif

#endif /* SIMULA_RT_H */
