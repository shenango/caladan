/*
 * rcu.c - support for read-copy-update
 *
 * The main challenge of RCU is determining when it's safe to free objects. The
 * strategy here is to maintain a per-kthread counter. Whenever the scheduler is
 * entered or exited, the counter is incremented. When the count is even, we
 * know that either the scheduler loop is still running or the kthread is
 * parked. When the count is odd, we know a uthread is currently running. We can
 * safely free objects by reading each kthread's counter and then waiting until
 * each kthread count is either even & >= the previous value (to detect parking)
 * or odd & > the previous value (to detect rescheduling).
 *
 * FIXME: Freeing objects is expensive with this minimal implementation. This
 * should be fine as long as RCU updates are rare. The Linux Kernel uses several
 * more optimized strategies that we may want to consider in the future.
 */

#include <base/stddef.h>
#include <base/lock.h>
#include <runtime/rcu.h>
#include <runtime/sync.h>
#include <runtime/thread.h>
#include <runtime/timer.h>

#include "defs.h"

/* the time RCU waits before checking if it can free objects */
#define RCU_SLEEP_PERIOD (10 * ONE_MS)

/* Protects @rcu_head. */
static DEFINE_SPINLOCK(rcu_lock);
/* The head of the list of objects waiting to be freed */
static struct rcu_head *rcu_head;
/* rcu worker thread - NULL when running */
static thread_t *rcu_worker_th;

#ifdef DEBUG
DEFINE_PERTHREAD(int, rcu_read_count);
#endif /* DEBUG */

static void rcu_worker(void *arg)
{
	struct rcu_head *head, *next;
	unsigned int last_rcu_gen[NCPU];
	unsigned int gen;
	int i;

	while (true) {
		/* check if any RCU objects are waiting to be freed */
		spin_lock_np(&rcu_lock);
		if (!rcu_head) {
			rcu_worker_th = thread_self();
			thread_park_and_unlock_np(&rcu_lock);
			continue;
		}
		head = rcu_head;
		rcu_head = NULL;
		spin_unlock_np(&rcu_lock);

		/* read the RCU generation counters */
		for (i = 0; i < maxks; i++)
			last_rcu_gen[i] = load_acquire(&ks[i]->rcu_gen);

		while (true) {
			/* wait for RCU generation counters to increase */
			timer_sleep(RCU_SLEEP_PERIOD);

			/* read the RCU generation counters again */
			for (i = 0; i < maxks; i++) {
				gen = load_acquire(&ks[i]->rcu_gen);
				if ((gen & 0x1) == 0x1 &&
				    gen == last_rcu_gen[i]) {
					break;
				}
			}

			/* did any of the RCU generation checks fail? */
			if (i != maxks)
				continue;

			/* actually free the RCU objects */
			while (head) {
				next = head->next;
				head->func(head);
				head = next;
			}

			break;
		}
	}
}

/**
 * rcu_free - frees an RCU object after the quiescent period
 * @head: the RCU head structure embedded within the object
 * @func: the release method
 */
void rcu_free(struct rcu_head *head, rcu_callback_t func)
{
	thread_t *th = NULL;

	head->func = func;

	spin_lock_np(&rcu_lock);
	swapvars(th, rcu_worker_th);
	head->next = rcu_head;
	rcu_head = head;
	spin_unlock_np(&rcu_lock);

	if (th)
		thread_ready(th);
}

struct sync_arg {
	struct rcu_head rcu;
	thread_t *th;
};

static void synchronize_rcu_finish(struct rcu_head *head)
{
	struct sync_arg *tmp = container_of(head, struct sync_arg, rcu);
	thread_ready(tmp->th);
}

/**
 * synchronize_rcu - blocks until it is safe to free an RCU object
 *
 * WARNING: Can only be called from thread context.
 */
void synchronize_rcu(void)
{
	struct sync_arg tmp;

	tmp.rcu.func = synchronize_rcu_finish;
	tmp.th = thread_self();

	spin_lock_np(&rcu_lock);
	tmp.rcu.next = rcu_head;
	rcu_head = &tmp.rcu;

	if (rcu_worker_th) {
		thread_ready(rcu_worker_th);
		rcu_worker_th = NULL;
	}

	thread_park_and_unlock_np(&rcu_lock);
}

/**
 * rcu_init_late - starts the RCU reclaim thread
 *
 * Returns 0 if succesful.
 */
int rcu_init_late(void)
{
	return thread_spawn(rcu_worker, NULL);
}
