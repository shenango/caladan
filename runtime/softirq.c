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

static bool softirq_timer_pending(struct kthread *k, uint64_t now_tsc)
{
	uint64_t now_us = (now_tsc - start_tsc) / cycles_per_us;

	return ACCESS_ONCE(k->timern) > 0 &&
	       ACCESS_ONCE(k->timers[0].deadline_us) <= now_us;
}

/**
 * softirq_pending - is there a softirq pending?
 */
bool softirq_pending(struct kthread *k, uint64_t now_tsc)
{
	return softirq_iokernel_pending(k) || softirq_timer_pending(k, now_tsc) ||
	       storage_available_completions(k);
}

/**
 * softirq_run_locked - schedule softirq work with kthread lock held
 * @k: the kthread to check for softirq work
 *
 * The kthread's lock must be held when calling this function.
 *
 * Returns true if softirq work was scheduled.
 */
bool softirq_run_locked(struct kthread *k)
{
	uint64_t now_tsc = rdtsc();
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
	work_done |= rx_poll_locked(k);

	/* check for timer softirq work */
	if (!k->timer_busy && softirq_timer_pending(k, now_tsc)) {
		k->timer_busy = true;
		thread_ready_head_locked(k->timer_softirq);
		work_done = true;
	}

	/* check for storage softirq work */
	if (!k->storage_busy && storage_available_completions(k)) {
		k->storage_busy = true;
		thread_ready_head_locked(k->storage_softirq);
		work_done = true;
	}

	k->last_softirq_tsc = now_tsc;
	return work_done;
}

/**
 * softirq_run - schedule softirq work
 *
 * Returns true if softirq work was scheduled.
 */
bool softirq_run(void)
{
	struct kthread *k;
	uint64_t now_tsc = rdtsc();
	bool work_done;

	k = getk();
	k->last_softirq_tsc = now_tsc;

	if (!softirq_pending(k, now_tsc)) {
		work_done = rx_poll(k);
		putk();
		return work_done;
	}
	spin_lock(&k->lock);
	work_done = softirq_run_locked(k);
	spin_unlock(&k->lock);
	putk();

	return work_done;
}
