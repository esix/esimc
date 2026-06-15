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

/* Allocate a descriptor over a frame of total size `framelen`. */
static SimulaText* st_new(char* frame, int64_t start, int64_t length,
                          int64_t pos, int64_t framelen) {
    SimulaText* t = (SimulaText*)malloc(sizeof(SimulaText));
    if (!t) { fprintf(stderr, "simula text: out of memory\n"); exit(1); }
    t->frame = frame; t->start = start; t->length = length; t->pos = pos;
    t->framelen = framelen;
    return t;
}

/* Allocate a fresh, blank-filled frame of n chars wrapped in a descriptor. */
static SimulaText* st_fresh(int64_t n) {
    if (n < 0) n = 0;
    char* f = (char*)malloc((size_t)(n > 0 ? n : 1));
    if (!f) { fprintf(stderr, "simula text: out of memory\n"); exit(1); }
    memset(f, ' ', (size_t)n);
    return st_new(f, 0, n, 0, n);
}

/* Wrap a C literal/string as a read-only TEXT over its (shared) storage. */
SimulaText* simula_text_lit(const char* cstr) {
    if (cstr == NULL) return NULL;
    int64_t n = (int64_t)strlen(cstr);
    return st_new((char*)cstr, 0, n, 0, n);
}

SimulaText* simula_blanks(int64_t n) {
    return st_fresh(n);
}

SimulaText* simula_text_copy(SimulaText* s) {
    if (s == NULL) return st_fresh(0);
    SimulaText* r = st_fresh(s->length);
    if (s->length > 0) memcpy(r->frame, s->frame + s->start, (size_t)s->length);
    return r;
}

SimulaText* simula_text_concat(SimulaText* a, SimulaText* b) {
    int64_t la = a ? a->length : 0;
    int64_t lb = b ? b->length : 0;
    SimulaText* r = st_fresh(la + lb);
    if (la > 0) memcpy(r->frame, a->frame + a->start, (size_t)la);
    if (lb > 0) memcpy(r->frame + la, b->frame + b->start, (size_t)lb);
    return r;
}

int64_t simula_text_length(SimulaText* s) {
    return s ? s->length : 0;
}

/* STRIP returns a subtext (alias) with trailing blanks removed. */
SimulaText* simula_text_strip(SimulaText* s) {
    if (s == NULL) return st_fresh(0);
    int64_t len = s->length;
    while (len > 0 && s->frame[s->start + len - 1] == ' ') len--;
    return st_new(s->frame, s->start, len, 0, s->framelen);
}

/* SUB(start,len) with 1-based start; aliases the parent frame (no copy). */
SimulaText* simula_text_sub(SimulaText* s, int64_t start, int64_t len) {
    if (s == NULL) return st_fresh(0);
    int64_t idx = start - 1;
    if (idx < 0) idx = 0;
    if (idx > s->length) idx = s->length;
    if (len < 0) len = 0;
    if (idx + len > s->length) len = s->length - idx;
    return st_new(s->frame, s->start + idx, len, 0, s->framelen);
}

/* MAIN: the whole underlying frame (per the standard, the main text). */
SimulaText* simula_text_main(SimulaText* s) {
    if (s == NULL) return NULL;
    return st_new(s->frame, 0, s->framelen, 0, s->framelen);
}

/* Content equality (= operator): same length and same characters. NOTEXT
 * equals only NOTEXT; an allocated "" (length 0) does NOT equal NOTEXT. */
int64_t simula_text_eq(SimulaText* a, SimulaText* b) {
    if (a == NULL && b == NULL) return 1;
    if (a == NULL || b == NULL) return 0;
    if (a->length != b->length) return 0;
    if (a->length == 0) return 1;
    return memcmp(a->frame + a->start, b->frame + b->start, (size_t)a->length) == 0 ? 1 : 0;
}

/* Reference identity (== operator): the same text object, i.e. same frame,
 * start and length (cursor is not part of identity). Both NOTEXT => true. */
