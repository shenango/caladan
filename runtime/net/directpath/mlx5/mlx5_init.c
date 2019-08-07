/*
 * mlx5_init.c - MLX5 driver for Shenango's network statck
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <base/log.h>
#include <base/mempool.h>

#ifdef DIRECTPATH

#include <util/mmio.h>
#include <util/udma_barrier.h>

#include "../../defs.h"
#include "mlx5.h"
#include "mlx5_ifc.h"

#define PORT_NUM 1 // TODO: make this dynamic

static struct mlx5_txq txqs[NCPU];

struct mlx5_rxq rxqs[NCPU];
struct ibv_context *context;
struct ibv_pd *pd;
struct ibv_mr *mr_tx;
struct ibv_mr *mr_rx;

static void mlx5_init_tx_segment(struct mlx5_txq *v, unsigned int idx)
{
	int size;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_eth_seg *eseg;
	struct mlx5_wqe_data_seg *dpseg;
	void *segment;

	segment = v->tx_qp_dv.sq.buf + idx * v->tx_qp_dv.sq.stride;
	ctrl = segment;
	eseg = segment + sizeof(*ctrl);
	dpseg = (void *)eseg + (offsetof(struct mlx5_wqe_eth_seg, inline_hdr) & ~0xf);

	size = (sizeof(*ctrl) / 16) +
	       (offsetof(struct mlx5_wqe_eth_seg, inline_hdr)) / 16 +
	       sizeof(struct mlx5_wqe_data_seg) / 16;

	/* set ctrl segment */
	*(uint32_t *)(segment + 8) = 0;
	ctrl->imm = 0;
	ctrl->fm_ce_se = MLX5_WQE_CTRL_CQ_UPDATE;
	ctrl->qpn_ds = htobe32(size | (v->tx_qp->qp_num << 8));

	/* set eseg */
	memset(eseg, 0, sizeof(struct mlx5_wqe_eth_seg));
	eseg->cs_flags |= MLX5_ETH_WQE_L3_CSUM | MLX5_ETH_WQE_L4_CSUM;

	/* set dpseg */
	dpseg->lkey = htobe32(mr_tx->lkey);
}

/*
 * simple_alloc - simple memory allocator for internal MLX5 structures
 */
static void *simple_alloc(size_t size, void *priv_data)
{
	return iok_shm_alloc(size, PGSIZE_4KB, NULL);
}

static void simple_free(void *ptr, void *priv_data) {}

static struct mlx5dv_ctx_allocators dv_allocators = {
	.alloc = simple_alloc,
	.free = simple_free,
};

