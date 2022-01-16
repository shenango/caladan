/*
 * timer.h - support for timers
 */

#pragma once

#include <base/stddef.h>

typedef void (*timer_fn_t)(unsigned long arg);

struct kthread;

struct timer_entry {
	bool		armed;
	unsigned int	idx;
	timer_fn_t	fn;
	unsigned long	arg;
	struct kthread *localk;
};


/*
 * Low-level API
 */

/**
 * timer_init - initializes a timer
 * @e: the timer entry to initialize
 * @fn: the timer handler (called when the timer fires)
 * @arg: an argument passed to the timer handler
 */
static inline void
timer_init(struct timer_entry *e, timer_fn_t fn, unsigned long arg)
{
	e->armed = false;
	e->fn = fn;
	e->arg = arg;
}

extern void timer_start(struct timer_entry *e, uint64_t deadline_us);
extern bool timer_cancel(struct timer_entry *e);

/**
 * timer_sleep_init - initializes a timer that wakes the thread that calls this function.
 * @e: timer entry to initialize. This entry can later be passed to timer_sleep_wait to perform the sleep (MUST be called from the same thread)
 * or timer_sleep_cancel to cancel the sleep and wake the thread immediately.
 */
extern void timer_sleep_init(struct timer_entry *e);
extern void timer_sleep_wait(struct timer_entry *e, uint64_t duration_us);
extern void timer_sleep_cancel(struct timer_entry *e);

/*
 * High-level API
 */

extern void timer_sleep_until(uint64_t deadline_us);
extern void timer_sleep(uint64_t duration_us);
