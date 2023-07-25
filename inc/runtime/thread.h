/*
 * thread.h - support for user-level threads
 */

#pragma once

#include <base/thread.h>
#include <base/types.h>
#include <base/compiler.h>
#include <runtime/preempt.h>
#include <iokernel/control.h>

struct thread;
typedef void (*thread_fn_t)(void *arg);
typedef struct thread thread_t;


/*
 * Low-level routines, these are helpful for bindings and synchronization
 * primitives.
 */

extern void thread_park_and_unlock_np(spinlock_t *l);
extern void thread_park_and_preempt_enable(void);
extern void thread_ready(thread_t *thread);
extern void thread_ready_head(thread_t *thread);
extern thread_t *thread_create(thread_fn_t fn, void *arg);
extern thread_t *thread_create_with_buf(thread_fn_t fn, void **buf, size_t len);

DECLARE_PERTHREAD(thread_t *, __self);
DECLARE_PERTHREAD(unsigned int, kthread_idx);

static inline unsigned int get_current_affinity(void)
{
	return perthread_read(kthread_idx);
}

/**
 * thread_self - gets the currently running thread
 */
inline thread_t *thread_self(void)
{
	return perthread_read_stable(__self);
}


extern uint64_t get_uthread_specific(void);
extern void set_uthread_specific(uint64_t val);


/*
 * High-level routines, use this API most of the time.
 */

extern void thread_yield(void);
extern int thread_spawn(thread_fn_t fn, void *arg);
extern void thread_exit(void) __noreturn;
