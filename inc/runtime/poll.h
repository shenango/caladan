/*
 * poll.h - support for event polling (similar to select/epoll/poll, etc.)
 */

#pragma once

#include <base/stddef.h>
#include <base/list.h>
#include <runtime/thread.h>
#include <runtime/sync.h>

typedef struct poll_waiter {
	spinlock_t		lock;
	struct list_head	triggered;
	thread_t		*waiting_th;
} poll_waiter_t;

typedef struct poll_trigger {
	struct list_node	link;
	struct poll_waiter	*waiter;
	bool			triggered;
	unsigned long		data;
} poll_trigger_t;


/*
 * Waiter API
 */

extern void poll_init(poll_waiter_t *w);
extern void poll_arm(poll_waiter_t *w, poll_trigger_t *t, unsigned long data);
extern void poll_disarm(poll_trigger_t *t);
extern unsigned long poll_wait(poll_waiter_t *w);


/*
 * Trigger API
 */

/**
 * poll_trigger_init - initializes a trigger
 * @t: the trigger to initialize
 */
static inline void poll_trigger_init(poll_trigger_t *t)
{
	t->waiter = NULL;
	t->triggered = false;
}

extern void poll_trigger(poll_waiter_t *w, poll_trigger_t *t);
