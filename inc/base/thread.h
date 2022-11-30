/*
 * thread.h - perthread data and other utilities
 */

#pragma once

#include <sys/syscall.h>
#include <unistd.h>

#include <base/cpu.h>
#include <base/limits.h>
#include <base/stddef.h>

/* used to define perthread variables */
#define DEFINE_PERTHREAD(type, name)                                           \
  typeof(type) __perthread_##name __perthread                                  \
      __attribute__((section(".perthread,\"\",@nobits#")))

/* used to make perthread variables externally available */
#define DECLARE_PERTHREAD(type, name) extern DEFINE_PERTHREAD(type, name)

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
#define perthread_get_remote(var, thread)                                      \
  (*((__force typeof(__perthread_##var) *)((uintptr_t)&__perthread_##var +     \
                                           (uintptr_t)                         \
                                               perthread_offsets[thread])))

#define __perthread_addr_qual(key, qualifier)                                  \
  ({                                                                           \
    void *__out;                                                               \
    asm qualifier("add %%gs:%1, %0"                                            \
                  : "=r"(__out)                                                \
                  : "m"(__perthread_perthread_ptr), "0"(key)                   \
                  : "cc");                                                     \
    __out;                                                                     \
  })

#define __perthread_read_qual(key, qualifier)                                  \
  ({                                                                           \
    BUILD_ASSERT(type_is_native(key));                                         \
    typeof(key) __out;                                                         \
    asm qualifier("mov %%gs:%1, %0" : "=r"(__out) : "m"(key));                 \
    __out;                                                                     \
  })

#define _perthread_store(var, val)                                             \
  ({                                                                           \
    BUILD_ASSERT(type_is_native(var));                                         \
    typeof(var) __in = (val);                                                  \
    asm volatile("mov %0, %%gs:%1" : : "r"(__in), "m"(var) : "memory");        \
  })

/**
 * perthread_store - stores a new value into a
 * native-type perthread variable.
 * @var: the perthread variable
 * @val: the value to store
 *
 */
#define perthread_store(var, val) _perthread_store(__perthread_##var, val)

#define __perthread_imm_op_sz(opn, sz, var, imm)                               \
  ({                                                                           \
    BUILD_ASSERT(type_is_native(var));                                         \
    asm volatile(opn sz " $" #imm ", %%gs:%0" : : "m"(var) : "memory", "cc");  \
  })

#define __perthread_imm_op(opn, var, imm)                                      \
  do {                                                                         \
    BUILD_ASSERT(type_is_native(var));                                         \
    switch (sizeof(var)) {                                                     \
    case 1:                                                                    \
      __perthread_imm_op_sz(opn, "b", var, imm);                               \
      break;                                                                   \
    case 2:                                                                    \
      __perthread_imm_op_sz(opn, "w", var, imm);                               \
      break;                                                                   \
    case 4:                                                                    \
      __perthread_imm_op_sz(opn, "l", var, imm);                               \
      break;                                                                   \
    case 8:                                                                    \
      __perthread_imm_op_sz(opn, "q", var, imm);                               \
      break;                                                                   \
    }                                                                          \
  } while (0);

/**
 * perthread_incr - increments a native-type perthread
 * variable using a single instruction.
 * @var: the perthread variable
 *
 */
#define perthread_incr(var) __perthread_imm_op("add", __perthread_##var, 1)

/**
 * perthread_decr - decrements a native-type perthread
 * variable using a single instruction.
 * @var: the perthread variable
 *
 */
#define perthread_decr(var) __perthread_imm_op("sub", __perthread_##var, 1)

/**
 * perthread_andi - bitwise-and an immediate into a
 * native-type perthread variable.
 * @var: the perthread variable
 * @imm: the immediate
 *
 */
#define perthread_andi(var, imm)                                               \
  __perthread_imm_op("and", __perthread_##var, imm)

/**
 * perthread_ori - bitwise-or an immediate into a
 * native-type perthread variable.
 * @var: the perthread variable
 * @imm: the immediate
 *
 */
#define perthread_ori(var, imm) __perthread_imm_op("or", __perthread_##var, imm)

/**
 * perthread_read - read the value stored at
 * a native-type perthread variable.
 * @var: the perthread variable
 *
 * Returns the value stored at @var
 *
 */
#define perthread_read(var) __perthread_read_qual(__perthread_##var, volatile)

/**
 * perthread_read_stable - read the value stored at
 * a native-type perthread variable. The result
 * may be cached by the compiler.
 * @var: the perthread variable
 *
 * Returns the value stored at @var
 *
 */
#define perthread_read_stable(var)                                             \
  __perthread_read_qual(__perthread_##var, /* */)

/**
 * perthread_ptr_stable - get a pointer to a local perthread variable
 * result may be cached by compiler.
 * @var: the perthread variable
 *
 * Returns a pointer to a perthread variable.
 */
#define perthread_ptr_stable(var)                                              \
  (((typeof(__perthread_##var) *)(__perthread_addr_qual(&__perthread_##var,    \
                                                        /* */))))

/**
 * perthread_get_stable - get the local perthread variable
 * result may be cached by compiler.
 * @var: the perthread variable
 *
 * Returns a perthread variable.
 */
#define perthread_get_stable(var) (*perthread_ptr_stable(var))

/**
 * perthread_ptr - get a pointer to a local perthread variable
 * @var: the perthread variable
 *
 * Returns a pointer to a perthread variable.
 */
#define perthread_ptr(var)                                                     \
  (((typeof(__perthread_##var) *)(__perthread_addr_qual(&__perthread_##var,    \
                                                        volatile))))

/**
 * perthread_get - get the local perthread variable
 * @var: the perthread variable
 *
 * Returns a perthread variable.
 */
#define perthread_get(var) (*perthread_ptr(var))

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
