/*
 * mlx5_rxtx.c - interactions with tx and rx queues
 */

#ifdef DIRECTPATH

#include <base/log.h>
#include <base/mempool.h>
#include <runtime/preempt.h>
#include <util/mmio.h>
#include <util/udma_barrier.h>

#include "mlx5.h"

off_t rx_mr_offset;
off_t tx_mr_offset;
struct mlx5_txq txqs[NCPU];
struct mlx5_rxq rxqs[NCPU];

static void mlx5_init_tx_segment(struct mlx5_txq *v, unsigned int idx,
	                             uint32_t lkey, uint32_t sqn)
{
	int size;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_eth_seg *eseg;
	struct mlx5_wqe_data_seg *dpseg;
	void *segment;

	segment = v->wq.buf + (idx << v->wq.log_stride);
	ctrl = segment;
	eseg = segment + sizeof(*ctrl);
	dpseg = (void *)eseg + ((offsetof(struct mlx5_wqe_eth_seg, inline_hdr) +
	                         MLX5_ETH_L2_INLINE_HEADER_SIZE) & ~0xf);

	size = (sizeof(*ctrl) / 16) +
	       ((offsetof(struct mlx5_wqe_eth_seg, inline_hdr)) +
	         MLX5_ETH_L2_INLINE_HEADER_SIZE) / 16 +
	       sizeof(struct mlx5_wqe_data_seg) / 16;

	/* set ctrl segment */
	*(uint32_t *)(segment + 8) = 0;
	ctrl->imm = 0;
	ctrl->fm_ce_se = MLX5_WQE_CTRL_CQ_UPDATE;
	ctrl->qpn_ds = htobe32(size | (sqn << 8));

	/* set eseg */
	memset(eseg, 0, sizeof(struct mlx5_wqe_eth_seg));
	eseg->cs_flags |= MLX5_ETH_WQE_L3_CSUM | MLX5_ETH_WQE_L4_CSUM;
	eseg->inline_hdr_sz = htobe16(MLX5_ETH_L2_INLINE_HEADER_SIZE);

	/* set dpseg */
	dpseg->lkey = htobe32(lkey);
}

static int mlx5_init_wq(struct mlx5_wq *wq, void *buf, uint32_t *dbr,
	                    uint32_t size, uint32_t stride)
{
	if (unlikely(!is_power_of_two(stride) || !is_power_of_two(size)))
		return -EINVAL;

	wq->buf = buf;
	wq->dbr = dbr;
	wq->cnt = size;
	wq->log_stride = __builtin_ctz(stride);
	wq->head = 0;
	wq->buffers = aligned_alloc(CACHE_LINE_SIZE, size * sizeof(void *));
	if (unlikely(!wq->buffers))
		return -ENOMEM;

	return 0;
}

int mlx5_init_cq(struct mlx5_cq *cq, struct mlx5_cqe64 *cqes,
	             uint32_t cqe_cnt, uint32_t *cqe_dbr)
{
	unsigned int i;

	cq->cqes = cqes;
	cq->cnt = cqe_cnt;
	cq->dbr = cqe_dbr;
	cq->head = 0;

	/* set ownership of cqes to "hardware" */
	for (i = 0; i < cqe_cnt; i++)
		mlx5dv_set_cqe_owner(&cqes[i], 1);

	return 0;
}

int mlx5_init_txq_wq(struct mlx5_txq *v, void *buf, uint32_t *dbr,
	                 uint32_t size, uint32_t stride, uint32_t lkey,
	                 uint32_t sqn, void *bf_reg, uint32_t bf_size)
{
	int ret;
	uint32_t i;

	ret = mlx5_init_wq(&v->wq, buf, dbr, size, stride);
	if (unlikely(ret))
		return ret;

	v->bf_reg = bf_reg;
	v->bf_offset = 0;
	v->bf_size = bf_size;

	for (i = 0; i < size; i++)
		mlx5_init_tx_segment(v, i, lkey, sqn);

	return 0;
}

int mlx5_init_rxq_wq(struct mlx5_wq *wq, void *seg_buf, uint32_t *dbr,
	                 uint32_t size, uint32_t stride, uint32_t lkey)
{
	int ret;
	uint32_t i;
	struct mlx5_wqe_data_seg *seg;
	void *buf;

	ret = mlx5_init_wq(wq, seg_buf, dbr, size, stride);
	if (unlikely(ret))
		return ret;

	if (cfg_directpath_strided)
		return mlx5_init_rxq_wq_stride(wq, seg_buf, dbr, size, stride, lkey);

	/* set byte_count and lkey for all descriptors once */
	for (i = 0; i < size; i++) {
		seg = seg_buf + i * stride;
		seg->byte_count = htobe32(net_get_mtu() + RX_BUF_TAIL);
		seg->lkey = htobe32(lkey);

		/* fill queue with buffers */
		buf = mempool_alloc(&directpath_buf_mp);
		if (unlikely(!buf))
			return -ENOMEM;

		seg->addr = htobe64((unsigned long)buf + RX_BUF_HEAD + rx_mr_offset);
		wq->buffers[i] = buf;
	}

	udma_to_device_barrier();
	wq->dbr[0] = htobe32(size & 0xffff);
	wq->head = size;
	return 0;
}


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
	uint32_t index;
	unsigned int i;
	unsigned char *buf;
	struct mlx5_wqe_data_seg *seg;

	assert(wraps_lte(nrdesc + vq->wq.head, vq->cq.head + vq->wq.cnt));

	preempt_disable();

	for (i = 0; i < nrdesc; i++) {
		buf = tcache_alloc(perthread_ptr(directpath_buf_pt));
		if (unlikely(!buf)) {
			preempt_enable();
			return -ENOMEM;
		}

		index = vq->wq.head++ & (vq->wq.cnt - 1);
		seg = vq->wq.buf + (index << vq->wq.log_stride);
		seg->addr = htobe64((unsigned long)buf + RX_BUF_HEAD + rx_mr_offset);
		vq->wq.buffers[index] = buf;
	}

	udma_to_device_barrier();
	vq->wq.dbr[0] = htobe32(vq->wq.head & 0xffff);

	preempt_enable();

	return 0;

}

