/*
 * commands.c - dataplane commands to/from runtimes
 */

#include <rte_mbuf.h>

#include <base/log.h>
#include <base/lrpc.h>
#include <iokernel/queue.h>

#include "defs.h"

static struct rte_mbuf *free_rx_mbuf(struct thread *t, unsigned long payload)
{
	struct rte_mbuf *buf = mbuf_from_addr(payload);
	if (unlikely(!buf))
		return NULL;

	/* validate this buf was owned by this proc */
	struct rx_priv_data *pdata = rte_mbuf_to_priv(buf);
	if (unlikely(pdata->owner != t->p))
		return NULL;

	pdata->owner = NULL;
	list_del_from(&t->p->owned_rx_bufs, &pdata->link);
	return buf;
}

static int commands_drain_queue(struct thread *t, struct rte_mbuf **bufs, int n,
	                        bool *error)
{
	int i, n_bufs = 0;
	struct rte_mbuf *buf;

	for (i = 0; i < n; i++) {
		union txcmdq_cmd cmd;
		unsigned long payload;

		if (!lrpc_recv(&t->txcmdq, &cmd.lrpc_cmd, &payload))
			break;

		switch (cmd.txcmd) {
		case TXCMD_NET_COMPLETE:
			buf = free_rx_mbuf(t, payload);
			if (unlikely(!buf)) {
				log_err("received invalid response");
				goto error;
			}
			bufs[n_bufs++] = buf;
			break;

		default:
			log_err("invalid command: %hu", cmd.txcmd);
			goto error;
		}
	}

	return n_bufs;

error:
	t->p->dataplane_error = true;
	dp_clients_remove_client(t->p);
	*error = true;
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
	bool error = false;

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
				IOKERNEL_CMD_BURST_SIZE - n_bufs, &error);

		if (unlikely(error))
			break;

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
