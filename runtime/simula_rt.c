/* _XOPEN_SOURCE must come before any system headers */
#ifndef _WIN32
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#endif

#include "simula_rt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ================================================================
 * Platform-specific coroutine backends
 * ================================================================ */

#ifdef _WIN32
/* ---- Windows: Fibers API ---- */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct SimulaCoro {
    void* fiber;
    void* caller_fiber;
    int state;  /* 0=new, 1=running, 2=detached, 3=terminated */
};

static void (*_pending_func)(void*);
static void* _pending_arg;
static SimulaCoro* _pending_coro;

static VOID CALLBACK coro_fiber_proc(LPVOID param) {
    void (*func)(void*) = _pending_func;
    void* arg = _pending_arg;
    SimulaCoro* self = _pending_coro;

    func(arg);

    self->state = 3;
    SwitchToFiber(self->caller_fiber);
    /* never reached */
}

SimulaCoro* simula_coro_create(void) {
    SimulaCoro* c = (SimulaCoro*)calloc(1, sizeof(SimulaCoro));
    c->state = 0;
    return c;
}

void simula_coro_start(SimulaCoro* coro, void (*func)(void*), void* arg) {
    /* Ensure the calling thread is a fiber */
    void* this_fiber = GetCurrentFiber();
    if (this_fiber == NULL || this_fiber == (void*)0x1E00) {
        this_fiber = ConvertThreadToFiber(NULL);
    }

    _pending_func = func;
    _pending_arg = arg;
    _pending_coro = coro;

    coro->fiber = CreateFiber(SIMULA_CORO_STACK_SIZE, coro_fiber_proc, NULL);
    coro->caller_fiber = this_fiber;
    coro->state = 1;

    SwitchToFiber(coro->fiber);
}

void simula_coro_detach(SimulaCoro* coro) {
    coro->state = 2;
    SwitchToFiber(coro->caller_fiber);
}

void simula_coro_resume(SimulaCoro* coro) {
    if (coro->state == 3) {
        fprintf(stderr, "simula_coro_resume: coroutine has terminated\n");
        return;
    }
    if (coro->state != 2) {
        fprintf(stderr, "simula_coro_resume: coroutine is not detached (state=%d)\n",
                coro->state);
        return;
    }

    void* this_fiber = GetCurrentFiber();
    coro->caller_fiber = this_fiber;
    coro->state = 1;

    SwitchToFiber(coro->fiber);
}

void simula_coro_free(SimulaCoro* coro) {
    if (coro) {
        if (coro->fiber) DeleteFiber(coro->fiber);
        free(coro);
    }
}

#else
/* ---- POSIX: ucontext ---- */

#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <ucontext.h>

struct SimulaCoro {
    ucontext_t context;
    ucontext_t caller_ctx;
    char* stack;
    int state;  /* 0=new, 1=running, 2=detached, 3=terminated */
};

static void (*_pending_func)(void*);
static void* _pending_arg;
static SimulaCoro* _pending_coro;

static void coro_trampoline(void) {
    void (*func)(void*) = _pending_func;
    void* arg = _pending_arg;
    SimulaCoro* self = _pending_coro;

    func(arg);

    self->state = 3;
    setcontext(&self->caller_ctx);
}

SimulaCoro* simula_coro_create(void) {
    SimulaCoro* c = (SimulaCoro*)calloc(1, sizeof(SimulaCoro));
    c->stack = (char*)malloc(SIMULA_CORO_STACK_SIZE);
    c->state = 0;
    return c;
}

void simula_coro_start(SimulaCoro* coro, void (*func)(void*), void* arg) {
    getcontext(&coro->context);
    coro->context.uc_stack.ss_sp = coro->stack;
    coro->context.uc_stack.ss_size = SIMULA_CORO_STACK_SIZE;
    coro->context.uc_link = NULL;

    _pending_func = func;
    _pending_arg = arg;
    _pending_coro = coro;

    makecontext(&coro->context, coro_trampoline, 0);

    coro->state = 1;
    swapcontext(&coro->caller_ctx, &coro->context);
}

void simula_coro_detach(SimulaCoro* coro) {
    coro->state = 2;
    swapcontext(&coro->context, &coro->caller_ctx);
}

void simula_coro_resume(SimulaCoro* coro) {
    if (coro->state == 3) {
        fprintf(stderr, "simula_coro_resume: coroutine has terminated\n");
        return;
    }
    if (coro->state != 2) {
        fprintf(stderr, "simula_coro_resume: coroutine is not detached (state=%d)\n",
                coro->state);
        return;
    }
    coro->state = 1;
    swapcontext(&coro->caller_ctx, &coro->context);
}