static __noinline void panic_error_cqe(struct mlx5_cqe64 *cqe, uint8_t opcode)
{
	struct mlx5_err_cqe *ecqe = (struct mlx5_err_cqe *)cqe;
	panic("got opcode %02X syndrome %x", opcode, ecqe->syndrome);
}

/*
 * mlx5_gather_completions - collect up to budget received completions
 */
static int mlx5_gather_completions(struct mbuf **mbufs, struct mlx5_txq *v,
	                               unsigned int budget)
{
	struct mlx5_cqe64 *cqe;
	uint8_t opcode;
	uint16_t wqe_idx;
	unsigned int compl_cnt;

	for (compl_cnt = 0; compl_cnt < budget; compl_cnt++, v->cq.head++) {
		cqe = &v->cq.cqes[v->cq.head & (v->cq.cnt - 1)];
		opcode = cqe_status(cqe, v->cq.cnt, v->cq.head);

		if (opcode == MLX5_CQE_INVALID)
			break;

		if (unlikely(opcode != MLX5_CQE_REQ))
			panic_error_cqe(cqe, opcode);

		wqe_idx = be16toh(cqe->wqe_counter) & (v->wq.cnt - 1);
		mbufs[compl_cnt] = load_acquire(&v->wq.buffers[wqe_idx]);
	}

	v->cq.dbr[0] = htobe32(v->cq.head & 0xffffff);

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
	v = &txqs[k->kthread_idx];
	idx = v->wq.head & (v->wq.cnt - 1);

	if (nr_inflight_tx(v) >= SQ_CLEAN_THRESH) {
		compl = mlx5_gather_completions(mbs, v, SQ_CLEAN_MAX);
		for (i = 0; i < compl; i++)
			mbuf_free(mbs[i]);
		if (unlikely(nr_inflight_tx(v) >= v->wq.cnt)) {
			putk();
			log_warn_ratelimited("txq full");
			return -1;
		}
	}

	segment = v->wq.buf + (idx << v->wq.log_stride);
	ctrl = segment;
	eseg = segment + sizeof(*ctrl);
	dpseg = (void *)eseg + ((offsetof(struct mlx5_wqe_eth_seg, inline_hdr) + MLX5_ETH_L2_INLINE_HEADER_SIZE) & ~0xf);

	ctrl->opmod_idx_opcode = htobe32(((v->wq.head & 0xffff) << 8) |
					       MLX5_OPCODE_SEND);

	assert(mbuf_length(m) >= MLX5_ETH_L2_INLINE_HEADER_SIZE);
	memcpy(eseg->inline_hdr_start, mbuf_data(m), MLX5_ETH_L2_INLINE_HEADER_SIZE);

	dpseg->byte_count = htobe32(mbuf_length(m) - MLX5_ETH_L2_INLINE_HEADER_SIZE);
	dpseg->addr = htobe64((uint64_t)mbuf_data(m) + MLX5_ETH_L2_INLINE_HEADER_SIZE + tx_mr_offset);

	/* record buffer */
	store_release(&v->wq.buffers[v->wq.head & (v->wq.cnt - 1)], m);
	v->wq.head++;

	/* write doorbell record */
	udma_to_device_barrier();
	v->wq.dbr[MLX5_SND_DBR] = htobe32(v->wq.head & 0xffff);

	/* ring bf doorbell */

	mmio_wc_start();
	mmio_write64_be(v->bf_reg + v->bf_offset, *(__be64 *)ctrl);
	mmio_flush_writes();

	v->bf_offset ^= v->bf_size;

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

int mlx5_gather_rx(struct mlx5_rxq *v, struct mbuf **ms, unsigned int budget)
{
	uint8_t opcode;
	uint16_t wqe_idx;
	int rx_cnt;

	struct mlx5_cqe64 *cqe;
	struct mbuf *m;

	for (rx_cnt = 0; rx_cnt < budget; rx_cnt++) {
		cqe = &v->cq.cqes[v->cq.head & (v->cq.cnt - 1)];
		opcode = cqe_status(cqe, v->cq.cnt, v->cq.head);

		if (opcode == MLX5_CQE_INVALID)
			break;

		v->cq.head++;
		prefetch(&v->cq.cqes[v->cq.head & (v->cq.cnt - 1)]);

		if (unlikely(opcode != MLX5_CQE_RESP_SEND))
			panic_error_cqe(cqe, opcode);

		STAT(RX_HW_DROP) += be32toh(cqe->sop_drop_qpn) >> 24;

		wqe_idx = be16toh(cqe->wqe_counter) & (v->wq.cnt - 1);
		m = v->wq.buffers[wqe_idx];
		mbuf_fill_cqe(m, cqe);
		ms[rx_cnt] = m;
	}

	if (unlikely(!rx_cnt))
		return 0;

	ACCESS_ONCE(*v->shadow_tail) = v->cq.head;

	v->cq.dbr[0] = htobe32(v->cq.head & 0xffffff);

	// TODO: handle buffer exhaustion
	BUG_ON(mlx5_refill_rxqueue(v, rx_cnt));

	return rx_cnt;
}

#endif

