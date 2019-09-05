
#include <base/log.h>
#include <runtime/preempt.h>

#ifdef DIRECTPATH

#include <util/mmio.h>
#include <util/udma_barrier.h>

#include "mlx5.h"


/*
 * mlx5_refill_rxqueue - replenish RX queue with nrdesc bufs
 * @vq: queue to refill
 * @nrdesc: number of buffers to fill
 *
 * WARNING: nrdesc must not exceed the number of free slots in the RXq
 * returns 0 on success, errno on error
 */
static int mlx5_refill_rxqueue(struct mlx5_rxq *vq, int nrdesc)
{
	unsigned int i;
	uint32_t index;
	unsigned char *buf;
	struct mlx5_wqe_data_seg *seg;

	struct mlx5dv_rwq *wq = &vq->rx_wq_dv;

	assert(wraps_lte(nrdesc + vq->wq_head, vq->consumer_idx + wq->wqe_cnt));

	preempt_disable();

	for (i = 0; i < nrdesc; i++) {
		buf = tcache_alloc(&perthread_get(directpath_buf_pt));
		if (unlikely(!buf)) {
			preempt_enable();
			return -ENOMEM;
		}

		index = vq->wq_head++ & (wq->wqe_cnt - 1);
		seg = wq->buf + (index << vq->rx_wq_log_stride);
		seg->addr = htobe64((unsigned long)buf + RX_BUF_HEAD);
		vq->buffers[index] = buf;
	}

	udma_to_device_barrier();
	wq->dbrec[0] = htobe32(vq->wq_head & 0xffff);

	preempt_enable();

	return 0;

}

/*
 * mlx5_gather_completions - collect up to budget received packets and completions
 */
static int mlx5_gather_completions(struct mbuf **mbufs, struct mlx5_txq *v, unsigned int budget)
{
	struct mlx5dv_cq *cq = &v->tx_cq_dv;
	struct mlx5_cqe64 *cqe, *cqes = cq->buf;

	unsigned int compl_cnt;
	uint8_t opcode;
	uint16_t wqe_idx;

	for (compl_cnt = 0; compl_cnt < budget; compl_cnt++, v->cq_head++) {
		cqe = &cqes[v->cq_head & (cq->cqe_cnt - 1)];
		opcode = cqe_status(cqe, cq->cqe_cnt, v->cq_head);

		if (opcode == MLX5_CQE_INVALID)
			break;

		BUG_ON(opcode != MLX5_CQE_REQ);

		BUG_ON(mlx5_get_cqe_format(cqe) == 0x3);

		wqe_idx = be16toh(cqe->wqe_counter) & (v->tx_qp_dv.sq.wqe_cnt - 1);
		mbufs[compl_cnt] = load_acquire(&v->buffers[wqe_idx]);
	}

	cq->dbrec[0] = htobe32(v->cq_head & 0xffffff);

	return compl_cnt;
}

/*
 * mlx5_transmit_one - send one mbuf
 * @m: mbuf to send
 *
 * uses local kthread tx queue
 * returns 0 on success, -1 on error
 */
