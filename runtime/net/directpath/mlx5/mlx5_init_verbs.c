/*
 * mlx5_init_verbs.c - create mlx5 queue pairs using libibverbs
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <base/log.h>

#ifdef DIRECTPATH

#include <util/mmio.h>
#include <util/udma_barrier.h>

#include "mlx5.h"
#include "mlx5_ifc.h"

struct ibv_qp *rx_qps[NCPU];
struct ibv_context *context;

static struct ibv_pd *pd;
static struct ibv_device_attr_ex device_attr;
static struct ibv_wq *rx_wqs[NCPU];
static struct ibv_mr *mr;
static uint32_t rwq_tbl_size;

/* borrowed from DPDK */
static int ibv_device_to_pci_addr(const struct ibv_device *device,
			                      struct pci_addr *pci_addr)
{
	FILE *file;
	char line[32];
	char path[strlen(device->ibdev_path) + strlen("/device/uevent") + 1];
	snprintf(path, sizeof(path), "%s/device/uevent", device->ibdev_path);

	file = fopen(path, "rb");
	if (!file)
		return -errno;

	while (fgets(line, sizeof(line), file) == line) {
		size_t len = strlen(line);
		int ret;

		/* Truncate long lines. */
		if (len == (sizeof(line) - 1))
			while (line[(len - 1)] != '\n') {
				ret = fgetc(file);
				if (ret == EOF)
					break;
				line[(len - 1)] = ret;
			}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
		/* Extract information. */
		if (sscanf(line,
			   "PCI_SLOT_NAME="
			   "%04hx:%02hhx:%02hhx.%hhd\n",
			   &pci_addr->domain,
			   &pci_addr->bus,
			   &pci_addr->slot,
			   &pci_addr->func) == 4) {
			break;
		}
#pragma GCC diagnostic pop
	}
	fclose(file);
	return 0;
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

static unsigned char rss_key[40] = {
	0x82, 0x19, 0xFA, 0x80, 0xA4, 0x31, 0x06, 0x59, 0x3E, 0x3F, 0x9A,
	0xAC, 0x3D, 0xAE, 0xD6, 0xD9, 0xF5, 0xFC, 0x0C, 0x63, 0x94, 0xBF,
	0x8F, 0xDE, 0xD2, 0xC5, 0xE2, 0x04, 0xB1, 0xCF, 0xB1, 0xB1, 0xA1,
	0x0D, 0x6D, 0x86, 0xBA, 0x61, 0x78, 0xEB};

static uint32_t mlx5_rss_flow_affinity(uint8_t ipproto, uint16_t local_port,
	                                   struct netaddr remote)
{
	uint32_t i, j, map, ret = 0, input_tuple[] = {
		remote.ip, netcfg.addr, local_port | remote.port << 16
	};

	for (j = 0; j < ARRAY_SIZE(input_tuple); j++) {
		for (map = input_tuple[j]; map; map &= (map - 1)) {
			i = (uint32_t)__builtin_ctz(map);
			ret ^= hton32(((const uint32_t *)rss_key)[j]) << (31 - i) |
			       (uint32_t)((uint64_t)(hton32(((const uint32_t *)rss_key)[j + 1])) >>
			       (i + 1));
		}
	}