int64_t simula_text_ref_eq(SimulaText* a, SimulaText* b) {
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    return (a->frame == b->frame && a->start == b->start && a->length == b->length) ? 1 : 0;
}

/* Value assignment (:=): copy characters from src into dst's frame window,
 * blank-padding if src is shorter; if src is longer, copy what fits. Returns
 * the descriptor that should be bound to the LHS: dst when it has a frame, or
 * a fresh COPY of src when dst is NOTEXT/frameless (first-assignment case). */
SimulaText* simula_text_assign(SimulaText* dst, SimulaText* src) {
    if (dst == NULL || dst->frame == NULL) return simula_text_copy(src);
    int64_t slen = src ? src->length : 0;
    int64_t n = slen < dst->length ? slen : dst->length;
    if (n > 0) memcpy(dst->frame + dst->start, src->frame + src->start, (size_t)n);
    for (int64_t i = n; i < dst->length; i++) dst->frame[dst->start + i] = ' ';
    return dst;
}

int64_t simula_text_more(SimulaText* s) {
    if (s == NULL) return 0;
    return s->pos < s->length ? 1 : 0;
}

int64_t simula_text_pos(SimulaText* s) {
    return s ? s->pos + 1 : 1;
}

void simula_text_setpos(SimulaText* s, int64_t p) {
    if (s == NULL) return;
    int64_t z = p - 1;
    if (z < 0) z = 0;
    if (z > s->length) z = s->length;
    s->pos = z;
}

int8_t simula_text_getchar(SimulaText* s) {
    if (s == NULL || s->pos >= s->length) return 0;
    return (int8_t)s->frame[s->start + s->pos++];
}

void simula_text_putchar(SimulaText* s, int8_t c) {
    if (s == NULL || s->pos >= s->length) return;
    s->frame[s->start + s->pos++] = (char)c;
}

void simula_outtext(SimulaText* s) {
    if (s == NULL || s->length <= 0) return;
    fwrite(s->frame + s->start, 1, (size_t)s->length, stdout);
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

/* Extract a TEXT descriptor's window into a NUL-terminated C string (filenames
 * etc.). Returns a static buffer good until the next call — fine for fopen. */
static const char* st_cstr(SimulaText* t) {
    static char buf[1024];
    if (t == NULL) { buf[0] = '\0'; return buf; }
    int64_t n = t->length;
    if (n > 1023) n = 1023;
    if (n > 0) memcpy(buf, t->frame + t->start, (size_t)n);
    buf[n] = '\0';
    return buf;
}

/* Open a file for reading. Returns a FILE* cast to int64_t (0 on failure). */
int64_t simula_inopen(SimulaText* name) {
    if (name == NULL) return 0;
    FILE* f = fopen(st_cstr(name), "r");
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

/* De-editing scans the descriptor window from its cursor and advances pos
 * past the parsed item, so sequential GETINT/GETREAL/GETFRAC walk the text. */

int64_t simula_text_getint(SimulaText* t) {
    if (t == NULL) return 0;
    int64_t i = t->pos;
    const char* f = t->frame + t->start;
    while (i < t->length && (f[i] == ' ' || f[i] == '\t')) i++;
    int64_t v = 0, neg = 0, seen = 0;
    if (i < t->length && (f[i] == '-' || f[i] == '+')) { neg = (f[i] == '-'); i++; }
    while (i < t->length && f[i] >= '0' && f[i] <= '9') { v = v * 10 + (f[i] - '0'); i++; seen = 1; }
    if (seen) t->pos = i;
    return neg ? -v : v;
}

double simula_text_getreal(SimulaText* t) {
    if (t == NULL) return 0.0;
    int64_t i = t->pos;
    const char* f = t->frame + t->start;
    while (i < t->length && (f[i] == ' ' || f[i] == '\t')) i++;
    char buf[64]; size_t j = 0;
    int64_t startScan = i;
    while (i < t->length && j < 62) {
        char c = f[i];
        if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' ||
            c == 'e' || c == 'E' || c == '&') {
            buf[j++] = (c == '&') ? 'e' : c;
            i++;
        } else break;
    }
    buf[j] = '\0';
    char* end = NULL;
    double v = strtod(buf, &end);
    if (end && end != buf) t->pos = startScan + (int64_t)(end - buf);
    return v;
}

int64_t simula_text_getfrac(SimulaText* t) {
    if (t == NULL) return 0;
    int64_t i = t->pos;
    const char* f = t->frame + t->start;
    while (i < t->length && (f[i] == ' ' || f[i] == '\t')) i++;
    int64_t v = 0, neg = 0, seen = 0;
    if (i < t->length && (f[i] == '-' || f[i] == '+')) { neg = (f[i] == '-'); i++; }
    while (i < t->length) {
        char c = f[i];
        if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); seen = 1; i++; }
        else if ((c == ' ' || c == '.') && seen &&
                 i + 1 < t->length && f[i+1] >= '0' && f[i+1] <= '9') i++;
        else break;
    }
    if (seen) t->pos = i;
    return neg ? -v : v;
}

