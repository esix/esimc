#ifndef SIMULA_RT_H
#define SIMULA_RT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SIMULA_CORO_STACK_SIZE (256 * 1024)

typedef struct SimulaCoro SimulaCoro;

/* Memory allocation */
void* simula_alloc(int64_t size);

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
