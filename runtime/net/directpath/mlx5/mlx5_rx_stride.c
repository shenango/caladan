
#ifdef DIRECTPATH

#include <base/log.h>
#include <base/mem.h>
#include <base/mempool.h>
#include <base/slab.h>
#include <runtime/sync.h>

#include <util/mmio.h>
#include <util/udma_barrier.h>

#include "mlx5.h"

#define MLX5_MPRQ_LEN_MASK 0x000ffff
#define MLX5_MPRQ_STRIDE_NUM_MASK 0x3fff0000
#define MLX5_MPRQ_STRIDE_NUM_SHIFT 16
#define MLX5_MPRQ_FILLER_MASK 0x80000000

/* number of total buffers in rx mempool */
static size_t nrbufs;
/* array of ref counters for buffers in rx mempool */
static uint16_t *sw_refs;
static size_t nr_hw_refs;
static uint16_t *hw_refs;
BUILD_ASSERT(DIRECTPATH_NUM_STRIDES <= UINT16_MAX);

/* buffers that are currently in use in sw (and not hw) */
static void **sw_pending_buffers;
static uint64_t sw_pending_head;
static uint64_t sw_pending_tail;

/* slab allocator for mbuf structs */
static struct slab mbuf_slab;
static struct tcache *mbuf_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, mbuf_pt);

static inline bool shared_rmp_enabled(void)
{
	return cfg_directpath_mode == DIRECTPATH_MODE_EXTERNAL;
}

static struct mlx5_wq *get_rx_wq(struct mlx5_rxq *v)
{
	if (shared_rmp_enabled())
		return &rmp.wq;

	return &v->wq;
}

static inline size_t index_of_buf(void *buf)
{
	assert((uintptr_t)buf >= (uintptr_t)iok.rx_buf);
	assert((uintptr_t)buf < (uintptr_t)iok.rx_buf + iok.rx_len);
	return (buf - (void *)iok.rx_buf) / DIRECTPATH_STRIDE_MODE_BUF_SZ;
}

static inline void *headbuf_from_buf(void *buf)
{
	size_t idx = index_of_buf(buf);
	return (void *)iok.rx_buf + idx * DIRECTPATH_STRIDE_MODE_BUF_SZ;
}

static inline void dec_hw_ref(uint32_t wq_idx, uint64_t count)
{
	assert_preempt_disabled();
	hw_refs[get_current_affinity() * nr_hw_refs + wq_idx] += count;
}

static inline void dec_sw_ref(void *buf, uint64_t count)
{
	size_t idx = index_of_buf(buf);
	assert_preempt_disabled();

	if (shared_rmp_enabled())
		sw_refs[get_current_affinity() * nrbufs + idx] += count;
	else if (__sync_sub_and_fetch(&sw_refs[idx], count) == 0)
		tcache_free(perthread_ptr(directpath_buf_pt), headbuf_from_buf(buf));

}

static inline void ref_reset_hw(uint32_t post_idx)
{
	size_t i;

	for (i = 0; i < maxks; i++)
		hw_refs[i * nr_hw_refs + post_idx] = 0;
}

static inline void ref_reset_sw(void *buf)
{
	size_t i, idx = index_of_buf(buf);

	for (i = 0; i < maxks; i++)
		sw_refs[i * nrbufs + idx] = 0;
}

static inline void ref_reset_normp(void *buf, uint32_t post_idx)
{
	size_t idx = index_of_buf(buf);
	ACCESS_ONCE(sw_refs[idx]) = DIRECTPATH_NUM_STRIDES;
}

static inline uint64_t get_sw_ref(void *buf)
{
	size_t i, idx = index_of_buf(buf), count = 0;

	for (i = 0; i < maxks; i++)
		count += sw_refs[i * nrbufs + idx];

	return count;
}

static inline uint64_t get_hw_ref(uint32_t idx)
{
	size_t i, count = 0;

	for (i = 0; i < maxks; i++)
		count += hw_refs[i * nr_hw_refs + idx];

	return count;
}