static int mlx5_create_rxq(int index, struct mlx5_rxq *v)
{
	int i, ret;
	unsigned char *buf;

	/* Create a CQ */
	struct ibv_cq_init_attr_ex cq_attr = {
		.cqe = RQ_NUM_DESC,
		.channel = NULL,
		.comp_vector = 0,
		.wc_flags = IBV_WC_EX_WITH_BYTE_LEN,
		.comp_mask = IBV_CQ_INIT_ATTR_MASK_FLAGS,
		.flags = IBV_CREATE_CQ_ATTR_SINGLE_THREADED,
	};
	struct mlx5dv_cq_init_attr dv_cq_attr = {
		.comp_mask = 0,
	};
	v->rx_cq = mlx5dv_create_cq(context, &cq_attr, &dv_cq_attr);
	if (!v->rx_cq)
		return -errno;

	/* Create the work queue for RX */
	struct ibv_wq_init_attr wq_init_attr = {
		.wq_type = IBV_WQT_RQ,
		.max_wr = RQ_NUM_DESC,
		.max_sge = 1,
		.pd = pd,
		.cq = ibv_cq_ex_to_cq(v->rx_cq),
		.comp_mask = 0,
		.create_flags = 0,
	};
	struct mlx5dv_wq_init_attr dv_wq_attr = {
		.comp_mask = 0,
	};
	v->rx_wq = mlx5dv_create_wq(context, &wq_init_attr, &dv_wq_attr);
	if (!v->rx_wq)
		return -errno;

	if (wq_init_attr.max_wr != RQ_NUM_DESC)
		log_warn("Ring size is larger than anticipated");

	/* Set the WQ state to ready */
	struct ibv_wq_attr wq_attr = {0};
	wq_attr.attr_mask = IBV_WQ_ATTR_STATE;
	wq_attr.wq_state = IBV_WQS_RDY;
	ret = ibv_modify_wq(v->rx_wq, &wq_attr);
	if (ret)
		return -ret;

	struct ibv_wq *ind_tbl[1] = {v->rx_wq};
	struct ibv_rwq_ind_table_init_attr rwq_attr = {0};
	rwq_attr.ind_tbl = ind_tbl;
	v->rwq_ind_table = ibv_create_rwq_ind_table(context, &rwq_attr);
	if (!v->rwq_ind_table)
		return -errno;

	static unsigned char rss_key[40];
	struct ibv_rx_hash_conf rss_cnf = {
		.rx_hash_function = IBV_RX_HASH_FUNC_TOEPLITZ,
		.rx_hash_key_len = ARRAY_SIZE(rss_key),
		.rx_hash_key = rss_key,
		.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 | IBV_RX_HASH_SRC_PORT_TCP | IBV_RX_HASH_DST_PORT_TCP,
	};

	struct ibv_qp_init_attr_ex qp_ex_attr = {
		.qp_type = IBV_QPT_RAW_PACKET,
		.comp_mask = IBV_QP_INIT_ATTR_RX_HASH | IBV_QP_INIT_ATTR_IND_TABLE | IBV_QP_INIT_ATTR_PD,
		.pd = pd,
		.rwq_ind_tbl = v->rwq_ind_table,
		.rx_hash_conf = rss_cnf,
	};

	v->qp = ibv_create_qp_ex(context, &qp_ex_attr);
	if (!v->qp)
		return -errno;

	/* expose direct verbs objects */
	struct mlx5dv_obj obj = {
		.cq = {
			.in = ibv_cq_ex_to_cq(v->rx_cq),
			.out = &v->rx_cq_dv,
		},
		.rwq = {
			.in = v->rx_wq,
			.out = &v->rx_wq_dv,
		},
	};
	ret = mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ | MLX5DV_OBJ_RWQ);
	if (ret)
		return -ret;

	BUG_ON(!is_power_of_two(v->rx_wq_dv.stride));
	BUG_ON(!is_power_of_two(v->rx_cq_dv.cqe_size));
	v->rx_wq_log_stride = __builtin_ctz(v->rx_wq_dv.stride);
	v->rx_cq_log_stride = __builtin_ctz(v->rx_cq_dv.cqe_size);

	/* allocate list of posted buffers */
	v->buffers = aligned_alloc(CACHE_LINE_SIZE, v->rx_wq_dv.wqe_cnt * sizeof(void *));
	if (!v->buffers)
		return -ENOMEM;

	v->rxq.consumer_idx = &v->consumer_idx;
	v->rxq.descriptor_table = v->rx_cq_dv.buf;
	v->rxq.nr_descriptors = v->rx_cq_dv.cqe_cnt;
	v->rxq.descriptor_log_size = __builtin_ctz(sizeof(struct mlx5_cqe64));
	v->rxq.parity_byte_offset = offsetof(struct mlx5_cqe64, op_own);
	v->rxq.parity_bit_mask = MLX5_CQE_OWNER_MASK;

	/* set byte_count and lkey for all descriptors once */
	struct mlx5dv_rwq *wq = &v->rx_wq_dv;
	for (i = 0; i < wq->wqe_cnt; i++) {
		struct mlx5_wqe_data_seg *seg = wq->buf + i * wq->stride;
		seg->byte_count =  htobe32(MBUF_DEFAULT_LEN - RX_BUF_RESERVED);
		seg->lkey = htobe32(mr_rx->lkey);

		/* fill queue with buffers */
		buf = mempool_alloc(&directpath_buf_mp);
		if (!buf)
			return -ENOMEM;

		seg->addr = htobe64((unsigned long)buf + RX_BUF_RESERVED);
		v->buffers[i] = buf;
		v->wq_head++;
	}

	/* set ownership of cqes to "hardware" */
	struct mlx5dv_cq *cq = &v->rx_cq_dv;
	for (i = 0; i < cq->cqe_cnt; i++) {
		struct mlx5_cqe64 *cqe = cq->buf + i * cq->cqe_size;
		mlx5dv_set_cqe_owner(cqe, 1);
	}

	udma_to_device_barrier();
	wq->dbrec[0] = htobe32(v->wq_head & 0xffff);

	return 0;
}