int mlx5_transmit_one(struct mbuf *m)
{
	struct kthread *k;
	struct mlx5_txq *v;
	struct mbuf *mbs[SQ_CLEAN_MAX];
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_eth_seg *eseg;
	struct mlx5_wqe_data_seg *dpseg;
	void *segment;
	uint32_t idx;
	int i, compl = 0;

	k = getk();
	v = container_of(k->directpath_txq, struct mlx5_txq, txq);
	idx = v->sq_head & (v->tx_qp_dv.sq.wqe_cnt - 1);

	if (nr_inflight_tx(v) >= SQ_CLEAN_THRESH) {
		compl = mlx5_gather_completions(mbs, v, SQ_CLEAN_MAX);
		for (i = 0; i < compl; i++)
			mbuf_free(mbs[i]);
		if (unlikely(nr_inflight_tx(v) >= v->tx_qp_dv.sq.wqe_cnt)) {
			putk();
			log_warn_ratelimited("txq full");
			return -1;
		}
	}

	segment = v->tx_qp_dv.sq.buf + (idx << v->tx_sq_log_stride);
	ctrl = segment;
	eseg = segment + sizeof(*ctrl);
	dpseg = (void *)eseg + ((offsetof(struct mlx5_wqe_eth_seg, inline_hdr) + MLX5_ETH_L2_INLINE_HEADER_SIZE) & ~0xf);

	ctrl->opmod_idx_opcode = htobe32(((v->sq_head & 0xffff) << 8) |
					       MLX5_OPCODE_SEND);

	assert(mbuf_length(m) >= MLX5_ETH_L2_INLINE_HEADER_SIZE);
	memcpy(eseg->inline_hdr_start, mbuf_data(m), MLX5_ETH_L2_INLINE_HEADER_SIZE);

	dpseg->byte_count = htobe32(mbuf_length(m) - MLX5_ETH_L2_INLINE_HEADER_SIZE);
	dpseg->addr = htobe64((uint64_t)mbuf_data(m) + MLX5_ETH_L2_INLINE_HEADER_SIZE);

	/* record buffer */
	store_release(&v->buffers[v->sq_head & (v->tx_qp_dv.sq.wqe_cnt - 1)], m);
	v->sq_head++;

	/* write doorbell record */
	udma_to_device_barrier();
	v->tx_qp_dv.dbrec[MLX5_SND_DBR] = htobe32(v->sq_head & 0xffff);

	/* ring bf doorbell */
	mmio_wc_start();
	mmio_write64_be(v->tx_qp_dv.bf.reg, *(__be64 *)ctrl);
	mmio_flush_writes();
	putk();

	return 0;

}

static void mbuf_fill_cqe(struct mbuf *m, struct mlx5_cqe64 *cqe)
{
	uint32_t len;

	len = be32toh(cqe->byte_cnt);

	mbuf_init(m, (unsigned char *)m + RX_BUF_HEAD, len, 0);
	m->len = len;
	m->csum_type = mlx5_csum_ok(cqe);
	m->csum = 0;
	m->rss_hash = mlx5_get_rss_result(cqe);
	m->release = directpath_rx_completion;
}

int mlx5_gather_rx(struct hardware_q *rxq, struct mbuf **ms, unsigned int budget)
{
	uint8_t opcode;
	uint16_t wqe_idx;
	int rx_cnt;

	struct mlx5_rxq *v = container_of(rxq, struct mlx5_rxq, rxq);
	struct mlx5dv_rwq *wq = &v->rx_wq_dv;
	struct mlx5dv_cq *cq = &v->rx_cq_dv;

	struct mlx5_cqe64 *cqe, *cqes = cq->buf;
	struct mbuf *m;

	for (rx_cnt = 0; rx_cnt < budget; rx_cnt++, v->consumer_idx++) {
		cqe = &cqes[v->consumer_idx & (cq->cqe_cnt - 1)];
		opcode = cqe_status(cqe, cq->cqe_cnt, v->consumer_idx);

		if (opcode == MLX5_CQE_INVALID)
			break;

		if (unlikely(opcode != MLX5_CQE_RESP_SEND)) {
			log_err("got opcode %02X", opcode);
			BUG();
		}

		STAT(RX_HW_DROP) += be32toh(cqe->sop_drop_qpn) >> 24;

		BUG_ON(mlx5_get_cqe_format(cqe) == 0x3); // not compressed
		wqe_idx = be16toh(cqe->wqe_counter) & (wq->wqe_cnt - 1);
		m = v->buffers[wqe_idx];
		mbuf_fill_cqe(m, cqe);
		ms[rx_cnt] = m;
	}

	if (unlikely(!rx_cnt))
		return rx_cnt;

	ACCESS_ONCE(*rxq->shadow_tail) = v->consumer_idx;

	cq->dbrec[0] = htobe32(v->consumer_idx & 0xffffff);
	BUG_ON(mlx5_refill_rxqueue(v, rx_cnt));

	return rx_cnt;
}

#endif

