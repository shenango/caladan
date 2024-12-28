/*
 * tx.c - the transmission path for the I/O kernel (runtimes -> network)
 */

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>

#include <base/log.h>
#include <iokernel/queue.h>

#include "defs.h"

#define TX_PREFETCH_STRIDE 2

unsigned int nrts;
struct thread *ts[NCPU];

static struct rte_mempool *tx_mbuf_pool;

static LIST_HEAD(overflow_procs);

struct pkt {
	union txpkt_xmit_cmd cmd;
	unsigned long	compl;
	char		*buffer;
	struct thread	*th;
	uint32_t	dst_ip;
	uint16_t	hash;
};

/*
 * Prepare rte_mbuf struct for transmission.
 */
static void tx_prepare_tx_mbuf(struct rte_mbuf *buf, struct pkt *pkt)
{
	struct proc *p = pkt->th->p;
	uint32_t page_number;
	struct tx_pktmbuf_priv *priv_data;

	/* initialize mbuf to point to net_hdr->payload */
	buf->buf_addr = pkt->buffer;

	if (!dp.iova_mode_pa)
		buf->buf_iova = (rte_iova_t)pkt->buffer;
	else if (p->uses_hugepages) {
		page_number = PGN_2MB(buf->buf_addr - p->region.base);
		buf->buf_iova = p->page_paddrs[page_number];
		buf->buf_iova += PGOFF_2MB(buf->buf_addr);
	} else {
		page_number = PGN_4KB(buf->buf_addr - p->region.base);
		buf->buf_iova = p->page_paddrs[page_number];
		buf->buf_iova += PGOFF_4KB(buf->buf_addr);
	}

	buf->data_off = 0;
	rte_mbuf_refcnt_set(buf, 1);

	buf->buf_len = pkt->cmd.len;
	buf->pkt_len = pkt->cmd.len;
	buf->data_len = pkt->cmd.len;

	buf->ol_flags = 0;
	if (pkt->cmd.olflags != 0 && !cfg.tx_offloads_disabled) {
		if (pkt->cmd.olflags & OLFLAG_IP_CHKSUM)
			buf->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
		if (pkt->cmd.olflags & OLFLAG_TCP_CHKSUM)
			buf->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
		if (pkt->cmd.olflags & OLFLAG_IPV4)
			buf->ol_flags |= RTE_MBUF_F_TX_IPV4;
		if (pkt->cmd.olflags & OLFLAG_IPV6)
			buf->ol_flags |= RTE_MBUF_F_TX_IPV6;

		buf->l4_len = sizeof(struct rte_tcp_hdr);
		buf->l3_len = sizeof(struct rte_ipv4_hdr);
		buf->l2_len = RTE_ETHER_HDR_LEN;
	}

	/* initialize the private data, used to send completion events */
	priv_data = rte_mbuf_to_priv(buf);
	priv_data->p = p;
	priv_data->th = pkt->th;
	priv_data->completion_data = pkt->compl;
	priv_data->dst_ip = 0;

	if (pkt->cmd.olflags & TXFLAG_LOCAL_HINT) {
		buf->hash.rss = pkt->hash;
		buf->ol_flags |= RTE_MBUF_F_RX_RSS_HASH;
		priv_data->dst_ip = pkt->cmd.dst_ip;
	}

#ifdef MLX
	/* initialize private data used by Mellanox driver to register memory */
	priv_data->lkey = p->lkey;
#endif /* MLX */

	/* reference count @p so it doesn't get freed before the completion */
	proc_get(p);
}

/*
 * Send a completion event to the runtime for the mbuf pointed to by obj.
 */
bool tx_send_completion(void *obj)
{
	struct rte_mbuf *buf;
	struct tx_pktmbuf_priv *priv_data;
	struct thread *th;
	struct proc *p;

	buf = (struct rte_mbuf *)obj;
	priv_data = rte_mbuf_to_priv(buf);
	p = priv_data->p;

	/* during initialization, the mbufs are enqueued for the first time */
	if (unlikely(!p))
		return true;

	/* check if runtime is still registered */
	if(unlikely(p->kill)) {
		proc_put(p);
		return true; /* no need to send a completion */
	}

	/* send completion to runtime */
	th = priv_data->th;
	union rxq_cmd cmd = {0};
	cmd.rxcmd = RX_NET_COMPLETE;
	if (th->active) {
		if (likely(lrpc_send(&th->rxq, cmd.lrpc_cmd,
			       priv_data->completion_data))) {
			goto success;
		}
	} else {
		if (likely(rx_send_to_runtime(p, p->next_thread_rr++, cmd.lrpc_cmd,
					priv_data->completion_data))) {
			goto success;
		}
	}

	if (unlikely(p->nr_overflows == p->max_overflows)) {
		log_warn("tx: Completion overflow queue is full");
		return false;
	}

	if (!p->nr_overflows)
		list_add(&overflow_procs, &p->overflow_link);

	p->overflow_queue[p->nr_overflows++] = priv_data->completion_data;
	log_debug_ratelimited("tx: failed to send completion to runtime");
	STAT_INC(COMPLETION_ENQUEUED, -1);
	STAT_INC(TX_COMPLETION_OVERFLOW, 1);


success:
	proc_put(p);
	STAT_INC(COMPLETION_ENQUEUED, 1);
	return true;
}

