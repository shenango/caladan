/*
 * commands.c - dataplane commands to/from runtimes
 */

#include <rte_mbuf.h>

#include <base/log.h>
#include <base/lrpc.h>
#include <iokernel/queue.h>

#include "defs.h"

static int commands_drain_queue(struct thread *t, struct rte_mbuf **bufs, int n)
{
	int i, n_bufs = 0;

	for (i = 0; i < n; i++) {
		uint64_t cmd;
		unsigned long payload;

		if (!lrpc_recv(&t->txcmdq, &cmd, &payload))
			break;

		switch (cmd) {
		case TXCMD_NET_COMPLETE:
			bufs[n_bufs++] = (struct rte_mbuf *)payload;
			/* TODO: validate pointer @buf */
			break;

		default:
			/* kill the runtime? */
			BUG();
		}
	}

	return n_bufs;
}

/*
 * Process a batch of commands from runtimes.
 */
bool commands_rx(void)
{
	struct rte_mbuf *bufs[IOKERNEL_CMD_BURST_SIZE];
	int i, n_bufs = 0;
	static unsigned int pos;
	struct thread *t;

	/*
	 * Poll each thread in each runtime until all have been polled or we
	 * have processed CMD_BURST_SIZE commands.
	 */
	for (i = 0; i < nrts; i++) {
		unsigned int idx = pos++ % nrts;

		if (n_bufs >= IOKERNEL_CMD_BURST_SIZE)
			break;

		t = ts[idx];
		n_bufs += commands_drain_queue(t, &bufs[n_bufs],
				IOKERNEL_CMD_BURST_SIZE - n_bufs);

		if (unlikely(!t->active)) {
			if (!lrpc_empty(&t->txcmdq))
				continue;
			if (lrpc_empty(&t->txpktq) || t->p->kill)
				unpoll_thread(t);
		}
	}

	STAT_INC(COMMANDS_PULLED, n_bufs);
	rte_pktmbuf_free_bulk(bufs, n_bufs);
	return n_bufs > 0;
}
