/*
 * thread.h - support for user-level threads
 */

#pragma once

#include <base/types.h>
#include <base/compiler.h>
#include <runtime/preempt.h>

struct thread;
typedef void (*thread_fn_t)(void *arg);
typedef struct thread thread_t;


/*
 * Low-level routines, these are helpful for bindings and synchronization
 * primitives.
 */

extern void thread_park_and_unlock_np(spinlock_t *l);
extern void thread_ready(thread_t *thread);
extern thread_t *thread_create(thread_fn_t fn, void *arg);
extern thread_t *thread_create_with_buf(thread_fn_t fn, void **buf, size_t len);

extern __thread thread_t *__self;

/**
 * thread_self - gets the currently running thread
 */
inline thread_t *thread_self(void)
{
	return __self;
}


/*
 * High-level routines, use this API most of the time.
 */

extern void thread_yield(void);
extern int thread_spawn(thread_fn_t fn, void *arg);
extern void thread_exit(void) __noreturn;

/* main initialization */
typedef int (*initializer_fn_t)(void);

extern int runtime_set_initializers(initializer_fn_t global_fn,
				    initializer_fn_t perthread_fn,
				    initializer_fn_t late_fn);
extern int runtime_init(const char *cfgpath, thread_fn_t main_fn, void *arg);


extern int *congestion_signal;

/**
 * runtime_is_congested - returns true if experiencing compute congestion
 */
static inline bool runtime_is_congested(void)
{
	return ACCESS_ONCE(congestion_signal) > 0;
}