/* Right-justify `src` into a char window of `width` (blank fill); if it does
 * not fit, fill the window with asterisks (standard editing overflow rule). */
static void edit_into(char* base, int64_t width, const char* src) {
    int64_t slen = (int64_t)strlen(src);
    if (slen > width) { memset(base, '*', (size_t)width); return; }
    memset(base, ' ', (size_t)(width - slen));
    memcpy(base + (width - slen), src, (size_t)slen);
}

/* Build the grouped PUTFRAC/OUTFRAC string into `out`. */
static void frac_to_str(int64_t v, int64_t d, char* out) {
    char digits[32];
    int neg = v < 0;
    unsigned long long uv = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    int n = snprintf(digits, sizeof digits, "%llu", uv);
    if (d < 0) d = 0;
    while (n <= (int)d && n < 30) {
        memmove(digits + 1, digits, (size_t)n + 1);
        digits[0] = '0';
        n++;
    }
    int intDigits = n - (int)d;
    int j = 0;
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
}

void simula_text_putint(SimulaText* t, int64_t v) {
    if (t == NULL) return;
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    edit_into(t->frame + t->start, t->length, buf);
}

void simula_text_putfix(SimulaText* t, double r, int64_t d) {
    if (t == NULL) return;
    char buf[64];
    if (d < 0) d = 0;
    snprintf(buf, sizeof buf, "%.*f", (int)d, r);
    edit_into(t->frame + t->start, t->length, buf);
}

void simula_text_putreal(SimulaText* t, double r, int64_t d) {
    if (t == NULL) return;
    char buf[64];
    int prec = (int)(d > 0 ? d - 1 : 0);
    snprintf(buf, sizeof buf, "%.*e", prec, r);
    for (char* p = buf; *p; p++)
        if (*p == 'e' || *p == 'E') *p = '&';
    edit_into(t->frame + t->start, t->length, buf);
}

void simula_text_putfrac(SimulaText* t, int64_t v, int64_t d) {
    if (t == NULL) return;
    char out[64];
    frac_to_str(v, d, out);
    edit_into(t->frame + t->start, t->length, out);
}

/* ================================================================
 * File output (OUTFILE support) and item-level file input
 * ================================================================ */

int64_t simula_outopen(SimulaText* name) {
    if (name == NULL) return 0;
    FILE* f = fopen(st_cstr(name), "w");
    return (int64_t)(intptr_t)f;
}

void simula_outclose(int64_t handle) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f != NULL) fclose(f);
}

void simula_file_outtext(int64_t handle, SimulaText* t) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f != NULL && t != NULL && t->length > 0)
        fwrite(t->frame + t->start, 1, (size_t)t->length, f);
}

