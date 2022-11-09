/*
 * mlx5.h - Verbs driver for Shenango's network statck
 */

#pragma once

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>
#include <base/byteorder.h>

#include "../defs.h"

#define PORT_NUM 1 // TODO: make this dynamic
#define MLX5_ETH_L2_INLINE_HEADER_SIZE 18

struct mlx5_rxq {
	/* handle for runtime */
	struct hardware_q rxq;

	/* queue steering mode */
	struct rcu_hlist_head head;
	struct rcu_hlist_node link;
	struct rcu_hlist_node *last_node;
	spinlock_t lock;

	uint32_t consumer_idx;

	struct mlx5dv_cq rx_cq_dv;
	struct mlx5dv_rwq rx_wq_dv;
	uint32_t wq_head;
	uint32_t rx_cq_log_stride;
	uint32_t rx_wq_log_stride;

	void **buffers; // array of posted buffers


	struct ibv_cq_ex *rx_cq;
	struct ibv_wq *rx_wq;
	struct ibv_rwq_ind_table *rwq_ind_table;
	struct ibv_qp *qp;

} __aligned(CACHE_LINE_SIZE);

struct mlx5_txq {
	/* handle for runtime */
	struct direct_txq txq;

	/* direct verbs qp */
	struct mbuf **buffers; // pending DMA
	struct mlx5dv_qp tx_qp_dv;
	uint32_t sq_head;
	uint32_t tx_sq_log_stride;

	/* direct verbs cq */
	struct mlx5dv_cq tx_cq_dv;
	uint32_t cq_head;
	uint32_t tx_cq_log_stride;

	struct ibv_cq_ex *tx_cq;
	struct ibv_qp *tx_qp;


} __aligned(CACHE_LINE_SIZE);

extern struct mlx5_rxq rxqs[NCPU];
extern struct ibv_context *context;
extern struct ibv_device_attr_ex device_attr;
extern struct ibv_pd *pd;

extern int mlx5_transmit_one(struct mbuf *m);
extern int mlx5_gather_rx(struct hardware_q *rxq, struct mbuf **ms, unsigned int budget);
extern int mlx5_common_init(struct hardware_q **rxq_out, struct direct_txq **txq_out,
			unsigned int nr_rxq, unsigned int nr_txq, bool use_rss);


static inline unsigned int nr_inflight_tx(struct mlx5_txq *v)
{
	return v->sq_head - v->cq_head;
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
	uint16_t parity = head & cqe_cnt;
	uint8_t op_own = ACCESS_ONCE(cqe->op_own);
	uint8_t op_owner = op_own & MLX5_CQE_OWNER_MASK;
	uint8_t op_code = (op_own & 0xf0) >> 4;

	return ((op_owner == !parity) * MLX5_CQE_INVALID) | op_code;
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
