/*
 * softirq.c - handles backend processing (I/O, timers, ingress packets, etc.)
 */

#include <base/stddef.h>
#include <base/log.h>
#include <runtime/thread.h>

#include "defs.h"
#include "net/defs.h"

static bool softirq_iokernel_pending(struct kthread *k)
{
	return !lrpc_empty(&k->rxq);
}

static bool softirq_directpath_pending(struct kthread *k)
{
	return rx_pending(k->directpath_rxq);
}

static bool softirq_timer_pending(struct kthread *k)
{
	return ACCESS_ONCE(k->timern) > 0 &&
	       ACCESS_ONCE(k->timers[0].deadline_us) <= microtime();
}

static bool softirq_storage_pending(struct kthread *k)
{
	return storage_available_completions(&k->storage_q);
}

/**
 * softirq_pending - is there a softirq pending?
 */
bool softirq_pending(struct kthread *k)
{
	return softirq_iokernel_pending(k) || softirq_directpath_pending(k) ||
	       softirq_timer_pending(k) || softirq_storage_pending(k);
}

/**
 * softirq_sched - schedule softirq work in scheduler context
 * @k: the kthread to check for softirq work
 *
 * The kthread's lock must be held when calling this function.
 *
 * Returns true if softirq work was marked ready.
 */
bool softirq_sched(struct kthread *k)
{
	bool work_done = false;

	assert_preempt_disabled();
	assert_spin_lock_held(&k->lock);

	/* check for iokernel softirq work */
	if (!k->iokernel_busy && softirq_iokernel_pending(k)) {
		k->iokernel_busy = true;
		thread_ready_head_locked(k->iokernel_softirq);
		work_done = true;
	}

	/* check for directpath softirq work */
	if (!k->directpath_busy && softirq_directpath_pending(k)) {
		k->directpath_busy = true;
		thread_ready_head_locked(k->directpath_softirq);
		work_done = true;
	}

	/* check for timer softirq work */
	if (!k->timer_busy && softirq_timer_pending(k)) {
		k->timer_busy = true;
		thread_ready_head_locked(k->timer_softirq);
		work_done = true;
	}

	/* check for storage softirq work */
	if (!k->storage_busy && softirq_storage_pending(k)) {
		k->storage_busy = true;
		thread_ready_head_locked(k->storage_softirq);
		work_done = true;
	}

	return work_done;
}

/**
 * softirq_run - schedule softirq work in thread context
 * @k: the kthread to check for softirq work
 *
 * Returns true if softirq work was marked ready.
 */
bool softirq_run(void)
{
	struct kthread *k;
	bool work_done = false;

	k = getk();
	if (!softirq_pending(k)) {
		putk();
		return false;
	}
	spin_lock(&k->lock);

	/* check for iokernel softirq work */
	if (!k->iokernel_busy && softirq_iokernel_pending(k)) {
		k->iokernel_busy = true;
		thread_ready_head_locked(k->iokernel_softirq);
		work_done = true;
	}

	/* check for directpath softirq work */
	if (!k->directpath_busy && softirq_directpath_pending(k)) {
		k->directpath_busy = true;
		thread_ready_head_locked(k->directpath_softirq);
		work_done = true;
	}

	/* check for timer softirq work */
	if (!k->timer_busy && softirq_timer_pending(k)) {
		k->timer_busy = true;
		thread_ready_head_locked(k->timer_softirq);
		work_done = true;
	}

	/* check for storage softirq work */
	if (!k->storage_busy && softirq_storage_pending(k)) {
		k->storage_busy = true;
		thread_ready_head_locked(k->storage_softirq);
		work_done = true;
	}

	spin_unlock(&k->lock);
	putk();

	return work_done;
}
