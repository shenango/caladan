/*
 * preempt.h - support for kthread preemption
 */

#pragma once

#include <base/stddef.h>
#include <base/thread.h>

DECLARE_PERTHREAD(unsigned int, preempt_cnt);
DECLARE_PERTHREAD(void *, uintr_stack);

extern void preempt(void);
extern void uintr_asm_return(void);

extern size_t xsave_max_size;
extern size_t xsave_features;

/* this flag is set whenever there is _not_ a pending preemption */
#define PREEMPT_NOT_PENDING	(1 << 31)

/**
 * preempt_disable - disables preemption
 *
 * Can be nested.
 */
static __always_inline __nofp void preempt_disable(void)
{
	asm volatile("addl $1, %%gs:__perthread_preempt_cnt(%%rip)" ::: "memory", "cc");
	barrier();
}

/**
 * preempt_enable_nocheck - reenables preemption without checking for conditions
 *
 * Can be nested.
 */
static inline void preempt_enable_nocheck(void)
{
	barrier();
	perthread_decr(preempt_cnt);
}

/**
 * preempt_enable - reenables preemption
 *
 * Can be nested.
 */
static __always_inline __nofp void preempt_enable(void)
{
#ifndef __GCC_ASM_FLAG_OUTPUTS__
	preempt_enable_nocheck();
	if (unlikely(perthread_read(preempt_cnt) == 0))
		preempt();
#else
	int zero;
	barrier();
	asm volatile("subl $1, %%gs:__perthread_preempt_cnt(%%rip)"
		     : "=@ccz" (zero) :: "memory", "cc");
	if (unlikely(zero))
		preempt();
#endif
}

/**
 * preempt_needed - returns true if a preemption event is stuck waiting
 */
static inline bool preempt_needed(void)
{
	return (perthread_read(preempt_cnt) & PREEMPT_NOT_PENDING) == 0;
}

/**
 * preempt_enabled - returns true if preemption is enabled
 */
static __always_inline __nofp bool preempt_enabled(void)
{
	return (perthread_read(preempt_cnt) & ~PREEMPT_NOT_PENDING) == 0;
}

/**
 * assert_preempt_disabled - asserts that preemption is disabled
 */
static inline void assert_preempt_disabled(void)
{
	assert(!preempt_enabled());
}

/**
 * clear_preempt_needed - clear the flag that indicates a preemption request is
 * pending
 */
static inline void clear_preempt_needed(void)
{
	BUILD_ASSERT(PREEMPT_NOT_PENDING == 0x80000000);
	perthread_ori(preempt_cnt, 0x80000000);
}
