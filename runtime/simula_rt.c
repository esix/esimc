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
#include <math.h>

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
    int state;      /* 0=new, 1=running, 2=detached, 3=terminated */
    int via_resume; /* 1 = operating via RESUME; 0 = attached (NEW/CALL) */
};

static void (*_pending_func)(void*);
static void* _pending_arg;
static SimulaCoro* _pending_coro;
static SimulaCoro* _coro_current = NULL;  /* currently running coroutine; NULL = main */
static void* _main_fiber = NULL;          /* the main program's fiber */

static VOID CALLBACK coro_fiber_proc(LPVOID param) {
    void (*func)(void*) = _pending_func;
    void* arg = _pending_arg;
    SimulaCoro* self = _pending_coro;

    func(arg);

    self->state = 3;
    /* Attached operation returns to the attacher; a component operating via
     * RESUME terminates into the main program. */
    if (self->via_resume && _main_fiber)
        SwitchToFiber(_main_fiber);
    else
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
    SimulaCoro* prev = _coro_current;
    coro->state = 1;
    coro->via_resume = 0;  /* initial operation is ATTACHED to the creator */
    _coro_current = coro;
    if (prev == NULL) _main_fiber = this_fiber;
    SwitchToFiber(coro->fiber);
    _coro_current = prev;
}

void simula_coro_detach(SimulaCoro* coro) {
    coro->state = 2;
    if (coro->via_resume && _main_fiber)
        SwitchToFiber(_main_fiber);
    else
        SwitchToFiber(coro->caller_fiber);
}

void simula_coro_resume(SimulaCoro* coro) {
    if (coro->state == 3) {
        fprintf(stderr, "Runtime error: RESUME of a terminated object\n");
        exit(1);
    }
    if (coro->state != 2) {
        fprintf(stderr, "Runtime error: RESUME of an object that is not detached\n");
        exit(1);
    }
    SimulaCoro* prev = _coro_current;
    coro->state = 1;
    coro->via_resume = 1;
    _coro_current = coro;
    if (prev == NULL) {
        // Main program resuming a coroutine.
        coro->caller_fiber = GetCurrentFiber();
        _main_fiber = coro->caller_fiber;
    } else if (prev->via_resume) {
        // A detached component resuming another: symmetric transfer; the
        // resumed component's DETACH/termination goes to the main fiber.
        prev->state = 2;
    } else {
        // An ATTACHED component executing RESUME suspends the main component;
        // its reactivation point is this fiber, right after the switch.
        _main_fiber = prev->fiber;
    }
    SwitchToFiber(coro->fiber);
    _coro_current = prev;
}

/* CALL(X): attach X to the caller so X's next DETACH returns HERE (see the POSIX
 * version for the full contract). */
void simula_coro_call(SimulaCoro* coro) {
    if (coro->state == 3) {
        fprintf(stderr, "Runtime error: CALL of a terminated object\n");
        exit(1);
    }
    if (coro->state != 2) {
        fprintf(stderr, "Runtime error: CALL of an object that is not detached\n");
        exit(1);
    }
    SimulaCoro* prev = _coro_current;
    coro->state = 1;
    coro->via_resume = 0;  /* CALL attaches: DETACH/termination returns here */
    _coro_current = coro;
    coro->caller_fiber = GetCurrentFiber();
    if (prev == NULL) _main_fiber = coro->caller_fiber;
    SwitchToFiber(coro->fiber);
    _coro_current = prev;
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
    int state;      /* 0=new, 1=running, 2=detached, 3=terminated */
    int via_resume; /* 1 = operating as a detached component (RESUME);
                       0 = attached (initial operation at NEW, or CALL) */
};

static void (*_pending_func)(void*);
static void* _pending_arg;
static SimulaCoro* _pending_coro;
static SimulaCoro* _coro_current = NULL;  /* currently running coroutine; NULL = main */
/* The main program's most recent suspension point. Every time main transfers
 * control away (NEW / RESUME / CALL), it saves its context somewhere; this
 * points at that save. A detached component's DETACH or termination transfers
 * HERE (Simula: control passes to the main program), never back into a stale
 * frame of whoever resumed it. */
static ucontext_t* _main_react = NULL;
/* Dedicated save slot for the main program suspending via RESUME. It must NOT
 * live in the resumed coroutine's caller_ctx: a later CALL of that (by then
 * detached) coroutine reuses caller_ctx and would clobber main's context. */