/* INFILE INIMAGE/INTEXT support: read one line and wrap it as a TEXT. */
SimulaText* simula_inreadtext(int64_t handle, int64_t maxlen) {
    char* line = simula_inreadline(handle, maxlen);
    if (line == NULL) return NULL;
    int64_t n = (int64_t)strlen(line);
    return st_new(line, 0, n, 0, n);
}

void simula_file_outint(int64_t handle, int64_t v, int64_t w) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f == NULL) return;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    if (w <= 0) { fputs(buf, f); return; }
    if (n > w) { for (int64_t i = 0; i < w; i++) fputc('*', f); return; }
    fprintf(f, "%*s", (int)w, buf);
}

void simula_file_outimage(int64_t handle) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f != NULL) fputc('\n', f);
}

/* Item-level input: skip whitespace, read an integer / real. */
int64_t simula_file_inint(int64_t handle) {
    FILE* f = (FILE*)(intptr_t)handle;
    long long v = 0;
    if (f != NULL) { if (fscanf(f, "%lld", &v) != 1) v = 0; }
    return (int64_t)v;
}

double simula_file_inreal(int64_t handle) {
    FILE* f = (FILE*)(intptr_t)handle;
    double v = 0.0;
    if (f != NULL) { if (fscanf(f, "%lf", &v) != 1) v = 0.0; }
    return v;
}

/* Look-ahead: skip blanks; true when only EOF remains. */
int64_t simula_file_lastitem(int64_t handle) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f == NULL) return 1;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            continue;
        ungetc(c, f);
        return 0;
    }
    return 1;
}

/* ================================================================
 * SYSOUT editing-style output and SYSIN INFRAC
 * ================================================================ */

/* OUTREAL(v, d, w): d significant digits, '&' exponent, width w
 * (asterisk fill on overflow, like the editing procedures). */
void simula_outreal(double v, int64_t d, int64_t w) {
    char buf[64];
    int prec = (int)(d > 0 ? d - 1 : 0);
    snprintf(buf, sizeof buf, "%.*e", prec, v);
    for (char* p = buf; *p; p++)
        if (*p == 'e' || *p == 'E') *p = '&';
    int n = (int)strlen(buf);
    if (w <= 0) { fputs(buf, stdout); return; }
    if (n > w) { for (int64_t i = 0; i < w; i++) putchar('*'); return; }
    printf("%*s", (int)w, buf);
}

/* OUTFRAC(v, d, w): grouped item, like PUTFRAC, in a width-w field. */
void simula_outfrac(int64_t v, int64_t d, int64_t w) {
    char out[64];
    frac_to_str(v, d, out);
    int width = (int)(w > 0 && w < 60 ? w : 0);
    if (width == 0) { fputs(out, stdout); return; }
    char frame[64];
    edit_into(frame, width, out);
    fwrite(frame, 1, (size_t)width, stdout);
}

/* INFRAC: read a grouped item from stdin (digits with embedded blanks
 * and an optional decimal point). */
int64_t simula_infrac(void) {
    int c; int64_t v = 0; int neg = 0; int seen = 0;
    while ((c = getchar()) != EOF &&
           (c == ' ' || c == '\t' || c == '\n' || c == '\r')) ;
    if (c == '-') { neg = 1; c = getchar(); }
    else if (c == '+') c = getchar();
    while (c != EOF) {
        if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); seen = 1; }
        else if ((c == ' ' || c == '.') && seen) { /* grouping */ }
        else break;
        c = getchar();
    }
    if (c != EOF) ungetc(c, stdin);
    return neg ? -v : v;
}

/* ================================================================
 * Random drawing procedures (Simula 67 ch. 9.9)
 * The INTEGER seed is passed by reference (call-by-name in source)
 * and advanced on every basic draw, so streams are reproducible and
 * independent per seed variable.
 * ================================================================ */

#include <math.h>

/* Basic draw: 64-bit LCG (Knuth), uniform in (0,1). */
static double simula_basicdraw(int64_t* u) {
    uint64_t x = (uint64_t)*u;
    if (x == 0) x = 0x9E3779B97F4A7C15ULL;
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    *u = (int64_t)x;
    double v = (double)((x >> 11) & ((1ULL << 53) - 1)) / 9007199254740992.0;
    return v > 0.0 ? v : 5e-324;
}

