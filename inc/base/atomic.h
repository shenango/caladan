/*
 * atomic.h - utilities for atomic memory ops
 *
 * With the exception of *_read and *_write, consider these operations full
 * barriers.
 */

#pragma once

#include <base/types.h>
#include <asm/atomic.h>

#define ATOMIC_INIT(val) {.cnt = (val)}

static inline int atomic_read(const atomic_t *a)
{
	return *((volatile int *)&a->cnt);
}

static inline void atomic_write(atomic_t *a, int val)
{
	a->cnt = val;
}

static inline int atomic_fetch_and_add(atomic_t *a, int val)
{
	return __sync_fetch_and_add(&a->cnt, val);
}

static inline int atomic_fetch_and_sub(atomic_t *a, int val)
{
	return __sync_fetch_and_sub(&a->cnt, val);
}

static inline long atomic_fetch_and_or(atomic_t *a, int val)
{
	return __sync_fetch_and_or(&a->cnt, val);
}

static inline long atomic_fetch_and_xor(atomic_t *a, int val)
{
	return __sync_fetch_and_xor(&a->cnt, val);
}

static inline long atomic_fetch_and_and(atomic_t *a, int val)
{
	return __sync_fetch_and_and(&a->cnt, val);
}

static inline int atomic_add_and_fetch(atomic_t *a, int val)
{
	return __sync_add_and_fetch(&a->cnt, val);
}

static inline int atomic_sub_and_fetch(atomic_t *a, int val)
{
	return __sync_sub_and_fetch(&a->cnt, val);
}

static inline void atomic_inc(atomic_t *a)
{
	atomic_fetch_and_add(a, 1);
}

static inline void atomic_dec(atomic_t *a)
{
	atomic_sub_and_fetch(a, 1);
}

static inline bool atomic_dec_and_test(atomic_t *a)
{
	return (atomic_sub_and_fetch(a, 1) == 0);
}

static inline bool atomic_cmpxchg(atomic_t *a, int oldv, int newv)
{
	return __sync_bool_compare_and_swap(&a->cnt, oldv, newv);
}

static inline int atomic_cmpxchg_val(atomic_t *a, int oldv, int newv)
{
	return __sync_val_compare_and_swap(&a->cnt, oldv, newv);
}

static inline long atomic64_read(const atomic64_t *a)
{
	return *((volatile long *)&a->cnt);
}

static inline void atomic64_write(atomic64_t *a, long val)
{
	a->cnt = val;
}

static inline long atomic64_fetch_and_add(atomic64_t *a, long val)
{
	return __sync_fetch_and_add(&a->cnt, val);
}

static inline long atomic64_fetch_and_sub(atomic64_t *a, long val)
{
	return __sync_fetch_and_sub(&a->cnt, val);
}

static inline long atomic64_fetch_and_or(atomic64_t *a, long val)
{
	return __sync_fetch_and_or(&a->cnt, val);
}

static inline long atomic64_fetch_and_xor(atomic64_t *a, long val)
{
	return __sync_fetch_and_xor(&a->cnt, val);
}


static inline long atomic64_fetch_and_and(atomic64_t *a, long val)
{
	return __sync_fetch_and_and(&a->cnt, val);
}

static inline long atomic64_add_and_fetch(atomic64_t *a, long val)
{
	return __sync_add_and_fetch(&a->cnt, val);
}

static inline long atomic64_sub_and_fetch(atomic64_t *a, long val)
{
	return __sync_sub_and_fetch(&a->cnt, val);
}

static inline void atomic64_inc(atomic64_t *a)
{
	atomic64_fetch_and_add(a, 1);
}

static inline void atomic64_dec(atomic64_t *a)
{
	atomic64_sub_and_fetch(a, 1);
}

static inline bool atomic64_dec_and_test(atomic64_t *a)
{
	return (atomic64_sub_and_fetch(a, 1) == 0);
}

static inline bool atomic64_cmpxchg(atomic64_t *a, long oldv, long newv)
{
	return __sync_bool_compare_and_swap(&a->cnt, oldv, newv);
}

static inline long atomic64_cmpxchg_val(atomic64_t *a, long oldv, long newv)
{
	return __sync_val_compare_and_swap(&a->cnt, oldv, newv);
}