static ucontext_t _main_resume_ctx;
/* Head (detached component) of the currently operating chain; NULL = the main
 * program's component. Objects attached under it (NEW/CALL) share the head. */
static SimulaCoro* _chain_head = NULL;

static void coro_trampoline(void) {
    void (*func)(void*) = _pending_func;
    void* arg = _pending_arg;
    SimulaCoro* self = _pending_coro;

    func(arg);

    self->state = 3;
    /* Attached operation returns to the attacher; a component operating via
     * RESUME terminates into the main program's reactivation point. */
    if (self->via_resume && _main_react)
        setcontext(_main_react);
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

    SimulaCoro* prev = _coro_current;
    SimulaCoro* prev_head = _chain_head;
    coro->state = 1;
    coro->via_resume = 0;  /* initial operation is ATTACHED to the creator */
    _coro_current = coro;
    if (prev == NULL) _main_react = &coro->caller_ctx;
    swapcontext(&coro->caller_ctx, &coro->context);
    _coro_current = prev;
    _chain_head = prev_head;
}

void simula_coro_detach(SimulaCoro* coro) {
    coro->state = 2;
    /* Detaching while attached returns to the attacher. Detaching while
     * operating via RESUME suspends this component and transfers to the main
     * program's current reactivation point. */
    if (coro->via_resume && _main_react)
        swapcontext(&coro->context, _main_react);
    else
        swapcontext(&coro->context, &coro->caller_ctx);
}

void simula_coro_resume(SimulaCoro* coro) {
    if (coro->state == 3) {
        fprintf(stderr, "Runtime error: RESUME of a terminated object\n");
        exit(1);
    }
    if (coro->state != 2) {
        fprintf(stderr, "Runtime error: RESUME of an object that is not detached\n");
        exit(1);
    }
    SimulaCoro* prev = _coro_current;
    SimulaCoro* prev_head = _chain_head;
    coro->state = 1;
    coro->via_resume = 1;
    _coro_current = coro;
    _chain_head = coro;
    if (prev == NULL) {
        // Main program resuming a coroutine: main's reactivation point is here.
        _main_react = &_main_resume_ctx;
        swapcontext(&_main_resume_ctx, &coro->context);
    } else if (prev->via_resume) {
        // A detached component resuming another: the resumer becomes detached so
        // it can be resumed back (Simula RESUME is a symmetric transfer). The
        // resumed component's DETACH/termination goes to _main_react.
        prev->state = 2;
        swapcontext(&prev->context, &coro->context);
    } else if (prev_head) {
        // RESUME from inside an object ATTACHED (CALL/NEW) to a detached
        // component: the whole component suspends with its reactivation point
        // here, and becomes resumable through its head. The main program's
        // reactivation point is untouched.
        prev_head->state = 2;
        swapcontext(&prev_head->context, &coro->context);
    } else {
        // An ATTACHED component (operating under CALL/NEW as part of the main
        // component's chain) executing RESUME suspends the main component; its
        // reactivation point is right after this resume statement.
        _main_react = &prev->context;
        swapcontext(&prev->context, &coro->context);
    }
    _coro_current = prev;
    _chain_head = prev_head;
}

/* CALL(X): transfer control to detached X, attaching it to the caller so that
 * X's next DETACH returns HERE — unlike RESUME, whose DETACH returns to X's own
 * detach point and which leaves the resumer detached. The caller simply waits
 * for X to detach (or terminate), whether the caller is a process or main. */