static int mlx5_init_txq(int index, struct mlx5_txq *v)
{
	int i, ret;

	/* Create a CQ */
	struct ibv_cq_init_attr_ex cq_attr = {
		.cqe = SQ_NUM_DESC,
		.channel = NULL,
		.comp_vector = 0,
		.wc_flags = 0,
		.comp_mask = IBV_CQ_INIT_ATTR_MASK_FLAGS,
		.flags = IBV_CREATE_CQ_ATTR_SINGLE_THREADED,
	};
	struct mlx5dv_cq_init_attr dv_cq_attr = {
		.comp_mask = 0,
	};
	v->tx_cq = mlx5dv_create_cq(context, &cq_attr, &dv_cq_attr);
	if (!v->tx_cq)
		return -errno;

	/* Create a 1-sided queue pair for sending packets */
	struct ibv_qp_init_attr_ex qp_init_attr = {
		.send_cq = ibv_cq_ex_to_cq(v->tx_cq),
		.recv_cq = ibv_cq_ex_to_cq(v->tx_cq),
		.cap = {
			.max_send_wr = SQ_NUM_DESC,
			.max_recv_wr = 0,
			.max_send_sge = 1,
			.max_inline_data = 0, // TODO: should inline some data?
		},
		.qp_type = IBV_QPT_RAW_PACKET,
		.sq_sig_all = 1,
		.pd = pd,
		.comp_mask = IBV_QP_INIT_ATTR_PD
	};
	struct mlx5dv_qp_init_attr dv_qp_attr = {
		.comp_mask = 0,
	};
	v->tx_qp = mlx5dv_create_qp(context, &qp_init_attr, &dv_qp_attr);
	if (!v->tx_qp)
		return -errno;

	/* Turn on TX QP in 3 steps */
	struct ibv_qp_attr qp_attr;
	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_INIT;
	qp_attr.port_num = 1;
	ret = ibv_modify_qp(v->tx_qp, &qp_attr, IBV_QP_STATE | IBV_QP_PORT);
	if (ret)
		return -ret;

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTR;
	ret = ibv_modify_qp(v->tx_qp, &qp_attr, IBV_QP_STATE);
	if (ret)
		return -ret;

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTS;
	ret = ibv_modify_qp(v->tx_qp, &qp_attr, IBV_QP_STATE);
	if (ret)
		return -ret;

	struct mlx5dv_obj obj = {
		.cq = {
			.in = ibv_cq_ex_to_cq(v->tx_cq),
			.out = &v->tx_cq_dv,
		},
		.qp = {
			.in = v->tx_qp,
			.out = &v->tx_qp_dv,
		},
	};
	ret = mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ | MLX5DV_OBJ_QP);
	if (ret)
		return -ret;

	BUG_ON(!is_power_of_two(v->tx_cq_dv.cqe_size));
	BUG_ON(!is_power_of_two(v->tx_qp_dv.sq.stride));
	v->tx_sq_log_stride = __builtin_ctz(v->tx_qp_dv.sq.stride);
	v->tx_cq_log_stride = __builtin_ctz(v->tx_cq_dv.cqe_size);

	/* allocate list of posted buffers */
	v->buffers = aligned_alloc(CACHE_LINE_SIZE, v->tx_qp_dv.sq.wqe_cnt * sizeof(*v->buffers));
	if (!v->buffers)
		return -ENOMEM;

	for (i = 0; i < v->tx_qp_dv.sq.wqe_cnt; i++)
		mlx5_init_tx_segment(v, i);

	return 0;
}

static struct net_driver_ops mlx5_net_ops = {
	.rx_batch = mlx5_gather_rx,
	.tx_single = mlx5_transmit_one,
	.steer_flows = mlx5_steer_flows,
	.register_flow = mlx5_register_flow,
	.deregister_flow = mlx5_deregister_flow,
	.get_flow_affinity = mlx5_get_flow_affinity,
};

/*
 * mlx5_init - intialize all TX/RX queues
 */
int mlx5_init(struct hardware_q **rxq_out, struct direct_txq **txq_out,
	             unsigned int nr_rxq, unsigned int nr_txq)
{
	int i, ret;

	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	struct mlx5dv_context_attr attr = {0};

	if (nr_rxq > NCPU)
		return -EINVAL;

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return -1;
	}

	i = 0;
	while ((ib_dev = dev_list[i])) {
		if (strncmp(ibv_get_device_name(ib_dev), "mlx5", 4) == 0)
			break;
		i++;
	}

	if (!ib_dev) {
		log_err("mlx5_init: IB device not found");
		return -1;
	}

	attr.flags = MLX5DV_CONTEXT_FLAGS_DEVX;
	context = mlx5dv_open_device(ib_dev, &attr);
	if (!context) {
		log_err("mlx5_init: Couldn't get context for %s (errno %d)",
			ibv_get_device_name(ib_dev), errno);
		return -1;
	}

	ibv_free_device_list(dev_list);

	ret = mlx5dv_set_context_attr(context,
		  MLX5DV_CTX_ATTR_BUF_ALLOCATORS, &dv_allocators);
	if (ret) {
		log_err("mlx5_init: error setting memory allocator");
		return -1;
	}

	pd = ibv_alloc_pd(context);
	if (!pd) {
		log_err("mlx5_init: Couldn't allocate PD");
		return -1;
	}

	/* Register memory for TX buffers */
	mr_tx = ibv_reg_mr(pd, net_tx_buf_mp.buf, net_tx_buf_mp.len, IBV_ACCESS_LOCAL_WRITE);
	if (!mr_tx) {
		log_err("mlx5_init: Couldn't register mr");
		return -1;
	}

	mr_rx = ibv_reg_mr(pd, directpath_buf_mp.buf, directpath_buf_mp.len, IBV_ACCESS_LOCAL_WRITE);
	if (!mr_rx) {
		log_err("mlx5_init: Couldn't register mr");
		return -1;
	}

	for (i = 0; i < nr_rxq; i++) {
		ret = mlx5_create_rxq(i, &rxqs[i]);
		if (ret)
			return ret;

		rxq_out[i] = &rxqs[i].rxq;
	}

	ret = mlx5_init_flows(nr_rxq);
	if (ret)
		return ret;

	for (i = 0; i < nr_txq; i++) {
		ret = mlx5_init_txq(i, &txqs[i]);
		if (ret)
			return ret;

		txq_out[i] = &txqs[i].txq;
	}

	net_ops = mlx5_net_ops;

	return 0;
}

#endif