static int drain_overflow_queue(struct proc *p, int n)
{
	int i = 0;
	union rxq_cmd cmd = {0};
	cmd.rxcmd = RX_NET_COMPLETE;

	while (p->nr_overflows > 0 && i < n) {
		if (!rx_send_to_runtime(p, p->next_thread_rr++, cmd.lrpc_cmd,
				p->overflow_queue[--p->nr_overflows])) {
			p->nr_overflows++;
			break;
		}
		i++;
	}
	return i;
}

bool tx_drain_completions(void)
{
	size_t drained = 0;
	struct proc *p, *p_next;
	struct list_head done;

	if (list_empty(&overflow_procs))
		return false;

	list_head_init(&done);

	list_for_each_safe(&overflow_procs, p, p_next, overflow_link) {
		drained += drain_overflow_queue(p, IOKERNEL_OVERFLOW_BATCH_DRAIN - drained);
		list_del_from(&overflow_procs, &p->overflow_link);
		if (p->nr_overflows)
			list_add_tail(&done, &p->overflow_link);

		if (drained >= IOKERNEL_OVERFLOW_BATCH_DRAIN)
			break;
	}

	list_append_list(&overflow_procs, &done);

	STAT_INC(COMPLETION_DRAINED, drained);

	return drained > 0;

}

static int tx_drain_queue(struct thread *t, int n, struct pkt *pkts)
{
	int i;
	struct pkt *pkt;

	for (i = 0; i < n; i++) {
		pkt = &pkts[i];

		if (!lrpc_recv(&t->txpktq, &pkt->cmd.lrpc_cmd, &pkt->compl))
			break;

		assert(pkt->cmd.txcmd == TXPKT_NET_XMIT);
		pkt->hash = rss_from_txpkt_payload(pkt->compl);
		pkt->compl = ptr_from_txpkt_payload(pkt->compl);
		pkt->dst_ip = pkt->cmd.dst_ip;
		pkt->th = t;
		pkt->buffer = shmptr_to_ptr(&t->p->region, pkt->compl,
				             pkt->cmd.len);
		if (unlikely(!pkt->buffer)) {
			log_warn("bad tx packet pointer");
			t->p->dataplane_error = true;
			dp_clients_remove_client(t->p);
			return i;
		}
	}

	return i;
}


/*
 * Process a batch of outgoing packets.
 */