void simula_coro_call(SimulaCoro* coro) {
    if (coro->state == 3) {
        fprintf(stderr, "Runtime error: CALL of a terminated object\n");
        exit(1);
    }
    if (coro->state != 2) {
        fprintf(stderr, "Runtime error: CALL of an object that is not detached\n");
        exit(1);
    }
    SimulaCoro* prev = _coro_current;
    SimulaCoro* prev_head = _chain_head;
    coro->state = 1;
    coro->via_resume = 0;  /* CALL attaches: DETACH/termination returns here */
    _coro_current = coro;
    if (prev == NULL) _main_react = &coro->caller_ctx;
    swapcontext(&coro->caller_ctx, &coro->context);
    _coro_current = prev;
    _chain_head = prev_head;
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
 * Platform-independent: runtime safety checks (checked mode)
 *
 * These are invoked from generated code on the failing path only; each
 * prints a source-located diagnostic and aborts. A line of 0 means the
 * front-end had no location for the site, so the line is omitted.
 * ================================================================ */

void simula_math_error(int64_t code, int64_t line) {
    const char* what =
        code == 1 ? "SQRT of a negative argument" :
        code == 2 ? "logarithm of a non-positive argument" :
        code == 3 ? "EXP argument too large" :
        code == 4 ? "** undefined for these operands (zero or negative base)" :
        code == 5 ? "REAL value out of INTEGER range in conversion" :
                    "arithmetic domain error";
    if (line > 0)
        fprintf(stderr, "Runtime error at line %ld: %s\n", (long)line, what);
    else
        fprintf(stderr, "Runtime error: %s\n", what);
    exit(1);
}

void simula_virtual_missing(int64_t line) {
    if (line > 0)
        fprintf(stderr, "Runtime error at line %ld: call of a virtual procedure "
                "with no matching definition\n", (long)line);
    else
        fprintf(stderr, "Runtime error: call of an unmatched virtual procedure\n");
    exit(1);
}

void simula_name_assign_error(int64_t line) {
    /* Store through a NAME parameter whose actual is an expression, not a
     * variable (Simula Standard 4.6.6 forbids the correspondence). */
    if (line > 0)
        fprintf(stderr, "Runtime error at line %ld: assignment to a name "
                "parameter whose actual is not a variable\n", (long)line);
    else
        fprintf(stderr, "Runtime error: assignment to a name parameter "
                "whose actual is not a variable\n");
    exit(1);
}

void simula_qua_error(int64_t line) {
    /* X QUA C where X's class is neither C nor prefixed by C. */
    if (line > 0)
        fprintf(stderr, "Runtime error at line %ld: QUA qualification error "
                "(object is not of the target class)\n", (long)line);
    else
        fprintf(stderr, "Runtime error: QUA qualification error\n");
    exit(1);
}

void simula_expi_error(int64_t line) {
    /* Simula 3.5: integer**integer (EXPI) is undefined for a negative exponent
       (use a real base, e.g. 2.0 ** -3, for the reciprocal form). */
    if (line > 0)
        fprintf(stderr, "Runtime error at line %ld: integer ** undefined here "
                "(negative exponent or 0**0; use a REAL base)\n", (long)line);
    else
        fprintf(stderr, "Runtime error: integer ** undefined here (negative exponent or 0**0)\n");
    exit(1);
}

void simula_div_zero(int64_t line) {
    if (line > 0)
        fprintf(stderr, "Runtime error at line %ld: division by zero\n",
                (long)line);
    else
        fprintf(stderr, "Runtime error: division by zero\n");
    exit(1);
}

void simula_bounds_error(int64_t idx, int64_t lo, int64_t hi, int64_t line) {
    if (line > 0)
        fprintf(stderr,
                "Runtime error at line %ld: array index %ld out of bounds [%ld:%ld]\n",
                (long)line, (long)idx, (long)lo, (long)hi);
    else
        fprintf(stderr,
                "Runtime error: array index %ld out of bounds [%ld:%ld]\n",
                (long)idx, (long)lo, (long)hi);
    exit(1);
}

void simula_nil_ref(int64_t line) {
    if (line > 0)
        fprintf(stderr, "Runtime error at line %ld: NONE-reference access (no object)\n",
                (long)line);
    else
        fprintf(stderr, "Runtime error: NONE-reference access (no object)\n");
    exit(1);
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

/* Wrap a C string as a TEXT over its (shared) storage WITHOUT copying. Only
 * for buffers that are already writable or never written (e.g. SYSIN.IMAGE). */
SimulaText* simula_text_lit(const char* cstr) {
    if (cstr == NULL) return NULL;
    int64_t n = (int64_t)strlen(cstr);
    return st_new((char*)cstr, 0, n, 0, n);
}

/* A TEXT literal: copy into a fresh writable frame so PUTCHAR/:=/PUTINT into
 * the literal (a writable text object in Simula) don't hit read-only .rodata. */
SimulaText* simula_text_dup(const char* cstr) {
    if (cstr == NULL) return NULL;
    int64_t n = (int64_t)strlen(cstr);
    SimulaText* r = st_fresh(n);
    if (n > 0) memcpy(r->frame, cstr, (size_t)n);
    return r;
}

SimulaText* simula_blanks(int64_t n) {
    return st_fresh(n);
}

/* UPCASE(t) / LOWCASE(t): convert every letter of the text in place (non-letters
 * unchanged) and return the same text reference. */
SimulaText* simula_upcase(SimulaText* t) {
    if (t) for (int64_t i = 0; i < t->length; i++) {
        char c = t->frame[t->start + i];
        if (c >= 'a' && c <= 'z') t->frame[t->start + i] = (char)(c - 32);
    }
    return t;
}

SimulaText* simula_lowcase(SimulaText* t) {
    if (t) for (int64_t i = 0; i < t->length; i++) {
        char c = t->frame[t->start + i];
        if (c >= 'A' && c <= 'Z') t->frame[t->start + i] = (char)(c + 32);
    }
    return t;
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
    /* Standard: T.SUB(i,n) requires i >= 1, n >= 0 and i+n-1 <= T.LENGTH. */
    int64_t tlen = s ? s->length : 0;
    if (start < 1 || len < 0 || start - 1 + len > tlen) {
        fprintf(stderr,
                "Runtime error: SUB(%ld,%ld) out of range for text length %ld\n",
                (long)start, (long)len, (long)tlen);
        exit(1);
    }
    if (s == NULL) return st_fresh(0);
    return st_new(s->frame, s->start + (start - 1), len, 0, s->framelen);
}

/* MAIN: the whole underlying frame (per the standard, the main text). */
SimulaText* simula_text_main(SimulaText* s) {
    if (s == NULL) return NULL;
    return st_new(s->frame, 0, s->framelen, 0, s->framelen);
}

/* Content equality (= operator): same length and same characters. A length-0
 * text — NOTEXT, "", COPY(""), BLANKS(0) — is content-equal to any other
 * length-0 text (the empty character sequences match). */
int64_t simula_text_eq(SimulaText* a, SimulaText* b) {
    int64_t la = a ? a->length : 0;
    int64_t lb = b ? b->length : 0;
    if (la != lb) return 0;
    if (la == 0) return 1;
    return memcmp(a->frame + a->start, b->frame + b->start, (size_t)la) == 0 ? 1 : 0;
}

/* Reference identity (== operator): the same text object (same frame, start and
 * length; the cursor is not part of identity). All texts of length 0 (NOTEXT and
 * every "") denote the one empty text, so they are identical to each other. */
int64_t simula_text_ref_eq(SimulaText* a, SimulaText* b) {
    if (a == b) return 1;
    int64_t la = a ? a->length : 0;
    int64_t lb = b ? b->length : 0;
    if (la == 0 && lb == 0) return 1;
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
    /* Standard: POS := if i < 1 or i > LENGTH+1 then LENGTH+1 else i,
     * so any out-of-range argument (either side) parks the cursor at the end
     * (MORE becomes false). */
    if (s == NULL) return;
    int64_t z = p - 1;
    if (z < 0 || z > s->length) z = s->length;
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

/* De-editing (Simula Standard 8.6): the numeric item is located from the
 * START of the text (POS is an output, not an input); the sign part is
 * BLANKS [SIGN] BLANKS; POS is left one past the item; finding no item of
 * the requested form is a runtime error. */

static void st_dedit_error(const char* what) {
    fprintf(stderr, "Runtime error: %s: no numeric item in text\n", what);
    exit(1);
}

/* SIGN-PART = BLANKS [SIGN] BLANKS. Returns the index after it. */
static int64_t st_signpart(const char* f, int64_t n, int* neg) {
    int64_t i = 0;
    *neg = 0;
    while (i < n && (f[i] == ' ' || f[i] == '\t')) i++;
    if (i < n && (f[i] == '-' || f[i] == '+')) {
        *neg = (f[i] == '-');
        i++;
        while (i < n && (f[i] == ' ' || f[i] == '\t')) i++;
    }
    return i;
}

int64_t simula_text_getint(SimulaText* t) {
    if (t == NULL) st_dedit_error("GETINT");
    const char* f = t->frame + t->start;
    int neg;
    int64_t i = st_signpart(f, t->length, &neg);
    int64_t v = 0, seen = 0;
    while (i < t->length && f[i] >= '0' && f[i] <= '9') { v = v * 10 + (f[i] - '0'); i++; seen = 1; }
    if (!seen) st_dedit_error("GETINT");
    t->pos = i;
    return neg ? -v : v;
}

double simula_text_getreal(SimulaText* t) {
    if (t == NULL) st_dedit_error("GETREAL");
    const char* f = t->frame + t->start;
    int neg;
    int64_t i = st_signpart(f, t->length, &neg);
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
    /* A bare-exponent numeral like "&2" means 1e2 (Simula de-editing). */
    int prepended = 0;
    char fixed[66];
    if (buf[0] == 'e' || buf[0] == 'E') {
        snprintf(fixed, sizeof fixed, "1%s", buf);
        memcpy(buf, fixed, strlen(fixed) + 1);
        prepended = 1;
    }
    char* end = NULL;
    double v = strtod(buf, &end);
    if (end == NULL || end == buf) st_dedit_error("GETREAL");
    t->pos = startScan + (int64_t)(end - buf) - prepended;
    return neg ? -v : v;
}

int64_t simula_text_getfrac(SimulaText* t) {
    if (t == NULL) st_dedit_error("GETFRAC");
    const char* f = t->frame + t->start;
    int neg;
    int64_t i = st_signpart(f, t->length, &neg);
    int64_t v = 0, seen = 0;
    while (i < t->length) {
        char c = f[i];
        if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); seen = 1; i++; }
        else if ((c == ' ' || c == '.') && seen &&
                 i + 1 < t->length && f[i+1] >= '0' && f[i+1] <= '9') i++;
        else break;
    }
    if (!seen) st_dedit_error("GETFRAC");
    t->pos = i;
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
/* Simula rounding is ties AWAY FROM ZERO; C's printf rounds ties to even.
 * Pre-round to d decimals so 0.125 -> 0.13, not 0.12. */
static double round_away(double v, int64_t d) {
    double s = pow(10.0, (double)d);
    double x = v * s;
    if (x >= 9.007199254740992e15 || x <= -9.007199254740992e15) return v;
    return copysign(floor(fabs(x) + 0.5), v) / s;
}

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
        /* Grouped item: fraction digits are also grouped three by three. */
        for (int i = intDigits; i < n && j < 62; i++) {
            if (i > intDigits && (i - intDigits) % 3 == 0) out[j++] = ' ';
            out[j++] = digits[i];
        }
    }
    out[j] = '\0';
}

