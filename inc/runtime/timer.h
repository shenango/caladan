/*
 * timer.h - support for timers
 */

#pragma once

#include <asm/atomic.h>
#include <asm/ops.h>
#include <base/stddef.h>

typedef void (*timer_fn_t)(unsigned long arg);

struct kthread;

struct timer_entry {
	bool		armed;
	bool		executing;
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
	e->executing = false;
	e->fn = fn;
	e->arg = arg;
}

/**
 * timer_finish - de-initializes a timer that has already expired
 * @e: the timer entry
 *
 * Ensures that it is safe to reclaim the memory for timer_entry.
 * This function may spin temporarily if racing with the timer firing code.
 * Should not be called on a timer that hasn't expired yet - use timer_cancel
 * instead.
 */
static inline void timer_finish(struct timer_entry *e)
{
	assert(!e->armed);

	if (unlikely(load_acquire(&e->executing)))
		while (load_acquire(&e->executing))
			cpu_relax();
}

static inline bool timer_busy(const struct timer_entry *e)
{
	return load_acquire(&e->armed) || load_acquire(&e->executing);
}

extern void timer_start(struct timer_entry *e, uint64_t deadline_us);
extern bool __timer_cancel(struct timer_entry *e);
static inline bool timer_cancel(struct timer_entry *e)
{
	if (!load_acquire(&e->armed)) {
		if (unlikely(load_acquire(&e->executing))) {
			while (load_acquire(&e->executing))
				cpu_relax();
		}
		return false;
	}

	return __timer_cancel(e);
}


/*
 * High-level API
 */

extern void timer_sleep_until(uint64_t deadline_us);
extern void timer_sleep(uint64_t duration_us);
