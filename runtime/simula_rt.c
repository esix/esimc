#define _XOPEN_SOURCE 600

#include "simula_rt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

/* Trampoline globals — set right before swapcontext, read immediately */
static void (*_pending_func)(void*);
static void* _pending_arg;
static SimulaCoro* _pending_coro;

static void coro_trampoline(void) {
    void (*func)(void*) = _pending_func;
    void* arg = _pending_arg;
    SimulaCoro* self = _pending_coro;

    func(arg);

    /* Body finished — mark terminated and return to caller */
    self->state = 3;
    setcontext(&self->caller_ctx);
    /* never reached */
}

void* simula_alloc(int64_t size) {
    void* p = calloc(1, (size_t)size);
    if (!p) {
        fprintf(stderr, "simula_alloc: out of memory\n");
        exit(1);
    }
    return p;
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
    /* Save where to return, then jump into the coroutine */
    swapcontext(&coro->caller_ctx, &coro->context);
    /* Execution resumes here when coroutine DETACHes or terminates */
}

void simula_coro_detach(SimulaCoro* coro) {
    coro->state = 2; /* detached */
    /* Save coroutine state, jump back to whoever started/resumed us */
    swapcontext(&coro->context, &coro->caller_ctx);
    /* Execution resumes here when someone RESUMEs us */
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
    /* Save current point into caller_ctx, jump to coroutine */
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
