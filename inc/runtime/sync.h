/*
 * sync.h - support for synchronization
 */

#pragma once

#include <base/stddef.h>
#include <base/list.h>
#include <base/lock.h>
#include <runtime/thread.h>
#include <runtime/preempt.h>


/*
 * Mutex support
 */

struct mutex {
	atomic_t		held;
	spinlock_t		waiter_lock;
	struct list_head	waiters;
};

typedef struct mutex mutex_t;

extern void __mutex_lock(mutex_t *m);
extern void __mutex_unlock(mutex_t *m);
extern void mutex_init(mutex_t *m);

/**
 * mutex_try_lock - attempts to acquire a mutex
 * @m: the mutex to acquire
 *
 * Returns true if the acquire was successful.
 */
static inline bool mutex_try_lock(mutex_t *m)
{
	return atomic_cmpxchg(&m->held, 0, 1);
}

/**
 * mutex_lock - acquires a mutex
 * @m: the mutex to acquire
 */
static inline void mutex_lock(mutex_t *m)
{
	if (likely(atomic_cmpxchg(&m->held, 0, 1)))
		return;

	__mutex_lock(m);
}

/**
 * mutex_unlock - releases a mutex
 * @m: the mutex to release
 */
static inline void mutex_unlock(mutex_t *m)
{
	if (likely(atomic_cmpxchg(&m->held, 1, 0)))
		return;

	__mutex_unlock(m);
}
/**
 * mutex_held - is the mutex currently held?
 * @m: the mutex to check
 */
static inline bool mutex_held(const mutex_t *m)
{
	return atomic_read(&m->held);
}

/**
 * assert_mutex_held - asserts that a mutex is currently held
 * @m: the mutex that must be held
 */
static inline void assert_mutex_held(mutex_t *m)
{
	assert(mutex_held(m));
}


/*
 * Condition variable support
 */

struct condvar {
	spinlock_t		waiter_lock;
	struct list_head	waiters;
};

typedef struct condvar condvar_t;

extern bool condvar_wait_timed(condvar_t *cv, mutex_t *m, uint64_t micros);
extern void condvar_wait(condvar_t *cv, mutex_t *m);
extern void condvar_signal(condvar_t *cv);
extern void condvar_broadcast(condvar_t *cv);
extern void condvar_init(condvar_t *cv);


/*
 * Wait group support
 */

struct waitgroup {
	spinlock_t		lock;
	int			cnt;
	struct list_head	waiters;
};

typedef struct waitgroup waitgroup_t;

extern void waitgroup_add(waitgroup_t *wg, int cnt);
extern void waitgroup_wait(waitgroup_t *wg);
extern void waitgroup_init(waitgroup_t *wg);

/**
 * waitgroup_done - notifies the wait group that one waiting event completed
 * @wg: the wait group to complete
 */
static inline void waitgroup_done(waitgroup_t *wg)
{
	waitgroup_add(wg, -1);
}


/*
 * Spin lock support
 */

/**
 * spin_lock_np - takes a spin lock and disables preemption
 * @l: the spin lock
 */
static inline void spin_lock_np(spinlock_t *l)
{
	preempt_disable();
	spin_lock(l);
}

/**
 * spin_try_lock_np - takes a spin lock if its available and disables preemption
 * @l: the spin lock
 *
 * Returns true if successful, otherwise fail.
 */
static inline bool spin_try_lock_np(spinlock_t *l)
{
	preempt_disable();
	if (spin_try_lock(l))
		return true;

	preempt_enable();
	return false;
}

/**
 * spin_unlock_np - releases a spin lock and re-enables preemption
 * @l: the spin lock
 */
static inline void spin_unlock_np(spinlock_t *l)
{
	spin_unlock(l);
	preempt_enable();
}


/*
 * Barrier support
 */

struct barrier {
	spinlock_t		lock;
	int			waiting;
	int			count;
	struct list_head	waiters;
};

typedef struct barrier barrier_t;

extern void barrier_init(barrier_t *b, int count);
extern bool barrier_wait(barrier_t *b);


/*
 * Read-write mutex support
 */

struct rwmutex {
	spinlock_t		waiter_lock;
	int			count;
	struct list_head	read_waiters;
	struct list_head	write_waiters;
	int			read_waiter_count;
};

typedef struct rwmutex rwmutex_t;

static inline bool rwmutex_held(const rwmutex_t *m)
{
	return ACCESS_ONCE(m->count) != 0;
}

extern void rwmutex_init(rwmutex_t *m);
extern void rwmutex_rdlock(rwmutex_t *m);
extern void rwmutex_wrlock(rwmutex_t *m);
extern bool rwmutex_try_rdlock(rwmutex_t *m);
extern bool rwmutex_try_wrlock(rwmutex_t *m);
extern void rwmutex_unlock(rwmutex_t *m);
