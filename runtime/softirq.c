/*
 * softirq.c - handles high priority events (timers, ingress packets, etc.)
 */

#include <base/stddef.h>
#include <base/log.h>
#include <runtime/thread.h>

#include "defs.h"
#include "net/defs.h"

struct softirq_work {
	unsigned int recv_cnt, compl_cnt, timer_budget, storage_cnt,
		direct_rx_cnt;
	struct kthread *k;
	struct mbuf *direct_reqs[SOFTIRQ_MAX_BUDGET];
	struct rx_net_hdr *recv_reqs[SOFTIRQ_MAX_BUDGET];
	struct mbuf *compl_reqs[SOFTIRQ_MAX_BUDGET];
	struct thread *storage_threads[SOFTIRQ_MAX_BUDGET];
};

static void softirq_fn(void *arg)
{
	struct softirq_work *w = arg;
	int i;

	/* complete TX requests and free packets */
	for (i = 0; i < w->compl_cnt; i++)
		mbuf_free(w->compl_reqs[i]);

#ifdef DIRECT_STORAGE
	for (i = 0; i < w->storage_cnt; i++)
		thread_ready(w->storage_threads[i]);
#endif

	/* deliver new RX packets to the runtime */
	net_rx_softirq(w->recv_reqs, w->recv_cnt);

#ifdef DIRECTPATH
	net_rx_softirq_direct(w->direct_reqs, w->direct_rx_cnt);
#endif

	/* handle any pending timeouts */
	if (timer_needed(w->k))
		timer_softirq(w->k, w->timer_budget);
}

static void softirq_gather_work(struct softirq_work *w, struct kthread *k,
				unsigned int budget)
{
	unsigned int recv_cnt = 0, compl_cnt = 0, real_budget;

	assert_spin_lock_held(&k->lock);

	real_budget = MIN(budget, SOFTIRQ_MAX_BUDGET);

	while (!preempt_needed()) {
		uint64_t cmd;
		unsigned long payload;

		if (!lrpc_recv(&k->rxq, &cmd, &payload))
			break;

		switch (cmd) {
		case RX_NET_RECV:
			w->recv_reqs[recv_cnt] =
				shmptr_to_ptr(&netcfg.rx_region,
					      (shmptr_t)payload,
					      MBUF_DEFAULT_LEN);
			BUG_ON(w->recv_reqs[recv_cnt] == NULL);
			recv_cnt++;
			break;

		case RX_NET_COMPLETE:
			w->compl_reqs[compl_cnt++] = (struct mbuf *)payload;
			break;

		default:
			log_err_ratelimited("net: invalid RXQ cmd '%ld'", cmd);
		}

		if (recv_cnt >= real_budget || compl_cnt >= SOFTIRQ_MAX_BUDGET)
			break;
	}

#ifdef DIRECTPATH
	w->direct_rx_cnt = net_ops.rx_batch(k->directpath_rxq, w->direct_reqs,
					    real_budget);
#endif

#ifdef DIRECT_STORAGE
	w->storage_cnt = storage_proc_completions(&k->storage_q, real_budget,
						  w->storage_threads);
#endif

	w->k = k;
	w->recv_cnt = recv_cnt;
	w->compl_cnt = compl_cnt;
	w->timer_budget = budget;
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

	/* check if there's any work available */
	if (!softirq_work_available(k))
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

	k = getk();
	if (!softirq_work_available(k)) {
		putk();
		return;
	}

	spin_lock(&k->lock);
	softirq_gather_work(&w, k, budget);
	spin_unlock(&k->lock);
	putk();

	softirq_fn(&w);
}
