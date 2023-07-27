/*
 * timer.c - support for timers
 *
 * So far we use a D-ary heap just like the Go runtime. We may want to consider
 * adding a lower-resolution shared timer wheel as well.
 */

#include <limits.h>
#include <stdlib.h>

#include <base/time.h>
#include <runtime/sync.h>
#include <runtime/thread.h>
#include <runtime/timer.h>

#include "defs.h"

/* the arity of the heap */
#define D	4

/**
 * is_valid_heap - checks that the timer heap is a valid min heap
 * @heap: the timer heap
 * @n: the number of timers in the heap
 *
 * Returns true if valid, false otherwise.
 */
static bool is_valid_heap(struct timer_idx *heap, int n)
{
	int i, p;

	/* check that each timer's deadline is later or equal to its parent's
	 * deadline */
	for (i = n-1; i > 1; i--) {
		p = (i - 1) / D;
		if (heap[p].deadline_us > heap[i].deadline_us)
			return false;
	}

	return true;
}

/**
 * timer_heap_is_valid - checks that this kthread's timer heap is a
 * valid min heap
 * @k: the kthread
 */
static void assert_timer_heap_is_valid(struct kthread *k)
{
	assert(is_valid_heap(k->timers, k->timern));
}

static void sift_up(struct timer_idx *heap, int i)
{
	struct timer_idx tmp = heap[i];
	int p;

	while (i > 0) {
		p = (i - 1) / D;
		if (tmp.deadline_us >= heap[p].deadline_us)
			break;
		heap[i] = heap[p];
		heap[i].e->idx = i;
		heap[p] = tmp;
		heap[p].e->idx = p;
		i = p;
	}
}

static void sift_down(struct timer_idx *heap, int i, int n)
{
	struct timer_idx tmp = heap[i];
	uint64_t w;
	int c, j;

	while (1) {
		w = tmp.deadline_us;
		c = INT_MAX;
		for (j = (i * D + 1); j <= (i * D + D); j++) {
			if (j >= n)
				break;
			if (heap[j].deadline_us < w) {
				w = heap[j].deadline_us;
				c = j;
			}
		}
		if (c == INT_MAX)
			break;
		heap[i] = heap[c];
		heap[i].e->idx = i;
		heap[c] = tmp;
		heap[c].e->idx = c;
		i = c;
	}
}

static void update_q_ptrs(struct kthread *k)
{
	uint64_t next_tsc = UINT64_MAX;

	if (k->timern)
		next_tsc = k->timers[0].deadline_us * cycles_per_us + start_tsc;
	ACCESS_ONCE(k->q_ptrs->next_timer_tsc) = next_tsc;
}

/**
 * timer_earliest_deadline - return the first deadline for this kthread or 0 if
 * there are no active timers.
 */
uint64_t timer_earliest_deadline(void)
{
	struct kthread *k = myk();
	uint64_t deadline_us;

	/* deliberate race condition */
	if (k->timern == 0)
		deadline_us = 0;
	else
		deadline_us = k->timers[0].deadline_us;

	return deadline_us;
}

static void timer_start_locked(struct timer_entry *e, uint64_t deadline_us)
{
	struct kthread *k = myk();
	int i;

	assert_spin_lock_held(&k->timer_lock);

	/* can't insert a timer twice! */
	BUG_ON(e->armed);

	i = k->timern++;
	if (k->timern >= RUNTIME_MAX_TIMERS) {
		/* TODO: support unlimited timers */
		BUG();
	}

	k->timers[i].deadline_us = deadline_us;
	k->timers[i].e = e;
	e->idx = i;
	e->localk = k;
	sift_up(k->timers, i);
	e->armed = true;
	e->executing = false;
}

/**
 * timer_start - arms a timer
 * @e: the timer entry to start
 * @deadline_us: the deadline in microseconds
 *
 * @e must have been initialized with timer_init().
 */
void timer_start(struct timer_entry *e, uint64_t deadline_us)
{
	struct kthread *k = getk();

	spin_lock(&k->timer_lock);
	timer_start_locked(e, deadline_us);
	update_q_ptrs(k);
	spin_unlock(&k->timer_lock);
	putk();
}

