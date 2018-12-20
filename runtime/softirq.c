/*
 * softirq.c - handles high priority events (timers, ingress packets, etc.)
 */

#include <base/stddef.h>
#include <base/log.h>
#include <runtime/thread.h>

#include "defs.h"
#include "net/defs.h"

struct softirq_work {
	unsigned int recv_cnt, compl_cnt, join_cnt, timer_budget, storage_cnt;
	struct kthread *k;
	struct rx_net_hdr *recv_reqs[SOFTIRQ_MAX_BUDGET];
	struct mbuf *compl_reqs[SOFTIRQ_MAX_BUDGET];
	struct kthread *join_reqs[SOFTIRQ_MAX_BUDGET];
	struct thread *storage_threads[SOFTIRQ_MAX_BUDGET];
};

static void softirq_fn(void *arg)
{
	struct softirq_work *w = arg;
	int i;

	/* complete TX requests and free packets */
	for (i = 0; i < w->compl_cnt; i++)
		mbuf_free(w->compl_reqs[i]);

	/* deliver new RX packets to the runtime */
	net_rx_softirq(w->recv_reqs, w->recv_cnt);

	/* handle any pending timeouts */
	if (timer_needed(w->k))
		timer_softirq(w->k, w->timer_budget);

	/* join parked kthreads */
	for (i = 0; i < w->join_cnt; i++)
		join_kthread(w->join_reqs[i]);

	for (i = 0; i < w->storage_cnt; i++)
		thread_ready(w->storage_threads[i]);

}

static void softirq_gather_work(struct softirq_work *w, struct kthread *k,
				unsigned int budget)
{
	unsigned int recv_cnt = 0, compl_cnt = 0, join_cnt = 0;
	int budget_left;

	budget_left = min(budget, SOFTIRQ_MAX_BUDGET);
	while (budget_left--) {
		uint64_t cmd;
		unsigned long payload;

		if (!lrpc_recv(&k->rxq, &cmd, &payload))
			break;

		switch (cmd) {
		case RX_NET_RECV:
			w->recv_reqs[recv_cnt] = shmptr_to_ptr(
				&netcfg.rx_region,
				(shmptr_t)payload, MBUF_DEFAULT_LEN);
			BUG_ON(w->recv_reqs[recv_cnt] == NULL);
			recv_cnt++;
			break;

		case RX_NET_COMPLETE:
			w->compl_reqs[compl_cnt++] = (struct mbuf *)payload;
			break;

		case RX_JOIN:
			w->join_reqs[join_cnt++] = (struct kthread *)payload;
			break;

		default:
			log_err_ratelimited("net: invalid RXQ cmd '%ld'", cmd);
		}
	}
#if __has_include("spdk/nvme.h")
	if (budget_left > 0)
		w->storage_cnt = storage_proc_completions(k, budget_left,
				w->storage_threads);
	else
		w->storage_cnt = 0;
#endif

	w->k = k;
	w->recv_cnt = recv_cnt;
	w->compl_cnt = compl_cnt;
	w->join_cnt = join_cnt;
	w->timer_budget = budget_left;
}

/**
 * softirq_run_thread - creates a closure for softirq handling
 * @k: the kthread from which to take RX queue commands
 * @budget: the maximum number of events to process
 *
 * Returns a thread that handles receive processing when executed or
 * NULL if no receive processing work is available.
 */
thread_t *softirq_run_thread(struct kthread *k, unsigned int budget)
{
	thread_t *th;
	struct softirq_work *w;
	bool work_available;

	assert_spin_lock_held(&k->lock);

	/* check if there's any work available */
	work_available = lrpc_empty(&k->rxq) && !timer_needed(k);
#if __has_include("spdk/nvme.h")
	work_available = work_available && !storage_available_completions(k);
#endif
	if (work_available)
		return NULL;

	th = thread_create_with_buf(softirq_fn, (void **)&w, sizeof(*w));
	if (unlikely(!th))
		return NULL;

	softirq_gather_work(w, k, budget);
	th->state = THREAD_STATE_RUNNABLE;
	return th;
}

/**
 * softirq_run - handles softirq processing in the current thread
 * @budget: the maximum number of events to process
 */
void softirq_run(unsigned int budget)
{
	struct kthread *k;
	struct softirq_work w;
	bool work_available;

	k = getk();
	/* check if there's any work available */
	work_available = lrpc_empty(&k->rxq) && !timer_needed(k);
#if __has_include("spdk/nvme.h")
	work_available = work_available && !storage_available_completions(k);
#endif
	if (work_available) {
		putk();
		return;
	}

	spin_lock(&k->lock);
	softirq_gather_work(&w, k, budget);
	spin_unlock(&k->lock);
	putk();

	softirq_fn(&w);
}
