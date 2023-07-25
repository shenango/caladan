#include <base/log.h>
#include <runtime/smalloc.h>
#include <runtime/sync.h>

#include "../runtime/defs.h"
#include "common.h"
#include <semaphore.h>

BUILD_ASSERT(sizeof(sem_t) >= sizeof(uintptr_t));

struct rt_semaphore {
	spinlock_t lock;
	unsigned int value;
	struct list_head waiters;
};

/* Initialize semaphore object SEM to VALUE.  If PSHARED then share it
   with other processes.  */
int sem_init(sem_t *__sem, int __pshared, unsigned int __value)
{
	struct rt_semaphore *rt_sem;

	NOTSELF(sem_init, __sem, __pshared, __value);

	if (__pshared) {
		log_err("No shim support for shared semaphores");
		errno = EINVAL;
		return -1;
	}

	rt_sem = smalloc(sizeof(*rt_sem));
	if (!rt_sem) {
		errno = ENOMEM;
		return -1;
	}

	spin_lock_init(&rt_sem->lock);
	list_head_init(&rt_sem->waiters);
	rt_sem->value = __value;
	*(struct rt_semaphore **)__sem = rt_sem;
	return 0;
}

/* Free resources associated with semaphore object SEM.  */
int sem_destroy(sem_t *__sem)
{
	struct rt_semaphore *rt_sem = *(struct rt_semaphore **)__sem;
	NOTSELF(sem_destroy, __sem);
	BUG_ON(!list_empty(&rt_sem->waiters));
	sfree(rt_sem);
	return 0;
}


/* Open a named semaphore NAME with open flags OFLAG.  */
sem_t *sem_open(const char *__name, int __oflag, ...)
{
	log_err("sem_open not supported");
	errno = EINVAL;
	return NULL;
}

/* Close descriptor for named semaphore SEM.  */
int sem_close(sem_t *__sem)
{
	log_err("sem_close not supported");
	errno = EINVAL;
	return -1;
}

/* Remove named semaphore NAME.  */
int sem_unlink(const char *__name)
{
	log_err("sem_unlink not supported");
	errno = EINVAL;
	return -1;
}

/* Wait for SEM being posted.
    This function is a cancellation point and therefore not marked with
    __THROW.  */
int sem_wait(sem_t *__sem)
{
	thread_t *myth;
	struct rt_semaphore *rt_sem = *(struct rt_semaphore **)__sem;

	NOTSELF(sem_wait, __sem);

	spin_lock_np(&rt_sem->lock);
	if (rt_sem->value > 0) {
		rt_sem->value--;
		spin_unlock_np(&rt_sem->lock);
		return 0;
	}

    myth = thread_self();
    list_add_tail(&rt_sem->waiters, &myth->link);
    thread_park_and_unlock_np(&rt_sem->lock);
    return 0;
}


int sem_clockwait(sem_t *__restrict __sem,
                          clockid_t clock,
                          const struct timespec *__restrict __abstime)
{
	NOTSELF(sem_clockwait, __sem, clock, __abstime);
	log_err("sem_clockwait not supported");
	errno = EINVAL;
	return -1;
}

/* Test whether SEM is posted.  */
int sem_trywait(sem_t *__sem)
{
	struct rt_semaphore *rt_sem = *(struct rt_semaphore **)__sem;

	NOTSELF(sem_trywait, __sem);

	bool success = false;
	spin_lock_np(&rt_sem->lock);
	if (rt_sem->value > 0) {
		rt_sem->value--;
		success = true;
	}
	spin_unlock_np(&rt_sem->lock);
	if (success)
		return 0;
	errno = EAGAIN;
	return -1;
}


/* Post SEM.  */
int sem_post(sem_t *__sem)
{
	struct rt_semaphore *rt_sem = *(struct rt_semaphore **)__sem;
	thread_t *waketh;

	NOTSELF(sem_post, __sem);

	spin_lock_np(&rt_sem->lock);
	waketh = list_pop(&rt_sem->waiters, thread_t, link);
	if (waketh)
		assert(rt_sem->value == 0);
	else
		rt_sem->value++;
	spin_unlock_np(&rt_sem->lock);

	if (waketh)
		thread_ready(waketh);

	return 0;
}

/* Get current value of SEM and store it in *SVAL.  */
int sem_getvalue(sem_t *__restrict __sem, int *__restrict __sval)
{
	struct rt_semaphore *rt_sem = *(struct rt_semaphore **)__sem;

	NOTSELF(sem_getvalue, __sem, __sval);
	spin_lock_np(&rt_sem->lock);
	*__sval = rt_sem->value;
	spin_unlock_np(&rt_sem->lock);
	return 0;
}
