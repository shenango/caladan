/*
 * thread.h - support for user-level threads
 */

#pragma once

#include <base/compiler.h>
#include <base/list.h>
#include <base/thread.h>
#include <base/trapframe.h>
#include <base/types.h>
#include <runtime/preempt.h>
#include <iokernel/control.h>

struct thread;
typedef void (*thread_fn_t)(void *arg);
typedef struct thread thread_t;

/*
 * Internal thread structure, only intended for building low level primitives.
 */
struct thread {
	struct thread_tf	tf;
	struct list_node	link;
	struct stack		*stack;
	unsigned int		main_thread:1;
	unsigned int            has_fsbase:1;
	unsigned int		thread_ready;
	unsigned int		thread_running;
	unsigned int		last_cpu;
	uint64_t		run_start_tsc;
	uint64_t		ready_tsc;
	uint64_t		fsbase;
	uint64_t		tlsvar;
};

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
extern void thread_set_fsbase(thread_t *th, uint64_t fsbase);

DECLARE_PERTHREAD(thread_t *, __self);
DECLARE_PERTHREAD_ALIAS(thread_t * const, __self, __const_self);
DECLARE_PERTHREAD(uint64_t, runtime_fsbase);

static inline unsigned int get_current_affinity(void)
{
	return this_thread_id();
}

/**
 * thread_self - gets the currently running thread
 */
static inline thread_t *thread_self(void)
{
	return perthread_read_const_p(__const_self);
}

static inline uint64_t get_uthread_specific(void)
{
    return thread_self()->tlsvar;
}

static inline void set_uthread_specific(uint64_t val)
{
    thread_self()->tlsvar = val;
}


/*
 * High-level routines, use this API most of the time.
 */

extern void thread_yield(void);
extern int thread_spawn(thread_fn_t fn, void *arg);
extern void thread_exit(void) __noreturn;