static inline void mlx5_stride_post_buf(struct mlx5_wq *wq, void *buf, uint32_t idx)
{
	struct mlx5_mprq_wqe *wseg;

	wseg = wq->buf + (idx << wq->log_stride);
	wseg->dseg.addr = htobe64((unsigned long)buf + rx_mr_offset);
	store_release(&wq->buffers[idx], buf);
}

static void directpath_strided_rx_completion(struct mbuf *m)
{
	preempt_disable();
	dec_sw_ref(m->head, m->release_data);
	tcache_free(perthread_ptr(mbuf_pt), m);
	preempt_enable();
}

int mlx5_init_rxq_wq_stride(struct mlx5_wq *wq, void *seg_buf, uint32_t *dbr,
	                        uint64_t size, uint32_t stride, uint32_t lkey)
{
	uint32_t i;
	struct mlx5_mprq_wqe *wseg;
	void *buf;

	/* set byte_count and lkey for all descriptors once */
	for (i = 0; i < size; i++) {
		wseg = seg_buf + i * stride;
		wseg->dseg.byte_count = htobe32(DIRECTPATH_STRIDE_MODE_BUF_SZ);
		wseg->dseg.lkey = htobe32(lkey);

		/* fill queue with buffers */
		buf = mempool_alloc(&directpath_buf_mp);
		if (unlikely(!buf))
			return -ENOMEM;

		if (!shared_rmp_enabled())
			ref_reset_normp(buf, i);

		mlx5_stride_post_buf(wq, buf, i);
	}

	if (shared_rmp_enabled()) {
		rmp.rmp_head = size;
		ACCESS_ONCE(runtime_info->directpath_strides_posted) = size;
	}

	udma_to_device_barrier();
	wq->dbr[0] = htobe32(size & 0xffff);
	wq->head = size;
	return 0;
}

static void mlx5_refill_strided_rxq(struct mlx5_rxq *v, uint32_t nrdesc)
{
	uint32_t index;
	void *buf;

	v->wq_tail += nrdesc;

	assert_preempt_disabled();

	while (wraps_gt(v->wq_tail + v->wq.cnt, v->wq.head)) {
		buf = tcache_alloc(perthread_ptr(directpath_buf_pt));
		if (unlikely(!buf)) {
			log_warn_ratelimited("failed to fully refill rxq");
			break;
		}

		index = v->wq.head++ & (v->wq.cnt - 1);
		ref_reset_normp(buf, index);
		mlx5_stride_post_buf(&v->wq, buf, index);
	}

	udma_to_device_barrier();
	v->wq.dbr[0] = htobe32(v->wq.head & 0xffff);
}


