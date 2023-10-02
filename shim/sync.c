#include <base/time.h>
#include <pthread.h>
#include <runtime/sync.h>
#include <sys/time.h>

#include "common.h"

#define INIT_MAGIC 0xDEADBEEF

struct condvar_with_attr {
	condvar_t cv; // condvar must be first
	clockid_t clockid;
	uint32_t magic;
};

struct mutex_with_attr {
	mutex_t mutex; // must be first
	uint32_t magic;
};

struct rwmutex_with_attr {
	rwmutex_t rwmutex; // must be first
	uint32_t magic;
};

BUILD_ASSERT(sizeof(pthread_barrier_t) >= sizeof(barrier_t));
BUILD_ASSERT(sizeof(pthread_mutex_t) >= sizeof(struct mutex_with_attr));
BUILD_ASSERT(sizeof(pthread_cond_t) >= sizeof(struct condvar_with_attr));
BUILD_ASSERT(sizeof(pthread_rwlock_t) >= sizeof(struct rwmutex_with_attr));

static inline void mutex_intialized_check(pthread_mutex_t *mutex) {
	struct mutex_with_attr *m = (struct mutex_with_attr *)mutex;
	if (unlikely(m->magic != INIT_MAGIC))
		pthread_mutex_init(mutex, NULL);
}

static inline void cond_intialized_check(pthread_cond_t *cond) {
	struct condvar_with_attr *cvattr = (struct condvar_with_attr *)cond;
	if (unlikely(cvattr->magic != INIT_MAGIC))
		pthread_cond_init(cond, NULL);
}

static inline void rwmutex_intialized_check(pthread_rwlock_t *r) {
	struct rwmutex_with_attr *rwattr = (struct rwmutex_with_attr *)r;
	if (unlikely(rwattr->magic != INIT_MAGIC))
		pthread_rwlock_init(r, NULL);
}


int pthread_mutex_init(pthread_mutex_t *mutex,
		       const pthread_mutexattr_t *mutexattr)
{
	NOTSELF(pthread_mutex_init, mutex, mutexattr);
	struct mutex_with_attr *m = (struct mutex_with_attr *)mutex;
	mutex_init(&m->mutex);
	m->magic = INIT_MAGIC;
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	NOTSELF(pthread_mutex_lock, mutex);
	mutex_intialized_check(mutex);
	mutex_lock((mutex_t *)mutex);
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	NOTSELF(pthread_mutex_trylock, mutex);
	mutex_intialized_check(mutex);
	return mutex_try_lock((mutex_t *)mutex) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	NOTSELF(pthread_mutex_unlock, mutex);
	mutex_unlock((mutex_t *)mutex);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	NOTSELF(pthread_mutex_destroy, mutex);
	return 0;
}

int pthread_barrier_init(pthread_barrier_t *restrict barrier,
			 const pthread_barrierattr_t *restrict attr,
			 unsigned count)
{
	NOTSELF(pthread_barrier_init, barrier, attr, count);

	barrier_init((barrier_t *)barrier, count);

	return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier)
{
	NOTSELF(pthread_barrier_wait, barrier);

	if (barrier_wait((barrier_t *)barrier))
		return PTHREAD_BARRIER_SERIAL_THREAD;

	return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
	NOTSELF(pthread_barrier_destroy, barrier);
	return 0;
}

int pthread_spin_lock(pthread_spinlock_t *lock)
{
	int ret;
	static typeof(pthread_spin_lock) *fn;
	if (unlikely(!fn)) {
		fn = dlsym(RTLD_NEXT, "pthread_spin_lock");
		BUG_ON(!fn);
	}

	if (unlikely(!shim_active()))
		return fn(lock);

	preempt_disable();
	ret = fn(lock);
	if (unlikely(ret != 0))
		preempt_enable();
	return ret;
}