/* The numeric editing procedures edit the item right-justified across the whole
 * frame and then set POS to LENGTH+1 (internally pos == length), per the Simula
 * standard — mirroring how the de-editing GET procedures advance POS. */
void simula_text_putint(SimulaText* t, int64_t v) {
    if (t == NULL) return;
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    edit_into(t->frame + t->start, t->length, buf);
    t->pos = t->length;
}

void simula_text_putfix(SimulaText* t, double r, int64_t d) {
    if (t == NULL) return;
    char buf[64];
    if (d < 0) d = 0;
    snprintf(buf, sizeof buf, "%.*f", (int)d, round_away(r, d));
    edit_into(t->frame + t->start, t->length, buf);
    t->pos = t->length;
}

void simula_text_putreal(SimulaText* t, double r, int64_t d) {
    if (t == NULL) return;
    char buf[64];
    int prec = (int)(d > 0 ? d - 1 : 0);
    snprintf(buf, sizeof buf, "%.*e", prec, r);
    for (char* p = buf; *p; p++)
        if (*p == 'e' || *p == 'E') *p = '&';
    edit_into(t->frame + t->start, t->length, buf);
    t->pos = t->length;
}

void simula_text_putfrac(SimulaText* t, int64_t v, int64_t d) {
    if (t == NULL) return;
    char out[64];
    frac_to_str(v, d, out);
    edit_into(t->frame + t->start, t->length, out);
    t->pos = t->length;
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
    /* IMAGE is the full fixed-width buffer: the external record is left-
     * justified and blank-padded to the image length (standard INIMAGE), so
     * e.g. INTEXT(width) consumes exactly one image. */
    char* line = simula_inreadline(handle, maxlen);
    if (line == NULL) return NULL;
    int64_t n = (int64_t)strlen(line);
    if (maxlen <= 0) maxlen = 132;
    if (n < maxlen) {
        char* padded = (char*)malloc((size_t)maxlen);
        memcpy(padded, line, (size_t)n);
        memset(padded + n, ' ', (size_t)(maxlen - n));
        free(line);
        return st_new(padded, 0, maxlen, 0, maxlen);
    }
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
    if (f == NULL) return 0.0;
    int c;
    while ((c = fgetc(f)) != EOF &&
           (c == ' ' || c == '\t' || c == '\n' || c == '\r'));
    char buf[64]; size_t j = 0;
    while (c != EOF && j < 62 &&
           ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' ||
            c == 'e' || c == 'E' || c == '&')) {
        buf[j++] = (char)(c == '&' ? 'e' : c);
        c = fgetc(f);
    }
    if (c != EOF) ungetc(c, f);
    buf[j] = '\0';
    /* Bare-exponent numeral like "&2" means 1e2. */
    if (buf[0] == 'e' || buf[0] == 'E' ||
        ((buf[0] == '+' || buf[0] == '-') && (buf[1] == 'e' || buf[1] == 'E'))) {
        char fixed[66];
        int neg = buf[0] == '-';
        snprintf(fixed, sizeof fixed, "%s1%s", neg ? "-" : "",
                 buf + (buf[0] == 'e' || buf[0] == 'E' ? 0 : 1));
        memcpy(buf, fixed, strlen(fixed) + 1);
    }
    return strtod(buf, NULL);
}