static void mlx5_refill_strided_rxq_rmp(void)
{
	struct kthread *k;
	uint32_t idx;
	uint64_t start_head;
	void *buf;

	if (!spin_try_lock_np(&rmp.lock))
		return;

	k = myk();
	start_head = rmp.rmp_head;

	/* scan RX buffer slots to find ones that hardware is done with */
	while (rmp.rmp_tail < rmp.rmp_head) {
		idx = rmp.rmp_tail & (rmp.wq.cnt - 1);
		buf = rmp.wq.buffers[idx];

		if (get_hw_ref(idx) != DIRECTPATH_NUM_STRIDES)
			break;

		ref_reset_hw(idx);

		/* record completed buffer */
		sw_pending_buffers[sw_pending_head++ & (nrbufs - 1)] = buf;
		rmp.rmp_tail++;
	}

	/* check if any buffers are now free */
	uint64_t incr = 1;
	for (size_t i = sw_pending_tail; i != sw_pending_head; i++) {
		if (unlikely(preempt_cede_needed(k)))
			break;

		buf = sw_pending_buffers[i & (nrbufs - 1)];
		if (!buf) {
			sw_pending_tail += incr;
			continue;
		}

		if (get_sw_ref(buf) != DIRECTPATH_NUM_STRIDES) {
			/* stop incrementing the tail once we've hit a hole */
			incr = 0;
			continue;
		}

		ref_reset_sw(buf);
		mempool_free(&directpath_buf_mp, buf);
		sw_pending_buffers[i & (nrbufs - 1)] = NULL;
		sw_pending_tail += incr;
	}

	/* keep cache footprint low */
	if (sw_pending_head == sw_pending_tail)
		sw_pending_head = sw_pending_tail = 0;

	/* refill any free slots in the RX queue */
	while (rmp.rmp_head - rmp.rmp_tail < rmp.wq.cnt) {
		idx = rmp.rmp_head & (rmp.wq.cnt - 1);
		buf = mempool_alloc(&directpath_buf_mp);
		if (unlikely(!buf)) {
			log_warn_ratelimited("out of rx buffers");
			break;
		}

		mlx5_stride_post_buf(&rmp.wq, buf, idx);
		rmp.rmp_head++;
	}

	/* notify NIC of newly posted buffers */
	if (rmp.rmp_head != start_head) {
		udma_to_device_barrier();
		rmp.wq.dbr[0] = htobe32(rmp.rmp_head & 0xffff);
		ACCESS_ONCE(runtime_info->directpath_strides_posted) = rmp.rmp_head;
	}

	/* if completely out of buffers, try reclaiming some from TCP stack */
	if (unlikely(rmp.rmp_head == rmp.rmp_tail)) {
		static uint64_t last_out_of_bufs;
		if (rmp.rmp_head >= last_out_of_bufs) {
			thread_spawn((thread_fn_t)tcp_free_rx_bufs, NULL);
			/* only try this once per full cycle of RQ buffers */
			last_out_of_bufs = rmp.rmp_head + rmp.wq.cnt;
		}
	}

	spin_unlock_np(&rmp.lock);
}


static __noinline void panic_error_cqe(struct mlx5_cqe64 *cqe, uint8_t opcode)
{
	struct mlx5_err_cqe *ecqe = (struct mlx5_err_cqe *)cqe;
	panic("got opcode %02X syndrome %x", opcode, ecqe->syndrome);
}

static struct mbuf *mbuf_fill_cqe(void *dbuf, struct mlx5_cqe64 *cqe,
	                              uint32_t len, uint64_t num_strides)
{
	struct mbuf *m;

	assert_preempt_disabled();
	m = tcache_alloc(perthread_ptr(mbuf_pt));
	if (unlikely(!m)) {
		log_warn_ratelimited("dropping packet; oom");
		return NULL;
	}

	prefetch(dbuf);

	// NIC pads two 0 bytes for alignment of IP headers etc
	mbuf_init(m, dbuf + 2, len, 0);
	m->len = len;
	m->csum_type = mlx5_csum_ok(cqe);
	m->csum = 0;
	m->rss_hash = mlx5_get_rss_result(cqe);
	m->release = directpath_strided_rx_completion;
	m->release_data = num_strides;

	return m;
}


