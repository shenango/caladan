#include <pthread.h>

#include <base/lock.h>
#include <runtime/sync.h>
#include <runtime/thread.h>

#include "common.h"

BUILD_ASSERT(sizeof(pthread_t) >= sizeof(uintptr_t));

struct join_handle {
	void *(*fn)(void *);
	void *args;
	void *retval;
	spinlock_t lock;
	thread_t *waiter;
	bool detached;
};

static void thread_trampoline(void *arg)
{
	struct join_handle *j = arg;

	j->retval = j->fn(j->args);
	spin_lock_np(&j->lock);
	if (j->detached) {
		spin_unlock_np(&j->lock);
		return;
	}
	if (j->waiter != NULL) {
		thread_ready(j->waiter);
	}
	j->waiter = thread_self();
	thread_park_and_unlock_np(&j->lock);
}

static int thread_spawn_joinable(struct join_handle **handle,
				 void *(*fn)(void *), void *arg)
{
	struct join_handle *j;
	thread_t *t = thread_create_with_buf(thread_trampoline, (void **)&j,
					     sizeof(struct join_handle));
	if (t == NULL)
		return -ENOMEM;

	j->fn = fn;
	j->args = arg;
	spin_lock_init(&j->lock);
	j->waiter = NULL;
	j->detached = false;

	if (handle)
		*handle = j;

	thread_ready(t);
	return 0;
}

static int thread_detach(struct join_handle *j)
{
	spin_lock_np(&j->lock);
	if (j->detached) {
		spin_unlock_np(&j->lock);
		return -EINVAL;
	}
	j->detached = true;
	if (j->waiter != NULL) {
		thread_ready(j->waiter);
	}
	spin_unlock_np(&j->lock);
	return 0;
}

static int thread_join(struct join_handle *j, void **retval)
{
	spin_lock_np(&j->lock);
	if (j->detached) {
		spin_unlock_np(&j->lock);
		return -EINVAL;
	}
	if (j->waiter == NULL) {
		j->waiter = thread_self();
		thread_park_and_unlock_np(&j->lock);
		spin_lock_np(&j->lock);
	}
	if (retval)
		*retval = j->retval;
	spin_unlock_np(&j->lock);
	thread_ready(j->waiter);
	return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
		   void *(*start_routine)(void *), void *arg)
{
	NOTSELF_4ARG(int, __func__, thread, attr, start_routine, arg);
	return thread_spawn_joinable((struct join_handle **)thread,
				     start_routine, arg);
}

int pthread_detach(pthread_t thread)
{
	NOTSELF_1ARG(int, __func__, thread);
	return thread_detach((struct join_handle *)thread);
}

int pthread_join(pthread_t thread, void **retval)
{
	NOTSELF_2ARG(int, __func__, thread, retval);
	return thread_join((struct join_handle *)thread, retval);
}

int pthread_yield(void)
{
	NOTSELF_NOARG(int, __func__);
	thread_yield();
	return 0;
}