/* True when a non-blank character remains at/after the cursor. Used by INFILE's
 * item procedures to decide whether the current image still has an item. */
int64_t simula_text_moreitem(SimulaText* t) {
    if (t == NULL) return 0;
    const char* f = t->frame + t->start;
    for (int64_t i = t->pos; i < t->length; i++)
        if (f[i] != ' ' && f[i] != '\t') return 1;
    return 0;
}

/* INTEXT(n): the next n characters from the cursor (clamped to what remains),
 * advancing the cursor past them. Returns an alias, like SUB. */
SimulaText* simula_text_intext(SimulaText* t, int64_t n) {
    if (t == NULL) return st_fresh(0);
    if (n < 0) n = 0;
    int64_t avail = t->length - t->pos;
    if (avail < 0) avail = 0;
    if (n > avail) n = avail;
    SimulaText* r = st_new(t->frame, t->start + t->pos, n, 0, t->framelen);
    t->pos += n;
    return r;
}

void simula_file_outfix(int64_t handle, double v, int64_t d, int64_t w) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f == NULL) return;
    char buf[64];
    if (d < 0) d = 0;
    snprintf(buf, sizeof buf, "%.*f", (int)d, round_away(v, d));
    int n = (int)strlen(buf);
    if (w <= 0) { fputs(buf, f); return; }
    if (n > w) { for (int64_t i = 0; i < w; i++) fputc('*', f); return; }
    fprintf(f, "%*s", (int)w, buf);
}