int mlx5_gather_rx_strided(struct mlx5_rxq *v, struct mbuf **ms,
	                       unsigned int budget)
{
	uint8_t opcode;
	uint16_t wqe_idx, stride_idx, stride_cnt, len;
	uint32_t byte_cnt, start_head = v->cq.head, strides_consumed = 0;
	int rx_cnt = 0;
	void *buf;
	struct kthread *k;
	struct mlx5_cqe64 *cqe;
	struct mlx5_wq *wq = get_rx_wq(v);

	k = getk();

	while (rx_cnt < budget && !preempt_cede_needed(k)) {
		cqe = &v->cq.cqes[v->cq.head & (v->cq.cnt - 1)];
		opcode = cqe_status(cqe, v->cq.cnt, v->cq.head);

		if (opcode == MLX5_CQE_INVALID)
			break;

		if (unlikely(opcode != MLX5_CQE_RESP_SEND))
			panic_error_cqe(cqe, opcode);

		v->cq.head++;
		prefetch(&v->cq.cqes[v->cq.head & (v->cq.cnt - 1)]);

		STAT(RX_HW_DROP) += be32toh(cqe->sop_drop_qpn) >> 24;

		wqe_idx = be16toh(cqe->wqe_id) & (wq->cnt - 1);
		stride_idx = be16toh(cqe->wqe_counter);
		byte_cnt = be32toh(cqe->byte_cnt);
		stride_cnt = (byte_cnt & MLX5_MPRQ_STRIDE_NUM_MASK) >>
				   MLX5_MPRQ_STRIDE_NUM_SHIFT;
		len = byte_cnt & MLX5_MPRQ_LEN_MASK;

		if (shared_rmp_enabled())
			dec_hw_ref(wqe_idx, stride_cnt);
		buf = load_acquire(&wq->buffers[wqe_idx]);
		strides_consumed += stride_cnt;

		if (byte_cnt & MLX5_MPRQ_FILLER_MASK) {
			dec_sw_ref(buf, stride_cnt);
		} else {
			buf += stride_idx * DIRECTPATH_STRIDE_SIZE;
			ms[rx_cnt] = mbuf_fill_cqe(buf, cqe, len, stride_cnt);
			if (unlikely(!ms[rx_cnt])) {
				dec_sw_ref(buf, stride_cnt);
				break;
			}
			rx_cnt++;
		}
	}

	if (start_head != v->cq.head) {
		ACCESS_ONCE(*v->shadow_tail) = v->cq.head;
		v->cq.dbr[0] = htobe32(v->cq.head & 0xffffff);
		k->q_ptrs->directpath_strides_consumed += strides_consumed;
	}

	if (!shared_rmp_enabled()) {
		v->strides_consumed += strides_consumed;
		if (v->strides_consumed >= DIRECTPATH_NUM_STRIDES) {
			mlx5_refill_strided_rxq(v, v->strides_consumed / DIRECTPATH_NUM_STRIDES);
			v->strides_consumed %= DIRECTPATH_NUM_STRIDES;
		}
	}

	putk();

	return rx_cnt;
}

int mlx5_rx_stride_init_bufs(void)
{
	nrbufs = iok.rx_len / DIRECTPATH_STRIDE_MODE_BUF_SZ;
	nrbufs = align_up(nrbufs, CACHE_LINE_SIZE / sizeof(uint16_t));

	if (!is_power_of_two(nrbufs)) {
		log_err("bad directpath buf pool size, want power of two buffer count");
		return -EINVAL;
	}

	sw_refs = calloc(nrbufs * maxks, sizeof(int16_t));
	if (unlikely(!sw_refs))
		return -ENOMEM;

	sw_pending_buffers = calloc(nrbufs, sizeof(void *));
	if (unlikely(!sw_pending_buffers))
		return -ENOMEM;

	if (shared_rmp_enabled()) {
		nr_hw_refs = align_up(DIRECTPATH_STRIDE_RQ_NUM_DESC,
			                  CACHE_LINE_SIZE / sizeof(*hw_refs));
		hw_refs = calloc(nr_hw_refs * maxks, sizeof(*hw_refs));
		if (!hw_refs)
			return -ENOMEM;

		net_ops.trigger_rx_refill = mlx5_refill_strided_rxq_rmp;
	}

	return 0;
}

int mlx5_rx_stride_init_thread(void)
{
	if (!cfg_directpath_strided)
		return 0;

	myk()->q_ptrs->directpath_strides_consumed = 0;

	tcache_init_perthread(mbuf_tcache, &perthread_get(mbuf_pt));
	return 0;
}

int mlx5_rx_stride_init(void)
{
	int ret;

	if (!cfg_directpath_strided)
		return 0;

	ret = slab_create(&mbuf_slab, "mbufs", sizeof(struct mbuf), 0);
	if (ret)
		return ret;

	mbuf_tcache = slab_create_tcache(&mbuf_slab, TCACHE_DEFAULT_MAG_SIZE);
	if (!mbuf_tcache)
		return -ENOMEM;

	if (cfg_directpath_external())
		return 0;

	return mlx5_rx_stride_init_bufs();
}

#endif