void simula_coro_free(SimulaCoro* coro) {
    if (coro) {
        free(coro->stack);
        free(coro);
    }
}

#ifdef __APPLE__
#pragma clang diagnostic pop
#endif

#endif /* _WIN32 */

/* ================================================================
 * Platform-independent: memory allocation
 * ================================================================ */

void* simula_alloc(int64_t size) {
    void* p = calloc(1, (size_t)size);
    if (!p) {
        fprintf(stderr, "simula_alloc: out of memory\n");
        exit(1);
    }
    return p;
}

/* ================================================================
 * Platform-independent: text operations
 * ================================================================ */

char* simula_blanks(int64_t n) {
    char* s = (char*)malloc((size_t)(n + 1));
    if (!s) {
        fprintf(stderr, "simula_blanks: out of memory\n");
        exit(1);
    }
    memset(s, ' ', (size_t)n);
    s[n] = '\0';
    return s;
}

char* simula_text_copy(const char* s) {
    if (s == NULL) return strdup("");
    return strdup(s);
}

char* simula_text_concat(const char* a, const char* b) {
    if (a == NULL) a = "";
    if (b == NULL) b = "";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char* result = (char*)malloc(la + lb + 1);
    if (!result) {
        fprintf(stderr, "simula_text_concat: out of memory\n");
        exit(1);
    }
    memcpy(result, a, la);
    memcpy(result + la, b, lb);
    result[la + lb] = '\0';
    return result;
}

int64_t simula_text_length(const char* s) {
    if (s == NULL) return 0;
    return (int64_t)strlen(s);
}

char* simula_text_strip(const char* s) {
    if (s == NULL) return strdup("");
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') len--;
    char* r = (char*)malloc(len + 1);
    memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

char* simula_text_sub(const char* s, int64_t start, int64_t len) {
    /* start is 1-based */
    if (s == NULL) return strdup("");
    size_t slen = strlen(s);
    int64_t idx = start - 1;
    if (idx < 0) idx = 0;
    if (idx >= (int64_t)slen) return strdup("");
    if (idx + len > (int64_t)slen) len = (int64_t)slen - idx;
    char* r = (char*)malloc((size_t)len + 1);
    memcpy(r, s + idx, (size_t)len);
    r[len] = '\0';
    return r;
}

int64_t simula_text_eq(const char* a, const char* b) {
    if (a == NULL && b == NULL) return 1;
    if (a == NULL || b == NULL) return 0;
    return strcmp(a, b) == 0 ? 1 : 0;
}

/* Look-ahead end-of-file test for SYSIN. Skips leading whitespace; if EOF is
 * reached returns 1 (true), otherwise pushes the next item's first character
 * back onto the stream and returns 0. This matches the Simula idiom
 * `WHILE NOT LASTITEM DO ... ININT ...`, where LASTITEM must peek without
 * consuming the next item. */
int64_t simula_lastitem(void) {
    int c;
    while ((c = getchar()) != EOF) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            continue;
        ungetc(c, stdin);
        return 0;
    }
    return 1;
}

/* ================================================================
 * File input (INFILE support)
 * ================================================================ */

/* Open a file for reading. Returns a FILE* cast to int64_t (0 on failure). */
int64_t simula_inopen(const char* name) {
    if (name == NULL) return 0;
    FILE* f = fopen(name, "r");
    return (int64_t)(intptr_t)f;
}

/* Read one line (without the trailing newline) into a freshly-allocated
 * string. Returns NULL at end-of-file. `maxlen` caps the line length. */
char* simula_inreadline(int64_t handle, int64_t maxlen) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f == NULL) return NULL;
    if (maxlen <= 0) maxlen = 132;
    size_t cap = (size_t)maxlen + 2;
    char* buf = (char*)malloc(cap);
    if (fgets(buf, (int)cap, f) == NULL) { free(buf); return NULL; }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    return buf;
}

void simula_inclose(int64_t handle) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f != NULL) fclose(f);
}

/* ================================================================
 * Numeric helpers
 * ================================================================ */

/* Exact integer exponentiation for INTEGER ** INTEGER.
 * Negative exponents are a runtime error in Simula; we return 0
 * (except 1**n and (-1)**n which stay exact). */