int64_t simula_randint(int64_t a, int64_t b, int64_t* u) {
    double d = simula_basicdraw(u);
    int64_t span = b - a + 1;
    if (span <= 0) return a;
    int64_t r = a + (int64_t)(d * (double)span);
    return r > b ? b : r;
}

double simula_uniform(double a, double b, int64_t* u) {
    return a + simula_basicdraw(u) * (b - a);
}

double simula_normal(double m, double s, int64_t* u) {
    double u1 = simula_basicdraw(u);
    double u2 = simula_basicdraw(u);
    return m + s * sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

double simula_negexp(double lambda, int64_t* u) {
    if (lambda <= 0.0) return 0.0;
    return -log(simula_basicdraw(u)) / lambda;
}

int64_t simula_poisson(double m, int64_t* u) {
    if (m <= 0.0) return 0;
    double limit = exp(-m), prod = simula_basicdraw(u);
    int64_t n = 0;
    while (prod >= limit) { n++; prod *= simula_basicdraw(u); }
    return n;
}

/* ERLANG(a,b,U): mean 1/a, b phases (b need not be integral; the
 * fractional phase contributes proportionally). */
double simula_erlang(double a, double b, int64_t* u) {
    if (a <= 0.0 || b <= 0.0) return 0.0;
    int64_t k = (int64_t)b;
    double frac = b - (double)k;
    double sum = 0.0;
    for (int64_t i = 0; i < k; i++)
        sum -= log(simula_basicdraw(u));
    if (frac > 0.0)
        sum -= frac * log(simula_basicdraw(u));
    return sum / (a * b);
}

int64_t simula_draw(double p, int64_t* u) {
    return simula_basicdraw(u) < p ? 1 : 0;
}

/* DISCRETE(A,U): A(lo:hi) is a cumulative distribution; result is the
 * smallest index i with A(i) > basic draw, or hi+1 if none. */
int64_t simula_discrete(const double* a, int64_t lo, int64_t n, int64_t* u) {
    double d = simula_basicdraw(u);
    for (int64_t i = 0; i < n; i++)
        if (a[i] > d) return lo + i;
    return lo + n;
}

/* LINEAR(A,B,U): inverse linear interpolation. A = function values,
 * B = nondecreasing cumulative probabilities (B(lo)=0, B(hi)=1). */
double simula_linear(const double* a, const double* b, int64_t n, int64_t* u) {
    double d = simula_basicdraw(u);
    for (int64_t i = 1; i < n; i++) {
        if (d <= b[i]) {
            double db = b[i] - b[i - 1];
            if (db <= 0.0) return a[i];
            return a[i - 1] + (a[i] - a[i - 1]) * (d - b[i - 1]) / db;
        }
    }
    return n > 0 ? a[n - 1] : 0.0;
}

/* HISTD(A,U): A(lo:hi) holds nonnegative frequencies; pick an index
 * with probability proportional to its frequency. */
int64_t simula_histd(const double* a, int64_t lo, int64_t n, int64_t* u) {
    double total = 0.0;
    for (int64_t i = 0; i < n; i++) total += a[i];
    if (total <= 0.0) return lo;
    double d = simula_basicdraw(u) * total;
    double run = 0.0;
    for (int64_t i = 0; i < n; i++) {
        run += a[i];
        if (d < run) return lo + i;
    }
    return lo + n - 1;
}

/* HISTO(A,B,c,w): histogram update. B(lo:hi) are ascending interval
 * limits; add w to the A entry for the interval containing c
 * (A needs one more element than B; the last catches c > all limits). */
void simula_histo(double* a, int64_t an, const double* b, int64_t bn,
                  double c, double w) {
    int64_t i = 0;
    while (i < bn && c > b[i]) i++;
    if (i < an) a[i] += w;
}

/* ================================================================
 * SIMULATION: event list (SQS) and process scheduling
 *
 * Every switch is main->process (resume) or process->main (detach):
 * scheduling statements only edit the SQS; HOLD/PASSIVATE/body-end
 * yield. The main program acts as the main process: HOLD in main
 * drives the event loop directly.
 * ================================================================ */

typedef struct SimNotice {
    void* obj;                 /* process object (struct: [vtable, coro, ...]) */
    double evtime;
    uint64_t seq;              /* FIFO rank within equal evtime */
    struct SimNotice* next;
} SimNotice;

typedef struct SimProcState {
    void* obj;
    int terminated;
    struct SimProcState* next;
} SimProcState;

static SimNotice* sim_sqs = NULL;
static SimProcState* sim_procs = NULL;
static double sim_time_v = 0.0;
static void* sim_current_obj = NULL;   /* NULL = main program */
static uint64_t sim_seq_counter = 0;

static SimulaCoro* sim_coro_of(void* obj) {
    return (SimulaCoro*)((void**)obj)[1];
}

static SimProcState* sim_state_of(void* obj) {
    for (SimProcState* p = sim_procs; p; p = p->next)
        if (p->obj == obj) return p;
    SimProcState* p = (SimProcState*)malloc(sizeof *p);
    p->obj = obj; p->terminated = 0; p->next = sim_procs; sim_procs = p;
    return p;
}

static void sim_remove_notice(void* obj) {
    SimNotice** pp = &sim_sqs;
    while (*pp) {
        if ((*pp)->obj == obj) {
            SimNotice* dead = *pp;
            *pp = dead->next;
            free(dead);
            return;  /* a process has at most one notice */
        }
        pp = &(*pp)->next;
    }
}

static int sim_has_notice(void* obj) {
    for (SimNotice* n = sim_sqs; n; n = n->next)
        if (n->obj == obj) return 1;
    return 0;
}

/* prior != 0 ranks the notice before others with the same evtime */
static void sim_insert(void* obj, double t, int prior) {
    if (t < sim_time_v) t = sim_time_v;
    SimNotice* n = (SimNotice*)malloc(sizeof *n);
    n->obj = obj; n->evtime = t; n->seq = ++sim_seq_counter;
    SimNotice** pp = &sim_sqs;
    while (*pp && ((*pp)->evtime < t || ((*pp)->evtime == t && !prior)))
        pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
}

double simula_sim_time(void) { return sim_time_v; }
void* simula_sim_current(void) { return sim_current_obj; }

int64_t simula_sim_idle(void* obj) {
    if (obj == NULL) return 0;
    if (obj == sim_current_obj) return 0;
    return sim_has_notice(obj) ? 0 : 1;
}

int64_t simula_sim_terminated(void* obj) {
    if (obj == NULL) return 0;
    for (SimProcState* p = sim_procs; p; p = p->next)
        if (p->obj == obj) return p->terminated;
    return 0;
}

double simula_sim_evtime(void* obj) {
    for (SimNotice* n = sim_sqs; n; n = n->next)
        if (n->obj == obj) return n->evtime;
    if (obj == sim_current_obj) return sim_time_v;
    return 0.0;
}

void simula_sim_cancel(void* obj) {
    if (obj != NULL) sim_remove_notice(obj);
}

/* ACTIVATE: schedule only if idle. REACTIVATE: always re-schedule. */
void simula_sim_activate(void* obj, double t, int64_t prior, int64_t reactivate) {
    if (obj == NULL) return;
    if (simula_sim_terminated(obj)) return;
    if (sim_has_notice(obj) || obj == sim_current_obj) {
        if (!reactivate) return;
        sim_remove_notice(obj);
    }
    sim_state_of(obj);
    sim_insert(obj, t, (int)prior);
}

/* ACTIVATE ... BEFORE/AFTER other: adopt the other's evtime and rank. */
void simula_sim_activate_rel(void* obj, void* other, int64_t before,
                             int64_t reactivate) {
    if (obj == NULL || other == NULL) return;
    if (simula_sim_terminated(obj)) return;
    SimNotice* on = NULL;
    for (SimNotice* n = sim_sqs; n; n = n->next)
        if (n->obj == other) { on = n; break; }
    if (on == NULL) return;  /* other idle: no effect (standard) */
    if (sim_has_notice(obj) || obj == sim_current_obj) {
        if (!reactivate) return;
        sim_remove_notice(obj);
        /* re-find: removal may have changed links */
        on = NULL;
        for (SimNotice* n = sim_sqs; n; n = n->next)
            if (n->obj == other) { on = n; break; }
        if (on == NULL) return;
    }
    sim_state_of(obj);
    SimNotice* nn = (SimNotice*)malloc(sizeof *nn);
    nn->obj = obj; nn->evtime = on->evtime; nn->seq = ++sim_seq_counter;
    if (before) {
        SimNotice** pp = &sim_sqs;
        while (*pp && *pp != on) pp = &(*pp)->next;
        nn->next = on; *pp = nn;
    } else {
        nn->next = on->next; on->next = nn;
    }
}

/* Run queued events from the main context until the SQS is exhausted or
 * the next event lies beyond `until` (pass INFINITY to drain). */
static void sim_main_loop(double until) {
    while (sim_sqs && sim_sqs->evtime <= until) {
        SimNotice* n = sim_sqs;
        sim_sqs = n->next;
        sim_time_v = n->evtime;
        void* obj = n->obj;
        free(n);
        if (simula_sim_terminated(obj)) continue;
        sim_current_obj = obj;
        simula_coro_resume(sim_coro_of(obj));
        sim_current_obj = NULL;
    }
    if (until != INFINITY && sim_time_v < until) sim_time_v = until;
}

/* HOLD(dt): from a process, reschedule self and yield to the scheduler;
 * from the main program, drive the event loop for dt time units. */
void simula_sim_hold(double dt) {
    if (dt < 0.0) dt = 0.0;
    if (sim_current_obj == NULL) {
        sim_main_loop(sim_time_v + dt);
        return;
    }
    void* self = sim_current_obj;
    sim_insert(self, sim_time_v + dt, 0);
    simula_coro_detach(sim_coro_of(self));
}

/* PASSIVATE: from a process, yield without rescheduling; from the main
 * program, drain the whole event list. */
void simula_sim_passivate(void) {
    if (sim_current_obj == NULL) {
        sim_main_loop(INFINITY);
        return;
    }
    void* self = sim_current_obj;
    simula_coro_detach(sim_coro_of(self));
}

/* End of a process body: mark terminated and yield forever. */
void simula_sim_terminate(void* obj) {
    sim_state_of(obj)->terminated = 1;
    sim_remove_notice(obj);
    for (;;) simula_coro_detach(sim_coro_of(obj));
}

/* Direct activation (plain ACTIVATE X): per the standard X becomes current
 * at once and the activator continues only after X yields. From a process
 * this is exact: schedule self's continuation, schedule X ahead of it, and
 * yield to the scheduler. From the main program we drain all current-time
 * events (X runs first among them) before main continues. */
void simula_sim_activate_now(void* obj, int64_t reactivate) {
    if (obj == NULL) return;
    if (simula_sim_terminated(obj)) return;
    if (sim_has_notice(obj) || obj == sim_current_obj) {
        if (!reactivate) return;
        if (obj == sim_current_obj) return; /* reactivating self: no-op here */
        sim_remove_notice(obj);
    }
    sim_state_of(obj);
    if (sim_current_obj == NULL) {
        sim_insert(obj, sim_time_v, 1);
        sim_main_loop(sim_time_v);
        return;
    }
    void* self = sim_current_obj;
    sim_insert(self, sim_time_v, 1);  /* continuation first among existing */
    sim_insert(obj, sim_time_v, 1);   /* X ahead of the continuation */
    simula_coro_detach(sim_coro_of(self));
}