/**
 * timer_cancel - cancels a timer
 * @e: the timer entry to cancel
 *
 * Returns true if the timer was successfully cancelled, otherwise it has
 * already fired or was never armed.
 */
bool __timer_cancel(struct timer_entry *e)
{
	struct kthread *k = e->localk;
	int last;

	spin_lock_np(&k->timer_lock);

	if (!e->armed) {
		spin_unlock_np(&k->timer_lock);
		if (unlikely(load_acquire(&e->executing))) {
			while (load_acquire(&e->executing))
				cpu_relax();
		}
		return false;
	}
	e->armed = false;

	last = --k->timern;
	if (e->idx == last) {
		update_q_ptrs(k);
		spin_unlock_np(&k->timer_lock);
		return true;
	}

	k->timers[e->idx] = k->timers[last];
	k->timers[e->idx].e->idx = e->idx;
	sift_up(k->timers, e->idx);
	sift_down(k->timers, e->idx, k->timern);
	update_q_ptrs(k);
	spin_unlock_np(&k->timer_lock);

	return true;
}

static void timer_finish_sleep(unsigned long arg)
{
	thread_t *th = (thread_t *)arg;
	thread_ready(th);
}

static void __timer_sleep(uint64_t deadline_us)
{
	struct kthread *k;
	struct timer_entry e;

	timer_init(&e, timer_finish_sleep, (unsigned long)thread_self());

	k = getk();
	spin_lock_np(&k->timer_lock);
	putk();
	timer_start_locked(&e, deadline_us);
	update_q_ptrs(k);
	thread_park_and_unlock_np(&k->timer_lock);

	timer_finish(&e);
}

/**
 * timer_sleep_until - sleeps until a deadline
 * @deadline_us: the deadline time in microseconds
 */
void timer_sleep_until(uint64_t deadline_us)
{
	if (unlikely(microtime() >= deadline_us))
		return;

	__timer_sleep(deadline_us);
}

/**
 * timer_sleep - sleeps for a duration
 * @duration_us: the duration time in microseconds
 */
void timer_sleep(uint64_t duration_us)
{
	__timer_sleep(microtime() + duration_us);
}

static void timer_softirq_one(struct kthread *k)
{
	struct timer_entry *e;
	uint64_t now_us;
	int i;

	spin_lock(&k->timer_lock);
	assert_timer_heap_is_valid(k);

	now_us = microtime();
	while (!preempt_needed() && k->timern > 0 &&
	       k->timers[0].deadline_us <= now_us) {
		i = --k->timern;
		e = k->timers[0].e;
		if (i > 0) {
			k->timers[0] = k->timers[i];
			k->timers[0].e->idx = 0;
			sift_down(k->timers, 0, i);
		}
		update_q_ptrs(k);
		e->executing = true;
		store_release(&e->armed, false);
		spin_unlock(&k->timer_lock);

		/* execute the timer handler */
		e->fn(e->arg);
		store_release(&e->executing, false);
		spin_lock(&k->timer_lock);
		now_us = microtime();
	}

	spin_unlock(&k->timer_lock);
}

static void timer_softirq(void *arg)
{
	struct kthread *k = arg;

	while (true) {
		preempt_disable();
		timer_softirq_one(k);
		k->timer_busy = false;
		thread_park_and_preempt_enable();
	}
}

/**
 * timer_init_thread - initializes per-thread timer state
 *
 * Returns 0 if successful, otherwise fail.
 */
int timer_init_thread(void)
{
	struct kthread *k = myk();
	thread_t *th;

	k->timers = aligned_alloc(CACHE_LINE_SIZE,
			align_up(sizeof(struct timer_idx) * RUNTIME_MAX_TIMERS,
				 CACHE_LINE_SIZE));
	if (!k->timers)
		return -ENOMEM;

	th = thread_create(timer_softirq, k);
	if (!th)
		return -ENOMEM;

	k->timer_softirq = th;
	k->q_ptrs->next_timer_tsc = UINT64_MAX;
	return 0;
}
