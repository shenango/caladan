#ifdef DIRECTPATH

#include <util/mmio.h>
#include <util/udma_barrier.h>

#include <base/log.h>
#include "../../defs.h"
#include "mlx5.h"
#include "mlx5_ifc.h"

static unsigned char rss_key[40] = {
	0x82, 0x19, 0xFA, 0x80, 0xA4, 0x31, 0x06, 0x59, 0x3E, 0x3F, 0x9A,
	0xAC, 0x3D, 0xAE, 0xD6, 0xD9, 0xF5, 0xFC, 0x0C, 0x63, 0x94, 0xBF,
	0x8F, 0xDE, 0xD2, 0xC5, 0xE2, 0x04, 0xB1, 0xCF, 0xB1, 0xB1, 0xA1,
	0x0D, 0x6D, 0x86, 0xBA, 0x61, 0x78, 0xEB};

static unsigned int nr_rxq;
static unsigned int queue_assignments[NCPU];
static uint32_t rwq_tbl_size;

static int mlx5_qs_init_flows(void)
{
	int i;
	struct ibv_wq *ind_tbl[2048];
	struct ibv_flow *eth_flow;
	struct ibv_qp *tcp_qp, *other_qp;
	struct ibv_rwq_ind_table *rwq_ind_table;

	rwq_tbl_size = MIN(2048, device_attr.rss_caps.max_rwq_indirection_table_size);
	if (rwq_tbl_size < nr_rxq)
		log_warn("mlx5_qs: max rwq table size is %u", rwq_tbl_size);

	for (i = 0; i < rwq_tbl_size; i++)
		ind_tbl[i] = rxqs[i % nr_rxq].rx_wq;

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
		.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 | IBV_RX_HASH_SRC_PORT_TCP | IBV_RX_HASH_DST_PORT_TCP,
	};

	struct ibv_qp_init_attr_ex qp_ex_attr = {
		.qp_type = IBV_QPT_RAW_PACKET,
		.comp_mask =  IBV_QP_INIT_ATTR_IND_TABLE | IBV_QP_INIT_ATTR_RX_HASH | IBV_QP_INIT_ATTR_PD,
		.pd = pd,
		.rwq_ind_tbl = rwq_ind_table,
		.rx_hash_conf = rss_cnf,
	};

	tcp_qp = ibv_create_qp_ex(context, &qp_ex_attr);
	if (!tcp_qp)
		return -errno;

	rss_cnf.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 | IBV_RX_HASH_SRC_PORT_UDP | IBV_RX_HASH_DST_PORT_UDP,
	qp_ex_attr.rx_hash_conf = rss_cnf;
	other_qp = ibv_create_qp_ex(context, &qp_ex_attr);
	if (!other_qp)
		return -errno;

	/* Route TCP packets for our MAC address to the QP with TCP RSS configuration */
	struct raw_eth_flow_attr {
		struct ibv_flow_attr attr;
		struct ibv_flow_spec_eth spec_eth;
		struct ibv_flow_spec_tcp_udp spec_tcp;
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
		.spec_eth = {
			.type = IBV_FLOW_SPEC_ETH,
			.size = sizeof(struct ibv_flow_spec_eth),
			.val = {
				.src_mac = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
				.ether_type = 0,
				.vlan_tag = 0,
			},
			.mask = {
				.dst_mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
				.src_mac = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
				.ether_type = 0,
				.vlan_tag = 0,
			}
		},
		.spec_tcp = {
			.type = IBV_FLOW_SPEC_TCP,
			.size = sizeof(struct ibv_flow_spec_tcp_udp),
			.val = {0},
			.mask = {0},
		},
	};
	memcpy(&flow_attr.spec_eth.val.dst_mac, netcfg.mac.addr, 6);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
	eth_flow = ibv_create_flow(tcp_qp, &flow_attr.attr);
	if (!eth_flow)
		return -errno;

	/* Route other unicast packets to the QP with the UDP RSS configuration */
	flow_attr.attr.num_of_specs = 1;
	eth_flow = ibv_create_flow(other_qp, &flow_attr.attr);
	if (!eth_flow)
		return -errno;

	/* Route broadcst packets to our set of RX work queues */
	memset(&flow_attr.spec_eth.val.dst_mac, 0xff, 6);
	eth_flow = ibv_create_flow(other_qp, &flow_attr.attr);
#pragma GCC diagnostic pop
	if (!eth_flow)
		return -errno;

	/* Route multicast traffic to our RX queues */
	struct ibv_flow_attr mc_attr = {
		.comp_mask = 0,
		.type = IBV_FLOW_ATTR_MC_DEFAULT,
		.size = sizeof(mc_attr),
		.priority = 0,
		.num_of_specs = 0,
		.port = PORT_NUM,
		.flags = 0,
	};
	eth_flow = ibv_create_flow(other_qp, &mc_attr);
	if (!eth_flow)
		return -errno;

	return 0;
}

