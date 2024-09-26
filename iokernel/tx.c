/*
 * tx.c - the transmission path for the I/O kernel (runtimes -> network)
 */

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_hash.h>
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

/*
 * Private data stored in egress mbufs, used to send completions to runtimes.
 */
struct tx_pktmbuf_priv {
#ifdef MLX
	uint32_t lkey;
#endif /* MLX */
	struct proc	*p;
	struct thread	*th;
	unsigned long	completion_data;
};

static inline struct tx_pktmbuf_priv *tx_pktmbuf_get_priv(struct rte_mbuf *buf)
{
	return (struct tx_pktmbuf_priv *)(((char *)buf)
			+ sizeof(struct rte_mbuf));
}

/*
 * Prepare rte_mbuf struct for transmission.
 */
static void tx_prepare_tx_mbuf(struct rte_mbuf *buf,
			       const struct tx_net_hdr *net_hdr,
			       struct thread *th)
{
	struct proc *p = th->p;
	uint32_t page_number;
	struct tx_pktmbuf_priv *priv_data;

	/* initialize mbuf to point to net_hdr->payload */
	buf->buf_addr = (char *)net_hdr->payload;
	page_number = PGN_2MB((uintptr_t)buf->buf_addr - (uintptr_t)p->region.base);
	buf->buf_iova = p->page_paddrs[page_number] + PGOFF_2MB(buf->buf_addr);
	buf->data_off = 0;
	rte_mbuf_refcnt_set(buf, 1);

	buf->buf_len = net_hdr->len;
	buf->pkt_len = net_hdr->len;
	buf->data_len = net_hdr->len;

	buf->ol_flags = 0;
	if (net_hdr->olflags != 0) {
		if (net_hdr->olflags & OLFLAG_IP_CHKSUM)
			buf->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
		if (net_hdr->olflags & OLFLAG_TCP_CHKSUM)
			buf->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
		if (net_hdr->olflags & OLFLAG_IPV4)
			buf->ol_flags |= RTE_MBUF_F_TX_IPV4;
		if (net_hdr->olflags & OLFLAG_IPV6)
			buf->ol_flags |= RTE_MBUF_F_TX_IPV6;

		buf->l4_len = sizeof(struct rte_tcp_hdr);
		buf->l3_len = sizeof(struct rte_ipv4_hdr);
		buf->l2_len = RTE_ETHER_HDR_LEN;
	}

	/* initialize the private data, used to send completion events */
	priv_data = tx_pktmbuf_get_priv(buf);
	priv_data->p = p;
	priv_data->th = th;
	priv_data->completion_data = net_hdr->completion_data;

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
	priv_data = tx_pktmbuf_get_priv(buf);
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
	if (th->active) {
		if (likely(lrpc_send(&th->rxq, RX_NET_COMPLETE,
			       priv_data->completion_data))) {
			goto success;
		}
	} else {
		if (likely(rx_send_to_runtime(p, p->next_thread_rr++, RX_NET_COMPLETE,
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
	while (p->nr_overflows > 0 && i < n) {
		if (!rx_send_to_runtime(p, p->next_thread_rr++, RX_NET_COMPLETE,
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

static int tx_drain_queue(struct thread *t, int n,
			  const struct tx_net_hdr **hdrs)
{
	int i;

	for (i = 0; i < n; i++) {
		uint64_t cmd;
		unsigned long payload;

		if (!lrpc_recv(&t->txpktq, &cmd, &payload)) {
			if (unlikely(!t->active))
				unpoll_thread(t);
			break;
		}

		/* TODO: need to kill the process? */
		BUG_ON(cmd != TXPKT_NET_XMIT);

		hdrs[i] = shmptr_to_ptr(&t->p->region, payload,
					sizeof(struct tx_net_hdr));
		/* TODO: need to kill the process? */
		BUG_ON(!hdrs[i]);
	}

	return i;
}


/*
 * Process a batch of outgoing packets.
 */
bool tx_burst(void)
{
	const struct tx_net_hdr *hdrs[IOKERNEL_TX_BURST_SIZE];
	static struct rte_mbuf *bufs[IOKERNEL_TX_BURST_SIZE];
	struct thread *threads[IOKERNEL_TX_BURST_SIZE];
	int i, j, ret, pulltotal = 0;
	static unsigned int pos = 0, n_pkts = 0, n_bufs = 0;
	struct thread *t;

	/*
	 * Poll each kthread in each runtime until all have been polled or we
	 * have PKT_BURST_SIZE pkts.
	 */
	for (i = 0; i < nrts; i++) {
		unsigned int idx = (pos + i) % nrts;
		t = ts[idx];
		ret = tx_drain_queue(t, IOKERNEL_TX_BURST_SIZE - n_pkts,
				     &hdrs[n_pkts]);
		for (j = n_pkts; j < n_pkts + ret; j++)
			threads[j] = t;
		n_pkts += ret;
		pulltotal += ret;
		if (n_pkts >= IOKERNEL_TX_BURST_SIZE)
			goto full;
	}

	if (n_pkts == 0)
		return false;

	pos++;

full:

	stats[TX_PULLED] += pulltotal;

	/* allocate mbufs */
	if (n_pkts - n_bufs > 0) {
		ret = rte_mempool_get_bulk(tx_mbuf_pool, (void **)&bufs[n_bufs],
					n_pkts - n_bufs);
		if (unlikely(ret)) {
			stats[TX_COMPLETION_FAIL] += n_pkts - n_bufs;
			log_warn_ratelimited("tx: error getting %d mbufs from mempool", n_pkts - n_bufs);
			return true;
		}
	}

	/* fill in packet metadata */
	for (i = n_bufs; i < n_pkts; i++) {
		if (i + TX_PREFETCH_STRIDE < n_pkts)
			prefetch(hdrs[i + TX_PREFETCH_STRIDE]);
		tx_prepare_tx_mbuf(bufs[i], hdrs[i], threads[i]);
	}

	n_bufs = n_pkts;

	/* finally, send the packets on the wire */
	ret = rte_eth_tx_burst(dp.port, 0, bufs, n_pkts);
	log_debug("tx: transmitted %d packets on port %d", ret, dp.port);

	/* apply back pressure if the NIC TX ring was full */
	if (unlikely(ret < n_pkts)) {
		STAT_INC(TX_BACKPRESSURE, n_pkts - ret);
		n_pkts -= ret;
		for (i = 0; i < n_pkts; i++)
			bufs[i] = bufs[ret + i];
	} else {
		n_pkts = 0;
	}

	n_bufs = n_pkts;
	return true;
}

/*
 * Zero out private data for a packet
 */

static void tx_pktmbuf_priv_init(struct rte_mempool *mp, void *opaque,
				 void *obj, unsigned obj_idx)
{
	struct rte_mbuf *buf = obj;
	struct tx_pktmbuf_priv *data = tx_pktmbuf_get_priv(buf);
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
	struct rte_pktmbuf_pool_private mbp_priv;
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
