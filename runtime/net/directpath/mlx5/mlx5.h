/*
 * mlx5.h - mlx5 driver for Shenango's network stack
 */

#pragma once

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>
#include <base/byteorder.h>

#include "../defs.h"

#define PORT_NUM 1 // TODO: make this dynamic
#define MLX5_ETH_L2_INLINE_HEADER_SIZE 18
#define MLX5_BF_SIZE 256

struct mlx5_cq {
	struct mlx5_cqe64 *cqes;
	uint32_t *dbr;
	uint32_t cnt;
	uint32_t head;
};

struct mlx5_wq {
	void *buf;
	void **buffers; // array of posted buffers
	uint32_t *dbr;
	uint32_t head;
	uint32_t cnt;
	uint32_t log_stride;
};

struct mlx5_rxq {
	/* queue steering mode */
	struct rcu_hlist_head head;
	struct rcu_hlist_node link;
	thread_t *poll_th;

	/* completion queue */
	struct mlx5_cq cq;
	uint16_t strides_consumed;
	uint32_t wq_tail;

	/* work queue */
	struct mlx5_wq wq;
	uint32_t *shadow_tail;
} __aligned(CACHE_LINE_SIZE);

BUILD_ASSERT(offsetof(struct mlx5_rxq, wq) <= CACHE_LINE_SIZE);

struct mlx5_txq {
	/* work queue */
	struct mlx5_wq wq;
	uint32_t bf_offset;
	uint32_t bf_size;
	void *bf_reg;

	/* direct verbs cq */
	struct mlx5_cq cq;

	struct ibv_cq_ex *tx_cq;
	struct ibv_qp *tx_qp;
} __aligned(CACHE_LINE_SIZE);

extern struct mlx5_txq txqs[NCPU];
extern struct mlx5_rxq rxqs[NCPU];

extern struct ibv_qp *rx_qps[NCPU];
extern struct ibv_context *context;

struct mlx5_rmp {
	struct mlx5_wq wq;
	uint64_t rmp_head;
	uint64_t rmp_tail;
	spinlock_t lock;
};

extern struct mlx5_rmp rmp;
extern off_t rx_mr_offset;
extern off_t tx_mr_offset;

// Main RX/TX routines
extern int mlx5_transmit_one(struct mbuf *m);
extern int mlx5_gather_rx(struct mlx5_rxq *rxq, struct mbuf **ms, unsigned int budget);
extern int mlx5_gather_rx_strided(struct mlx5_rxq *v, struct mbuf **ms, unsigned int budget);
extern bool mlx5_rx_poll(unsigned int q_index);
extern bool mlx5_rx_poll_locked(unsigned int q_index);


// Top level initializaiton functions
extern int mlx5_verbs_init_context(bool uses_qsteering);
extern int mlx5_verbs_init(bool uses_qsteering);
extern int mlx5_init_flow_steering(void);
extern int mlx5_init_queue_steering(void);
extern int mlx5_rx_stride_init(void);
extern int mlx5_rx_stride_init_thread(void);
extern int mlx5_rx_stride_init_bufs(void);

// Lower level intialization functions
extern int mlx5_init_cq(struct mlx5_cq *cq, struct mlx5_cqe64 *cqes,
	                    uint32_t cqe_cnt, uint32_t *cqe_dbr);
extern int mlx5_init_rxq_wq(struct mlx5_wq *wq, void *buf, uint32_t *dbr,
	                        uint32_t size, uint32_t stride, uint32_t lkey);
extern int mlx5_init_rxq_wq_stride(struct mlx5_wq *wq, void *seg_buf, uint32_t *dbr,
	                        uint64_t size, uint32_t stride, uint32_t lkey);
extern int mlx5_init_txq_wq(struct mlx5_txq *v, void *buf, uint32_t *dbr,
	                        uint32_t size, uint32_t stride, uint32_t lkey,
	                        uint32_t sqn, void *bf_reg, uint32_t bf_size);

static inline unsigned int nr_inflight_tx(struct mlx5_txq *v)
{
	return v->wq.head - v->cq.head;
}

/*
 * cqe_status - retrieves status of completion queue element
 * @cqe: pointer to element
 * @cqe_cnt: total number of elements
 * @idx: index as stored in head pointer
 *
 * returns CQE status enum (MLX5_CQE_INVALID is -1)
 */
static inline uint8_t cqe_status(struct mlx5_cqe64 *cqe, uint32_t cqe_cnt, uint32_t head)
{
	uint64_t parity = head & cqe_cnt;
	uint8_t op_own = ACCESS_ONCE(cqe->op_own);
	uint8_t op_owner = op_own & MLX5_CQE_OWNER_MASK;
	uint8_t op_code = (op_own & 0xf0) >> 4;

	return ((op_owner == !parity) * MLX5_CQE_INVALID) | op_code;
}

static inline bool mlx5_rxq_pending(struct mlx5_rxq *v)
{
	struct mlx5_cqe64 *cqe;
	cqe = &v->cq.cqes[v->cq.head & (v->cq.cnt - 1)];

	return cqe_status(cqe, v->cq.cnt, v->cq.head) != MLX5_CQE_INVALID;
}

static inline int mlx5_csum_ok(struct mlx5_cqe64 *cqe)
{
	return ((cqe->hds_ip_ext & (MLX5_CQE_L4_OK | MLX5_CQE_L3_OK)) ==
		 (MLX5_CQE_L4_OK | MLX5_CQE_L3_OK)) &
		(((cqe->l4_hdr_type_etc >> 2) & 0x3) == MLX5_CQE_L3_HDR_TYPE_IPV4);
}

static inline int mlx5_get_cqe_opcode(struct mlx5_cqe64 *cqe)
{
	return (cqe->op_own & 0xf0) >> 4;
}

static inline int mlx5_get_cqe_format(struct mlx5_cqe64 *cqe)
{
	return (cqe->op_own & 0xc) >> 2;
}

static inline uint32_t mlx5_get_rss_result(struct mlx5_cqe64 *cqe)
{
	return ntoh32(*((uint32_t *)cqe + 3));
}