int64_t simula_ipow(int64_t base, int64_t exp) {
    if (exp < 0) {
        if (base == 1) return 1;
        if (base == -1) return (exp & 1) ? -1 : 1;
        return 0;
    }
    int64_t result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    return result;
}

/* OUTINT with standard editing: if the number does not fit in the
 * field, fill the field with asterisks. w <= 0 prints minimal width. */
void simula_outint(int64_t v, int64_t w) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    if (w <= 0) { fputs(buf, stdout); return; }
    if (n > w) {
        for (int64_t i = 0; i < w; i++) putchar('*');
        return;
    }
    printf("%*s", (int)w, buf);
}

/* ================================================================
 * TEXT editing / de-editing procedures (Simula 67 ch. 8)
 * ================================================================ */

/* GETINT: interpret the text as an integer item (leading blanks allowed). */
int64_t simula_text_getint(const char* t) {
    if (t == NULL) return 0;
    while (*t == ' ' || *t == '\t') t++;
    return strtoll(t, NULL, 10);
}

/* GETREAL: interpret the text as a real item; accepts the Simula '&'
 * exponent marker as well as 'e'/'E'. */
double simula_text_getreal(const char* t) {
    if (t == NULL) return 0.0;
    char buf[64]; size_t j = 0;
    for (const char* p = t; *p && j < 62; p++)
        buf[j++] = (*p == '&') ? 'e' : *p;
    buf[j] = '\0';
    return strtod(buf, NULL);
}

/* GETFRAC: interpret as a grouped item — digits with embedded spaces and an
 * optional decimal point; returns the integer formed by all the digits. */
int64_t simula_text_getfrac(const char* t) {
    if (t == NULL) return 0;
    int64_t v = 0; int neg = 0; const char* p = t;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') v = v * 10 + (*p - '0');
        else if (*p == ' ' || *p == '.') continue;
        else break;
    }
    return neg ? -v : v;
}

/* Right-justify `src` into the full frame of `t` (blank fill); if it does
 * not fit, fill the frame with asterisks (standard editing overflow rule). */
static void simula_edit_into(char* t, const char* src) {
    size_t flen = strlen(t);
    size_t slen = strlen(src);
    if (slen > flen) {
        memset(t, '*', flen);
        return;
    }
    memset(t, ' ', flen - slen);
    memcpy(t + (flen - slen), src, slen);
}

/* PUTINT: edit v right-justified into the whole frame of t. */
void simula_text_putint(char* t, int64_t v) {
    if (t == NULL) return;
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    simula_edit_into(t, buf);
}

/* PUTFIX: fixed-point with d digits after the point. */
void simula_text_putfix(char* t, double r, int64_t d) {
    if (t == NULL) return;
    char buf[64];
    if (d < 0) d = 0;
    snprintf(buf, sizeof buf, "%.*f", (int)d, r);
    simula_edit_into(t, buf);
}

/* PUTREAL: scientific form with d significant digits, '&' exponent marker. */
void simula_text_putreal(char* t, double r, int64_t d) {
    if (t == NULL) return;
    char buf[64];
    int prec = (int)(d > 0 ? d - 1 : 0);
    snprintf(buf, sizeof buf, "%.*e", prec, r);
    for (char* p = buf; *p; p++)
        if (*p == 'e' || *p == 'E') *p = '&';
    simula_edit_into(t, buf);
}

/* PUTFRAC: grouped item — digit groups of three separated by blanks, the
 * last d digits after a decimal point. E.g. v=1234567, d=2 -> "12 345.67". */
void simula_text_putfrac(char* t, int64_t v, int64_t d) {
    if (t == NULL) return;
    char digits[32];
    int neg = v < 0;
    unsigned long long uv = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    int n = snprintf(digits, sizeof digits, "%llu", uv);
    if (d < 0) d = 0;
    /* Pad with leading zeros so there are more than d digits */
    while (n <= (int)d && n < 30) {
        memmove(digits + 1, digits, (size_t)n + 1);
        digits[0] = '0';
        n++;
    }
    int intDigits = n - (int)d;
    char out[64]; int j = 0;
    if (neg) out[j++] = '-';
    for (int i = 0; i < intDigits && j < 60; i++) {
        if (i > 0 && (intDigits - i) % 3 == 0) out[j++] = ' ';
        out[j++] = digits[i];
    }
    if (d > 0 && j < 60) {
        out[j++] = '.';
        for (int i = intDigits; i < n && j < 62; i++) out[j++] = digits[i];
    }
    out[j] = '\0';
    simula_edit_into(t, out);
}
