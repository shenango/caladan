/*
 * poll.h - support for event polling (similar to select/epoll/poll, etc.)
 */

#include <runtime/poll.h>

/**
 * poll_init - initializes a polling waiter object
 * @w: the waiter object to initialize
 */
void poll_init(poll_waiter_t *w)
{
	spin_lock_init(&w->lock);
	list_head_init(&w->triggered);
	w->waiting_th = NULL;
}

/**
 * poll_arm - registers a trigger with a waiter
 * @w: the waiter to register with
 * @t: the trigger to register
 * @data: data to provide when the trigger fires
 */
void poll_arm(poll_waiter_t *w, poll_trigger_t *t, unsigned long data)
{
	if (WARN_ON(t->waiter != NULL))
		return;

	t->waiter = w;
	t->triggered = false;
	t->data = data;
}

/**
 * poll_disarm - unregisters a trigger with a waiter
 * @t: the trigger to unregister
 */
void poll_disarm(poll_trigger_t *t)
{
	poll_waiter_t *w;
	if (WARN_ON(t->waiter == NULL))
		return;

	w = t->waiter;
	spin_lock_np(&w->lock);
	if (t->triggered) {
		list_del(&t->link);
		t->triggered = false;
	}
	spin_unlock_np(&w->lock);

	t->waiter = NULL;
}

/**
 * poll_wait - waits for the next event to trigger
 * @w: the waiter to wait on
 *
 * Returns the data provided to the trigger that fired
 */
unsigned long poll_wait(poll_waiter_t *w)
{
	thread_t *th = thread_self();
	poll_trigger_t *t;

	while (true) {
		spin_lock_np(&w->lock);
		t = list_pop(&w->triggered, poll_trigger_t, link);
		if (t) {
			spin_unlock_np(&w->lock);
			return t->data;
		}
		w->waiting_th = th;
		thread_park_and_unlock_np(&w->lock);
	}
}

/**
 * poll_trigger - fires a trigger
 * @w: the waiter to wake up (if it is waiting)
 * @t: the trigger that fired
 */
void poll_trigger(poll_waiter_t *w, poll_trigger_t *t)
{
	thread_t *wth = NULL;

	spin_lock_np(&w->lock);
	if (t->triggered) {
		spin_unlock_np(&w->lock);
		return;
	}
	t->triggered = true;
	list_add(&w->triggered, &t->link);
	if (w->waiting_th) {
		wth = w->waiting_th;
		w->waiting_th = NULL;
	}
	spin_unlock_np(&w->lock);

	if (wth)
		thread_ready(wth);
}