	return (ret % rwq_tbl_size) % (uint32_t)maxks;
}

static int mlx5_verbs_setup_rss(void)
{
	int i;
	struct ibv_wq *ind_tbl[2048];
	struct ibv_flow *tcp_flow, *other_flow;
	struct ibv_qp *tcp_qp, *other_qp;
	struct ibv_rwq_ind_table *rwq_ind_table;

	rwq_tbl_size = device_attr.rss_caps.max_rwq_indirection_table_size;
	rwq_tbl_size = MIN(2048, rwq_tbl_size);
	if (rwq_tbl_size < maxks)
		log_warn("mlx5_qs: max rwq table size is %u", rwq_tbl_size);

	for (i = 0; i < rwq_tbl_size; i++)
		ind_tbl[i] = rx_wqs[i % maxks];

	/* Create Receive Work Queue Indirection Table */
	struct ibv_rwq_ind_table_init_attr rwq_attr = {
		.log_ind_tbl_size = __builtin_ctz(rwq_tbl_size),
		.ind_tbl = ind_tbl,
		.comp_mask = 0,
	};
	rwq_ind_table = ibv_create_rwq_ind_table(context, &rwq_attr);

	if (!rwq_ind_table)
		return -errno;

	/* Create the main RX QP using the indirection table */
	struct ibv_rx_hash_conf rss_cnf = {
		.rx_hash_function = IBV_RX_HASH_FUNC_TOEPLITZ,
		.rx_hash_key_len = ARRAY_SIZE(rss_key),
		.rx_hash_key = rss_key,
		.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 |
		                       IBV_RX_HASH_SRC_PORT_TCP |
		                       IBV_RX_HASH_DST_PORT_TCP,
	};

	struct ibv_qp_init_attr_ex qp_ex_attr = {
		.qp_type = IBV_QPT_RAW_PACKET,
		.comp_mask =  IBV_QP_INIT_ATTR_IND_TABLE | IBV_QP_INIT_ATTR_RX_HASH |
		              IBV_QP_INIT_ATTR_PD,
		.pd = pd,
		.rwq_ind_tbl = rwq_ind_table,
		.rx_hash_conf = rss_cnf,
	};

	tcp_qp = ibv_create_qp_ex(context, &qp_ex_attr);
	if (!tcp_qp)
		return -errno;

	rss_cnf.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 |
	                              IBV_RX_HASH_SRC_PORT_UDP |
	                              IBV_RX_HASH_DST_PORT_UDP,
	qp_ex_attr.rx_hash_conf = rss_cnf;
	other_qp = ibv_create_qp_ex(context, &qp_ex_attr);
	if (!other_qp)
		return -errno;

	/* Route TCP packets for our MAC address to the QP with TCP RSS configuration */
	struct raw_eth_flow_attr {
		struct ibv_flow_attr attr;
		struct ibv_flow_spec_ipv4 spec_ipv4;
		struct ibv_flow_spec_tcp_udp spec_l4;
	} __attribute__((packed)) flow_attr = {
		.attr = {
			.comp_mask = 0,
			.type = IBV_FLOW_ATTR_NORMAL,
			.size = sizeof(flow_attr),
			.priority = 0,
			.num_of_specs = 2,
			.port = PORT_NUM,
			.flags = 0,
		},
		.spec_ipv4 = {
			.type = IBV_FLOW_SPEC_IPV4,
			.size = sizeof(struct ibv_flow_spec_ipv4),
			.val = {
				.src_ip = 0,
				.dst_ip = hton32(netcfg.addr),
			},
			.mask = {
				.src_ip = 0,
				.dst_ip = 0xffffffff,
			}
		},
		.spec_l4 = {
			.type = IBV_FLOW_SPEC_TCP,
			.size = sizeof(struct ibv_flow_spec_tcp_udp),
			.val = {0},
			.mask = {0},
		},
	};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
	struct ibv_flow_attr *attr = &flow_attr.attr;
#pragma GCC diagnostic pop

	tcp_flow = ibv_create_flow(tcp_qp, attr);
	if (!tcp_flow)
		return -errno;

	/* Route UDP packets to the QP with the UDP RSS configuration */
	flow_attr.spec_l4.type = IBV_FLOW_SPEC_UDP;
	other_flow = ibv_create_flow(other_qp, attr);
	if (!other_flow)
		return -errno;

	net_ops.get_flow_affinity = mlx5_rss_flow_affinity;

	return 0;
}

static int mlx5_create_rxq_verbs(int index, struct mlx5_rxq *v, bool use_rss)
{
	int ret;
	struct mlx5dv_cq rx_cq_dv;
	struct mlx5dv_rwq rx_wq_dv;
	struct ibv_cq_ex *rx_cq;
	struct ibv_wq *rx_wq;
	struct ibv_rwq_ind_table *rwq_ind_tbl;

	uint32_t max_wr = RQ_NUM_DESC;
	uint32_t cqe_cnt = RQ_NUM_DESC;

	if (cfg_directpath_strided) {
		max_wr = DIRECTPATH_STRIDE_RQ_NUM_DESC;
		cqe_cnt = max_wr * DIRECTPATH_NUM_STRIDES;
	}

	/* Create a CQ */
	struct ibv_cq_init_attr_ex cq_attr = {
		.cqe = cqe_cnt,
		.channel = NULL,
		.comp_vector = 0,
		.wc_flags = IBV_WC_EX_WITH_BYTE_LEN,
		.comp_mask = IBV_CQ_INIT_ATTR_MASK_FLAGS,
		.flags = IBV_CREATE_CQ_ATTR_SINGLE_THREADED,
	};
	struct mlx5dv_cq_init_attr dv_cq_attr = {
		.comp_mask = 0,
	};
	rx_cq = mlx5dv_create_cq(context, &cq_attr, &dv_cq_attr);
	if (!rx_cq)
		return -errno;

	/* Create the work queue for RX */
	struct ibv_wq_init_attr wq_init_attr = {
		.wq_type = IBV_WQT_RQ,
		.max_wr = max_wr,
		.max_sge = 1,
		.pd = pd,
		.cq = ibv_cq_ex_to_cq(rx_cq),
		.comp_mask = 0,
		.create_flags = 0,
	};

	struct mlx5dv_wq_init_attr dv_wq_attr = {
		.comp_mask = MLX5DV_WQ_INIT_ATTR_MASK_STRIDING_RQ,
		.striding_rq_attrs.single_stride_log_num_of_bytes = __builtin_ctz(DIRECTPATH_STRIDE_SIZE),
		.striding_rq_attrs.single_wqe_log_num_of_strides = __builtin_ctz(DIRECTPATH_NUM_STRIDES),
		.striding_rq_attrs.two_byte_shift_en = 1,
	};

	if (!cfg_directpath_strided)
		dv_wq_attr.comp_mask = 0;

	rx_wq = mlx5dv_create_wq(context, &wq_init_attr, &dv_wq_attr);
	if (!rx_wq)
		return -errno;

	if (wq_init_attr.max_wr != max_wr)
		log_warn("Ring size is larger than anticipated");

	/* Set the WQ state to ready */
	struct ibv_wq_attr wq_attr = {0};
	wq_attr.attr_mask = IBV_WQ_ATTR_STATE;
	wq_attr.wq_state = IBV_WQS_RDY;
	ret = ibv_modify_wq(rx_wq, &wq_attr);
	if (ret)
		return -ret;

	/* Create 1 QP per WQ if not using RSS */
	if (!use_rss) {
		struct ibv_wq *ind_tbl[1] = {rx_wq};
		struct ibv_rwq_ind_table_init_attr rwq_attr = {0};
		rwq_attr.ind_tbl = ind_tbl;
		rwq_ind_tbl = ibv_create_rwq_ind_table(context, &rwq_attr);
		if (!rwq_ind_tbl)
			return -errno;

		static unsigned char null_rss[40];
		struct ibv_rx_hash_conf rss_cnf = {
				.rx_hash_function = IBV_RX_HASH_FUNC_TOEPLITZ,
				.rx_hash_key_len = ARRAY_SIZE(null_rss),
				.rx_hash_key = null_rss,
				.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 |
				                       IBV_RX_HASH_DST_IPV4 |
				                       IBV_RX_HASH_SRC_PORT_TCP |
				                       IBV_RX_HASH_DST_PORT_TCP,
			};

		struct ibv_qp_init_attr_ex qp_ex_attr = {
			.qp_type = IBV_QPT_RAW_PACKET,
			.comp_mask = IBV_QP_INIT_ATTR_RX_HASH | IBV_QP_INIT_ATTR_IND_TABLE |
			             IBV_QP_INIT_ATTR_PD,
			.pd = pd,
			.rwq_ind_tbl = rwq_ind_tbl,
			.rx_hash_conf = rss_cnf,
		};

		rx_qps[index] = ibv_create_qp_ex(context, &qp_ex_attr);
		if (!rx_qps[index])
			return -errno;
	} else {
		rx_wqs[index] = rx_wq;
	}

	/* expose direct verbs objects */
	struct mlx5dv_obj obj = {
		.cq = {
			.in = ibv_cq_ex_to_cq(rx_cq),
			.out = &rx_cq_dv,
		},
		.rwq = {
			.in = rx_wq,
			.out = &rx_wq_dv,
		},
	};
	ret = mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ | MLX5DV_OBJ_RWQ);
	if (ret)
		return -ret;