void simula_file_outreal(int64_t handle, double v, int64_t d, int64_t w) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f == NULL) return;
    char buf[64];
    int prec = (int)(d > 0 ? d - 1 : 0);
    snprintf(buf, sizeof buf, "%.*e", prec, v);
    for (char* p = buf; *p; p++) if (*p == 'e' || *p == 'E') *p = '&';
    int n = (int)strlen(buf);
    if (w <= 0) { fputs(buf, f); return; }
    if (n > w) { for (int64_t i = 0; i < w; i++) fputc('*', f); return; }
    fprintf(f, "%*s", (int)w, buf);
}

void simula_file_outfrac(int64_t handle, int64_t v, int64_t d, int64_t w) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f == NULL) return;
    char out[64];
    frac_to_str(v, d, out);
    int width = (int)(w > 0 && w < 60 ? w : 0);
    if (width == 0) { fputs(out, f); return; }
    char frame[64];
    edit_into(frame, width, out);
    fwrite(frame, 1, (size_t)width, f);
}

void simula_file_outchar(int64_t handle, int64_t c) {
    FILE* f = (FILE*)(intptr_t)handle;
    if (f != NULL) fputc((int)(unsigned char)c, f);
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

/* OUTFIX(v, d, w): fixed-point item with d decimals, right-justified in a field
 * of width w, filling the field with '*' on overflow (like OUTINT/OUTREAL). */
void simula_outfix(double v, int64_t d, int64_t w) {
    char buf[64];
    if (d < 0) d = 0;
    snprintf(buf, sizeof buf, "%.*f", (int)d, round_away(v, d));
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

/* INCHAR: next character from SYSIN; at end of file the standard's image is
 * filled with the EM character (ISO rank 25). */
int64_t simula_inchar(void) {
    int c = getchar();
    return c == EOF ? 25 : c;
}

/* INREAL: read a real item from SYSIN. Accepts the lowten character '&' as
 * the exponent mark (the same mark OUTREAL emits), as well as e/E. */
double simula_inreal(void) {
    int c;
    while ((c = getchar()) != EOF &&
           (c == ' ' || c == '\t' || c == '\n' || c == '\r'));
    char buf[64]; size_t j = 0;
    while (c != EOF && j < 62 &&
           ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' ||
            c == 'e' || c == 'E' || c == '&')) {
        buf[j++] = (char)(c == '&' ? 'e' : c);
        c = getchar();
    }
    if (c != EOF) ungetc(c, stdin);
    buf[j] = '\0';
    if (buf[0] == 'e' || buf[0] == 'E' ||
        ((buf[0] == '+' || buf[0] == '-') && (buf[1] == 'e' || buf[1] == 'E'))) {
        char fixed[66];
        int neg = buf[0] == '-';
        snprintf(fixed, sizeof fixed, "%s1%s", neg ? "-" : "", buf + (buf[0] == 'e' || buf[0] == 'E' ? 0 : 1));
        memcpy(buf, fixed, strlen(fixed) + 1);
    }
    return strtod(buf, NULL);
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
    fprintf(stderr, "Runtime error: EVTIME of an idle process (no event notice)\n");
    exit(1);
}

/* NEXTEV: the process of the event notice following this process's notice.
 * The operating process's own notice is (conceptually) the SQS head, so its
 * NEXTEV is the first queued notice. Idle/last-scheduled processes give NULL. */
void* simula_sim_nextev(void* obj) {
    if (obj == sim_current_obj) return sim_sqs ? sim_sqs->obj : NULL;
    for (SimNotice* n = sim_sqs; n; n = n->next)
        if (n->obj == obj) return n->next ? n->next->obj : NULL;
    return NULL;
}

void simula_sim_cancel(void* obj) {
    if (obj == NULL) return;
    /* Standard: cancel(current) is passivate. */
    if (obj == sim_current_obj) { simula_sim_passivate(); return; }
    sim_remove_notice(obj);
}

/* ACTIVATE: schedule only if idle. REACTIVATE: always re-schedule. */
void simula_sim_activate(void* obj, double t, int64_t prior, int64_t reactivate) {
    if (obj == NULL) return;
    if (simula_sim_terminated(obj)) return;
    int is_self = (obj == sim_current_obj);
    if (sim_has_notice(obj) || is_self) {
        if (!reactivate) return;
        sim_remove_notice(obj);
    }
    sim_state_of(obj);
    sim_insert(obj, t, (int)prior);
    /* REACTIVATE current AT/DELAY t reschedules self, so the active phase must
       terminate and control pass to the scheduler — exactly hold(t). */
    if (is_self) simula_coro_detach(sim_coro_of(obj));
}

/* ACTIVATE ... BEFORE/AFTER other: adopt the other's evtime and rank. */
void simula_sim_activate_rel(void* obj, void* other, int64_t before,
                             int64_t reactivate) {
    if (obj == NULL || other == NULL) return;
    if (simula_sim_terminated(obj)) return;
    int is_self = (obj == sim_current_obj);
    SimNotice* on = NULL;
    for (SimNotice* n = sim_sqs; n; n = n->next)
        if (n->obj == other) { on = n; break; }
    if (on == NULL) {
        /* Other is idle: plain ACTIVATE has no effect; REACTIVATE removes X's
           event notice, leaving X passive (standard ACTIVAT: EVENT :- none). */
        if (reactivate) {
            sim_remove_notice(obj);
            if (is_self) simula_coro_detach(sim_coro_of(obj));
        }
        return;
    }
    if (sim_has_notice(obj) || is_self) {
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
    /* REACTIVATE current BEFORE/AFTER other reschedules self -> yield. */
    if (is_self) simula_coro_detach(sim_coro_of(obj));
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
        /* Main's event notice is removed, so it can continue only if some
         * process reactivates it; run the remaining events, and if the SQS
         * empties the simulation is stuck (standard: error termination). */
        sim_main_loop(INFINITY);
        fprintf(stderr, "Runtime error: PASSIVATE left the sequencing set empty (no next event)\n");
        exit(1);
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
