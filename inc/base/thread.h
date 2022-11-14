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
	__attribute__((section(".perthread")))

/* used to make perthread variables externally available */
#define DECLARE_PERTHREAD(type, name) \
	extern DEFINE_PERTHREAD(type, name)

extern void *perthread_offsets[NTHREAD];
DECLARE_PERTHREAD(void *, perthread_ptr);
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


#define __perthread_addr_qual(key, qualifier)                     \
({                                                                \
	void *__out;                                                  \
	asm qualifier("add %%gs:__perthread_perthread_ptr(%%rip), %0" \
	              : "=r"(__out) : "0"(key) : "cc");               \
	__out;                                                        \
})

#define __perthread_read_qual(key, qualifier)        \
({                                                   \
	typeof(__perthread_##key) __out;                 \
	BUILD_ASSERT(type_is_native(__perthread_##key)); \
	asm qualifier(                                   \
	    "mov %%gs:"                                  \
	     "__perthread_" #key "(%%rip), %0"           \
	     : "=r"(__out));                             \
	__out;                                           \
})

/**
 * perthread_store - stores a new value into a
 * native-type perthread variable.
 * @var: the perthread variable
 * @val: the value to store
 *
 */
#define perthread_store(key, val)                    \
({                                                   \
	BUILD_ASSERT(type_is_native(__perthread_##key)); \
	typeof(__perthread_##key) __in = (val);          \
	asm volatile(                                    \
	    "mov %0, %%gs:"                              \
	     "__perthread_" #key "(%%rip)"               \
	     : : "r"(__in) : "memory");                  \
})


/**
 * perthread_incr - increments a native-type perthread
 * variable.
 * @var: the perthread variable
 *
 */
#define perthread_incr(key)        \
({                                                   \
	BUILD_ASSERT(type_is_native(__perthread_##key)); \
	asm volatile(                                    \
	    "add $1, %%gs:"                              \
	     "__perthread_" #key "(%%rip)"               \
	     : : : "memory", "cc");                      \
})

/**
 * perthread_read - read the value stored at
 * a native-type perthread variable.
 * @var: the perthread variable
 *
 * Returns the value stored at @var
 *
 */
#define perthread_read(key) __perthread_read_qual(key, volatile)

/**
 * perthread_read_stable - read the value stored at
 * a native-type perthread variable. The result
 * may be cached by the compiler.
 * @var: the perthread variable
 *
 * Returns the value stored at @var
 *
 */
#define perthread_read_stable(key) __perthread_read_qual(key, /* */)


/**
 * perthread_get_stable - get the local perthread variable
 * result may be cached by compiler.
 * @var: the perthread variable
 *
 * Returns a perthread variable.
 */
#define perthread_get_stable(var)    \
	(*((typeof(__perthread_##var) *) \
	  (__perthread_addr_qual(&__perthread_##var, /* */))))


/**
 * perthread_get - get the local perthread variable
 * @var: the perthread variable
 *
 * Returns a perthread variable.
 */
#define perthread_get(var)           \
	(*((typeof(__perthread_##var) *) \
	  (__perthread_addr_qual(&__perthread_##var, volatile))))

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

DECLARE_PERTHREAD(unsigned int, thread_id);
DECLARE_PERTHREAD(unsigned int, thread_numa_node);

static inline unsigned int this_thread_id(void)
{
	return perthread_read(thread_id);
}

static inline unsigned int this_numa_node(void)
{
	return perthread_read(thread_numa_node);
}

extern pid_t thread_gettid(void);
