/*
 * thread.h - perthread data and other utilities
 */

#pragma once

#include <sys/syscall.h>
#include <unistd.h>

#include <base/stddef.h>
#include <base/limits.h>
#include <base/cpu.h>

/* used to define perthread variables */
#define DEFINE_PERTHREAD(type, name) \
	typeof(type) __perthread_##name __perthread \
	__attribute__((section(".perthread,\"\",@nobits#")))

/* used to make perthread variables externally available */
#define DECLARE_PERTHREAD(type, name) \
	extern DEFINE_PERTHREAD(type, name)

extern void *perthread_offsets[NTHREAD];
extern __thread void *perthread_ptr;
extern unsigned int thread_count;
extern const char __perthread_start[];

/**
 * perthread_get_remote - get a perthread variable on a specific thread
 * @var: the perthread variable
 * @thread: the thread id
 *
 * Returns a perthread variable.
 */
#define perthread_get_remote(var, thread)			\
	(*((__force typeof(__perthread_##var) *)		\
	 ((uintptr_t)&__perthread_##var + (uintptr_t)perthread_offsets[thread] - (uintptr_t)__perthread_start)))

static inline void *__perthread_get(void __perthread *key)
{
	return (__force void *)((uintptr_t)key + (uintptr_t)perthread_ptr - (uintptr_t)__perthread_start);
}

/**
 * perthread_get - get the local perthread variable
 * @var: the perthread variable
 *
 * Returns a perthread variable.
 */
#define perthread_get(var)					\
	(*((typeof(__perthread_##var) *)(__perthread_get(&__perthread_##var))))

/**
 * thread_is_active - is the thread initialized?
 * @thread: the thread id
 *
 * Returns true if yes, false if no.
 */
#define thread_is_active(thread)					\
	(perthread_offsets[thread] != NULL)

static inline int __thread_next_active(int thread)
{
	while (thread < (int)thread_count) {
		if (thread_is_active(++thread))
			return thread;
	}

	return thread;
}

/**
 * for_each_thread - iterates over each thread
 * @thread: the thread id
 */
#define for_each_thread(thread)						\
	for ((thread) = -1; (thread) = __thread_next_active(thread),	\
			    (thread) < thread_count;)

extern __thread unsigned int thread_id;
extern __thread unsigned int thread_numa_node;

/**
 * returns the tid
 */
static inline pid_t gettid(void)
{
	pid_t tid;

	#ifdef SYS_gettid
	tid = syscall(SYS_gettid);
	#else
	#error "SYS_gettid unavailable on this system"
	#endif

	return tid;
}