static uint32_t mlx5_qs_flow_affinity(uint8_t ipproto, uint16_t local_port, struct netaddr remote)
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

static inline void assign_q(unsigned int qidx, unsigned int kidx)
{
	if (queue_assignments[qidx] == kidx)
		return;

	rcu_hlist_del(&rxqs[qidx].link);
	rcu_hlist_add_head(&rxqs[kidx].head, &rxqs[qidx].link);
	ACCESS_ONCE(queue_assignments[qidx]) = kidx;
	ACCESS_ONCE(ks[qidx]->q_ptrs->q_assign_idx) = kidx;
}

static int mlx5_qs_init_qs(void)
{
	int i;

	for (i = 0; i < nr_rxq; i++) {
		rcu_hlist_init_head(&rxqs[i].head);
		spin_lock_init(&rxqs[i].lock);
		rcu_hlist_add_head(&rxqs[0].head, &rxqs[i].link);
	}

	return 0;
}

static int mlx5_qs_steer(unsigned int *new_fg_assignment)
{
	int i;
	for (i = 0; i < nr_rxq; i++)
		assign_q(i, new_fg_assignment[i]);

	return 0;
}

static int mlx5_qs_gather_rx(struct hardware_q *rxq, struct mbuf **ms, unsigned int budget)
{
	struct hardware_q *hq;
	struct mlx5_rxq *mrxq, *hrxq = container_of(rxq, struct mlx5_rxq, rxq);
	struct rcu_hlist_node *node;

	unsigned int pulled = 0;

	preempt_disable();

	node = hrxq->last_node;
	/* start at the beginning if the last pass pulled from all queues */
	if (!hrxq->last_node) {
		node = hrxq->head.head;
	} else {
		mrxq = rcu_hlist_entry(node, struct mlx5_rxq, link);
		/* queue assignment changed, start at the beginning */
		if (ACCESS_ONCE(queue_assignments[mrxq - rxqs]) != hrxq - rxqs) {
			node = hrxq->head.head;
			hrxq->last_node = NULL;
		}
	}

	while (!preempt_cede_needed(myk())) {

		if (!node) {
			/* if we started in the middle, go back to the beginning */
			if (hrxq->last_node) {
				node = hrxq->head.head;
				hrxq->last_node = NULL;
				continue;
			}
			break;
		}

		mrxq = rcu_hlist_entry(node, struct mlx5_rxq, link);
		hq = &mrxq->rxq;

		if (hardware_q_pending(hq) && spin_try_lock(&mrxq->lock)) {
			pulled += mlx5_gather_rx(hq, ms + pulled, budget - pulled);
			spin_unlock(&mrxq->lock);
			if (pulled == budget)
				break;
		}

		node = rcu_dereference_protected(node->next, !preempt_enabled());
	}

	/* stash the last node we polled so we can start here next time */
	hrxq->last_node = node;
	preempt_enable();
	return pulled;
}

static int mlx5_qs_register_flow(unsigned int affininty, struct trans_entry *e, void **handle_out)
{
	return 0;
}

static int mlx5_qs_deregister_flow(struct trans_entry *e, void *handle)
{
	return 0;
}

static int mlx5_qs_have_work(struct hardware_q *rxq)
{
	struct mlx5_rxq *mrxq, *hrxq = container_of(rxq, struct mlx5_rxq, rxq);
	struct rcu_hlist_node *node;

	rcu_hlist_for_each(&hrxq->head, node, !preempt_enabled()) {
		mrxq = rcu_hlist_entry(node, struct mlx5_rxq, link);
		if (hardware_q_pending(&mrxq->rxq))
			return true;
	}

	return false;
}


static struct net_driver_ops mlx5_net_ops_queue_steering = {
	.rx_batch = mlx5_qs_gather_rx,
	.tx_single = mlx5_transmit_one,
	.steer_flows = mlx5_qs_steer,
	.register_flow = mlx5_qs_register_flow,
	.deregister_flow = mlx5_qs_deregister_flow,
	.get_flow_affinity = mlx5_qs_flow_affinity,
	.rxq_has_work = mlx5_qs_have_work,
};

int mlx5_init_queue_steering(struct hardware_q **rxq_out, struct direct_txq **txq_out,
	             unsigned int nrrxq, unsigned int nr_txq)
{
	int ret;

	nr_rxq = nrrxq;

	ret = mlx5_common_init(rxq_out, txq_out, nr_rxq, nr_txq, true);
	if (ret)
		return ret;

	ret = mlx5_qs_init_flows();
	if (ret)
		return ret;

	ret = mlx5_qs_init_qs();
	if (ret)
		return ret;

	net_ops = mlx5_net_ops_queue_steering;

	return 0;
}

#endif
