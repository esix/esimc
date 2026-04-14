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