bool tx_burst(void)
{
	/* packets are unassociated with an mbuf as of yet. */
	static struct pkt pkts[IOKERNEL_TX_BURST_SIZE];
	static unsigned int n_pkts;

	static struct rte_mbuf *pending_bufs[IOKERNEL_TX_BURST_SIZE];
	static unsigned int n_pending;

	struct rte_mbuf *tmp_bufs[IOKERNEL_TX_BURST_SIZE];

	struct rte_mbuf *loopback_bufs[IOKERNEL_TX_BURST_SIZE];
	unsigned int n_loopback = 0;

	int i, ret, budget;
	static unsigned int pos;
	struct thread *t;
	bool work_done = false;

	budget = IOKERNEL_TX_BURST_SIZE - (n_pkts + n_pending);
	budget = MIN(dma_chan_slots_avail(), budget);

	/*
	 * Poll each kthread in each runtime until all have been polled or we
	 * have PKT_BURST_SIZE pkts.
	 */
	for (i = 0; i < nrts && budget > 0; i++) {
		if (++pos >= nrts)
			pos = 0;
		t = ts[pos];
		ret = tx_drain_queue(t, budget, &pkts[n_pkts]);
		STAT_INC(TX_PULLED, ret);
		n_pkts += ret;
		budget -= ret;
	}

	/* allocate mbufs */
	if (n_pkts > 0) {
		ret = rte_mempool_get_bulk(tx_mbuf_pool, (void **)&tmp_bufs[0],
					n_pkts);
		if (unlikely(ret)) {
			stats[TX_COMPLETION_FAIL] += n_pkts;
			log_warn_ratelimited("tx: error getting %d mbufs from mempool", n_pkts);
			return true;
		}

		/* fill in packet metadata */
		for (i = 0; i < n_pkts; i++) {
			tx_prepare_tx_mbuf(tmp_bufs[i], &pkts[i]);

			if (dp.loopback_en &&
			    pkts[i].cmd.olflags & (TXFLAG_LOCAL | TXFLAG_BROADCAST)) {
				loopback_bufs[n_loopback++] = tmp_bufs[i];
				if (pkts[i].cmd.olflags & TXFLAG_BROADCAST) {
					rte_mbuf_refcnt_set(tmp_bufs[i], 2);
					pending_bufs[n_pending++] = tmp_bufs[i];
				}
			} else {
				pending_bufs[n_pending++] = tmp_bufs[i];
			}
		}
		n_pkts = 0;
		work_done = true;
	}

	if (n_pending) {
		/* finally, send the packets on the wire */
		ret = rte_eth_tx_burst(dp.port, 0, pending_bufs, n_pending);
		log_debug("tx: transmitted %d packets on port %d", ret, dp.port);

		/* apply back pressure if the NIC TX ring was full */
		if (unlikely(ret < n_pending)) {
			STAT_INC(TX_BACKPRESSURE, n_pending - ret);
			for (i = 0; i < n_pending; i++)
				pending_bufs[i] = pending_bufs[ret + i];
		}
		n_pending -= ret;
		work_done = true;
	}

	if (n_loopback) {
		log_debug("tx: transmitted %d packets loopback!", n_loopback);
		rx_loopback(loopback_bufs, n_loopback);
		work_done = true;
	}

	return work_done;
}

/*
 * Zero out private data for a packet
 */

static void tx_pktmbuf_priv_init(struct rte_mempool *mp, void *opaque,
				 void *obj, unsigned obj_idx)
{
	struct rte_mbuf *buf = obj;
	struct tx_pktmbuf_priv *data = rte_mbuf_to_priv(buf);
	memset(data, 0, sizeof(*data));
}

/*
 * Create and initialize a packet mbuf pool for holding struct mbufs and
 * handling completion events. Actual buffer memory is separate, in shared
 * memory.
 */
static struct rte_mempool *tx_pktmbuf_completion_pool_create(const char *name,
		unsigned n, uint16_t priv_size, int socket_id)
{
	struct rte_mempool *mp;
	struct rte_pktmbuf_pool_private mbp_priv = {0};
	unsigned elt_size;
	int ret;

	if (RTE_ALIGN(priv_size, RTE_MBUF_PRIV_ALIGN) != priv_size) {
		log_err("tx: mbuf priv_size=%u is not aligned", priv_size);
		rte_errno = EINVAL;
		return NULL;
	}
	elt_size = sizeof(struct rte_mbuf) + (unsigned)priv_size;
	mbp_priv.mbuf_data_room_size = 0;
	mbp_priv.mbuf_priv_size = priv_size;

	mp = rte_mempool_create_empty(name, n, elt_size, 0,
		 sizeof(struct rte_pktmbuf_pool_private), socket_id, 0);
	if (mp == NULL)
		return NULL;

	ret = rte_mempool_set_ops_byname(mp, "completion", NULL);
	if (ret != 0) {
		log_err("tx: error setting mempool handler");
		rte_mempool_free(mp);
		rte_errno = -ret;
		return NULL;
	}
	rte_pktmbuf_pool_init(mp, &mbp_priv);

	ret = rte_mempool_populate_default(mp);
	if (ret < 0) {
		rte_mempool_free(mp);
		rte_errno = -ret;
		return NULL;
	}

	rte_mempool_obj_iter(mp, rte_pktmbuf_init, NULL);
	rte_mempool_obj_iter(mp, tx_pktmbuf_priv_init, NULL);

	return mp;
}

/*
 * Initialize tx state.
 */
int tx_init(void)
{
	if (cfg.vfio_directpath)
		return 0;

	/* create a mempool to hold struct rte_mbufs and handle completions */
	tx_mbuf_pool = tx_pktmbuf_completion_pool_create("TX_MBUF_POOL",
			IOKERNEL_NUM_COMPLETIONS, sizeof(struct tx_pktmbuf_priv),
			rte_socket_id());

	if (tx_mbuf_pool == NULL) {
		log_err("tx: couldn't create tx mbuf pool");
		return -1;
	}

	return 0;
}