	if (unlikely(rx_cq_dv.cqe_size != sizeof(struct mlx5_cqe64)))
		return -EINVAL;

	ret = mlx5_init_rxq_wq(&v->wq, rx_wq_dv.buf, rx_wq_dv.dbrec, rx_wq_dv.wqe_cnt,
		                   rx_wq_dv.stride, mr->lkey);
	if (ret)
		return ret;

	return mlx5_init_cq(&v->cq, rx_cq_dv.buf, rx_cq_dv.cqe_cnt, rx_cq_dv.dbrec);
}

static int mlx5_create_txq_verbs(int index, struct mlx5_txq *v)
{
	int ret;
	struct mlx5dv_qp tx_qp_dv;
	struct mlx5dv_cq tx_cq_dv;

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
			.max_inline_data = MLX5_ETH_L2_INLINE_HEADER_SIZE,
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
	qp_attr.port_num = PORT_NUM;
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
			.out = &tx_cq_dv,
		},
		.qp = {
			.in = v->tx_qp,
			.out = &tx_qp_dv,
		},
	};
	ret = mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ | MLX5DV_OBJ_QP);
	if (ret)
		return -ret;

	if (unlikely(tx_cq_dv.cqe_size != sizeof(struct mlx5_cqe64)))
		return -EINVAL;

	ret = mlx5_init_cq(&v->cq, tx_cq_dv.buf, tx_cq_dv.cqe_cnt, tx_cq_dv.dbrec);
	if (unlikely(ret))
		return ret;

	return mlx5_init_txq_wq(v, tx_qp_dv.sq.buf, tx_qp_dv.dbrec,
		                    tx_qp_dv.sq.wqe_cnt, tx_qp_dv.sq.stride,
		                    mr->lkey, v->tx_qp->qp_num, tx_qp_dv.bf.reg,
		                    tx_qp_dv.bf.size);
}

