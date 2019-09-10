
#include <dlfcn.h>
#include <pthread.h>

#include <runtime/sync.h>

BUILD_ASSERT(sizeof(pthread_barrier_t) >= sizeof(barrier_t));
BUILD_ASSERT(sizeof(pthread_mutex_t) >= sizeof(mutex_t));
BUILD_ASSERT(sizeof(pthread_spinlock_t) >= sizeof(spinlock_t));
BUILD_ASSERT(sizeof(pthread_cond_t) >= sizeof(condvar_t));
BUILD_ASSERT(sizeof(pthread_rwlock_t) >= sizeof(rwmutex_t));

#define NOTSELF_1ARG(retType, name, arg)                                       \
	if (unlikely(!__self)) {                                               \
		static retType (*fn)(typeof(arg));                             \
		if (!fn) {                                                     \
			fn = dlsym(RTLD_NEXT, name);                           \
			BUG_ON(!fn);                                           \
		}                                                              \
		return fn(arg);                                                \
	}

#define NOTSELF_2ARG(retType, name, arg1, arg2)                                \
	if (unlikely(!__self)) {                                               \
		static retType (*fn)(typeof(arg1), typeof(arg2));              \
		if (!fn) {                                                     \
			fn = dlsym(RTLD_NEXT, name);                           \
			BUG_ON(!fn);                                           \
		}                                                              \
		return fn(arg1, arg2);                                         \
	}

#define NOTSELF_3ARG(retType, name, arg1, arg2, arg3)                          \
	if (unlikely(!__self)) {                                               \
		static retType (*fn)(typeof(arg1), typeof(arg2),               \
				     typeof(arg3));                            \
		if (!fn) {                                                     \
			fn = dlsym(RTLD_NEXT, name);                           \
			BUG_ON(!fn);                                           \
		}                                                              \
		return fn(arg1, arg2, arg3);                                   \
	}

int pthread_mutex_init(pthread_mutex_t *mutex,
		       const pthread_mutexattr_t *mutexattr)
{
	NOTSELF_2ARG(int, __func__, mutex, mutexattr);
	mutex_init((mutex_t *)mutex);
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	NOTSELF_1ARG(int, __func__, mutex);
	mutex_lock((mutex_t *)mutex);
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	NOTSELF_1ARG(int, __func__, mutex);
	return mutex_try_lock((mutex_t *)mutex) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	NOTSELF_1ARG(int, __func__, mutex);
	mutex_unlock((mutex_t *)mutex);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	NOTSELF_1ARG(int, __func__, mutex);
	return 0;
}

int pthread_barrier_init(pthread_barrier_t *restrict barrier,
			 const pthread_barrierattr_t *restrict attr,
			 unsigned count)
{
	NOTSELF_3ARG(int, __func__, barrier, attr, count);

	barrier_init((barrier_t *)barrier, count);

	return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier)
{
	NOTSELF_1ARG(int, __func__, barrier);

	if (barrier_wait((barrier_t *)barrier))
		return PTHREAD_BARRIER_SERIAL_THREAD;

	return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
	NOTSELF_1ARG(int, __func__, barrier);
	return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock)
{
	NOTSELF_1ARG(int, __func__, lock);
	return 0;
}

int pthread_spin_init(pthread_spinlock_t *lock, int pshared)
{
	NOTSELF_2ARG(int, __func__, lock, pshared);
	spin_lock_init((spinlock_t *)lock);
	return 0;
}

int pthread_spin_lock(pthread_spinlock_t *lock)
{
	NOTSELF_1ARG(int, __func__, lock);
	spin_lock_np((spinlock_t *)lock);
	return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock)
{
	NOTSELF_1ARG(int, __func__, lock);
	return spin_try_lock_np((spinlock_t *)lock) ? 0 : EBUSY;
}

int pthread_spin_unlock(pthread_spinlock_t *lock)
{
	NOTSELF_1ARG(int, __func__, lock);
	spin_unlock_np((spinlock_t *)lock);
	return 0;
}

int pthread_cond_init(pthread_cond_t *__restrict cond,
		      const pthread_condattr_t *__restrict cond_attr)
{
	NOTSELF_2ARG(int, __func__, cond, cond_attr);
	condvar_init((condvar_t *)cond);
	return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
	NOTSELF_1ARG(int, __func__, cond);
	condvar_signal((condvar_t *)cond);
	return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
	NOTSELF_1ARG(int, __func__, cond);
	condvar_broadcast((condvar_t *)cond);
	return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	NOTSELF_2ARG(int, __func__, cond, mutex);
	condvar_wait((condvar_t *)cond, (mutex_t *)mutex);
	return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
			   const struct timespec *abstime)
{
	BUG();
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
	NOTSELF_1ARG(int, __func__, cond);
	return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *r)
{
	NOTSELF_1ARG(int, __func__, r);
	return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *r, const pthread_rwlockattr_t *attr)
{
	NOTSELF_2ARG(int, __func__, r, attr);
	rwmutex_init((rwmutex_t *)r);
	return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *r)
{
	NOTSELF_1ARG(int, __func__, r);
	rwmutex_rdlock((rwmutex_t *)r);
	return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *r)
{
	NOTSELF_1ARG(int, __func__, r);
	return rwmutex_try_rdlock((rwmutex_t *)r) ? 0 : EBUSY;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *r)
{
	NOTSELF_1ARG(int, __func__, r);
	return rwmutex_try_wrlock((rwmutex_t *)r) ? 0 : EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *r)
{
	NOTSELF_1ARG(int, __func__, r);
	rwmutex_wrlock((rwmutex_t *)r);
	return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *r)
{
	NOTSELF_1ARG(int, __func__, r);
	rwmutex_unlock((rwmutex_t *)r);
	return 0;
}