int pthread_spin_trylock(pthread_spinlock_t *lock)
{
	int ret;
	static typeof(pthread_spin_trylock) *fn;

	if (unlikely(!fn)) {
		fn = dlsym(RTLD_NEXT, "pthread_spin_trylock");
		BUG_ON(!fn);
	}

	if (unlikely(!shim_active()))
		return fn(lock);

	preempt_disable();

	ret = fn(lock);

	if (ret != 0)
		preempt_enable();

	return ret;
}

int pthread_spin_unlock(pthread_spinlock_t *lock)
{
	int ret;
	static typeof(pthread_spin_unlock) *fn;

	if (unlikely(!fn)) {
		fn = dlsym(RTLD_NEXT, "pthread_spin_unlock");
		BUG_ON(!fn);
	}

	ret = fn(lock);

	if (likely(shim_active()))
		preempt_enable();

	return ret;
}

int pthread_cond_init(pthread_cond_t *__restrict cond,
		      const pthread_condattr_t *__restrict cond_attr)
{
	NOTSELF(pthread_cond_init, cond, cond_attr);
	struct condvar_with_attr *cvattr = (struct condvar_with_attr *)cond;
	condvar_init(&cvattr->cv);
	cvattr->magic = INIT_MAGIC;

	if (!cond_attr ||
	    pthread_condattr_getclock(cond_attr, &cvattr->clockid)) {
		cvattr->clockid = CLOCK_REALTIME;
	}

	return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
	NOTSELF(pthread_cond_signal, cond);
	cond_intialized_check(cond);
	condvar_signal((condvar_t *)cond);
	return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
	NOTSELF(pthread_cond_broadcast, cond);
	cond_intialized_check(cond);
	condvar_broadcast((condvar_t *)cond);
	return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	NOTSELF(pthread_cond_wait, cond, mutex);
	cond_intialized_check(cond);
	condvar_wait((condvar_t *)cond, (mutex_t *)mutex);
	return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
			   const struct timespec *abstime)
{
	bool done;
	uint64_t wait_us, now_us;
	struct condvar_with_attr *cvattr = (struct condvar_with_attr *)cond;
	struct timespec now_ts;

	NOTSELF(pthread_cond_timedwait, cond, mutex, abstime);

	cond_intialized_check(cond);

	BUG_ON(clock_gettime(cvattr->clockid, &now_ts));
	wait_us = abstime->tv_sec * ONE_SECOND + abstime->tv_nsec / 1000;
	now_us = now_ts.tv_sec * ONE_SECOND + now_ts.tv_nsec / 1000;

	if (wait_us <= now_us)
		return ETIMEDOUT;

	done = condvar_wait_timed((condvar_t *)cond, (mutex_t *)mutex, wait_us - now_us);
	return done ? 0 : ETIMEDOUT;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
	NOTSELF(pthread_cond_destroy, cond);
	return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *r)
{
	NOTSELF(pthread_rwlock_destroy, r);
	return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *r, const pthread_rwlockattr_t *attr)
{
	NOTSELF(pthread_rwlock_init, r, attr);
	struct rwmutex_with_attr *rwattr = (struct rwmutex_with_attr *)r;
	rwattr->magic = INIT_MAGIC;
	rwmutex_init(&rwattr->rwmutex);
	return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *r)
{
	NOTSELF(pthread_rwlock_rdlock, r);
	rwmutex_intialized_check(r);
	rwmutex_rdlock((rwmutex_t *)r);
	return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *r)
{
	NOTSELF(pthread_rwlock_tryrdlock, r);
	rwmutex_intialized_check(r);
	return rwmutex_try_rdlock((rwmutex_t *)r) ? 0 : EBUSY;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *r)
{
	NOTSELF(pthread_rwlock_trywrlock, r);
	rwmutex_intialized_check(r);
	return rwmutex_try_wrlock((rwmutex_t *)r) ? 0 : EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *r)
{
	NOTSELF(pthread_rwlock_wrlock, r);
	rwmutex_intialized_check(r);
	rwmutex_wrlock((rwmutex_t *)r);
	return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *r)
{
	NOTSELF(pthread_rwlock_unlock, r);
	rwmutex_unlock((rwmutex_t *)r);
	return 0;
}