int mlx5_verbs_init_context(bool uses_qsteering)
{
	int i, ret;

	struct ibv_device **dev_list;
	struct mlx5dv_context_attr attr = {0};
	struct mlx5dv_context query_attrs = {0};
	struct mlx5dv_striding_rq_caps *caps;
	struct pci_addr pci_addr;

	BUG_ON(setenv("MLX5_SINGLE_THREADED", "1", 1));

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return -1;
	}

	for (i = 0; dev_list[i]; i++) {
		if (strncmp(ibv_get_device_name(dev_list[i]), "mlx5", 4))
			continue;

		if (ibv_device_to_pci_addr(dev_list[i], &pci_addr)) {
			log_warn("failed to read pci addr for %s, skipping",
				     ibv_get_device_name(dev_list[i]));
			continue;
		}

		if (memcmp(&pci_addr, &nic_pci_addr, sizeof(pci_addr)) == 0)
			break;
	}

	if (!dev_list[i]) {
		log_err("mlx5_init: IB device not found");
		return -1;
	}

	attr.flags = uses_qsteering ? 0 : MLX5DV_CONTEXT_FLAGS_DEVX;
	context = mlx5dv_open_device(dev_list[i], &attr);
	if (!context) {
		log_err("mlx5_init: Couldn't get context for %s (errno %d)",
			ibv_get_device_name(dev_list[i]), errno);
		ibv_free_device_list(dev_list);
		return -1;
	}

	ibv_free_device_list(dev_list);

	if (cfg_directpath_strided) {
		query_attrs.comp_mask = MLX5DV_CONTEXT_MASK_STRIDING_RQ;
		ret = mlx5dv_query_device(context, &query_attrs);
		if (unlikely(ret)) {
			log_err("mlx5_verbs_init_context: failed to query device");
			return ret;
		}

		caps = &query_attrs.striding_rq_caps;
		if (unlikely(!ibv_is_qpt_supported(caps->supported_qpts,
			                               IBV_QPT_RAW_PACKET))) {
			log_err("mlx5_verbs_init_context: device does not support strq");
			return -EINVAL;
		}

		uint32_t val = __builtin_ctz(DIRECTPATH_STRIDE_SIZE);
		if (val < caps->min_single_stride_log_num_of_bytes ||
			val > caps->max_single_stride_log_num_of_bytes) {
			log_err("mlx5_verbs_init_context: bad stride size");
			return -EINVAL;
		}

		val = __builtin_ctz(DIRECTPATH_NUM_STRIDES);
		if (val < caps->min_single_wqe_log_num_of_strides ||
			val > caps->max_single_wqe_log_num_of_strides) {
			log_err("mlx5_verbs_init_context: bad num strides");
			return -EINVAL;
		}
	}

	return 0;
}


/*
 * mlx5_init - intialize all TX/RX queues
 */
int mlx5_verbs_init(bool uses_qsteering)
{
	int i, ret;

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

	ret = ibv_query_device_ex(context, NULL, &device_attr);
	if (ret) {
		log_err("mlx5_init: failed to query device attributes");
		return -1;
	}

	/* Register memory for all buffers */
	mr = ibv_reg_mr(pd, netcfg.tx_region.base, netcfg.tx_region.len,
		            IBV_ACCESS_LOCAL_WRITE);
	if (!mr) {
		log_err("mlx5_init: Couldn't register mr");
		return -1;
	}

	for (i = 0; i < maxks; i++) {
		ret = mlx5_create_rxq_verbs(i, &rxqs[i], uses_qsteering);
		if (ret)
			return ret;
	}

	if (uses_qsteering) {
		ret = mlx5_verbs_setup_rss();
		if (ret)
			return ret;
	}

	for (i = 0; i < maxks; i++) {
		ret = mlx5_create_txq_verbs(i, &txqs[i]);
		if (ret)
			return ret;
	}

	return 0;
}

#endif
