#include <base/log.h>
#include <runtime/sync.h>

#ifdef DIRECTPATH

#include "mlx5.h"
#include "mlx5_ifc.h"

#define PORT_MATCH_BITS 10
#define PORT_MASK ((1 << PORT_MATCH_BITS) - 1)

static struct mlx5dv_dr_domain		*dmn;
static unsigned int		nr_rxq;
static DEFINE_SPINLOCK(direct_rule_lock);

struct tbl {
	struct mlx5dv_dr_table		*tbl;
	struct mlx5dv_dr_matcher		*default_egress_match;
	struct mlx5dv_dr_rule		*default_egress_rule;

	/* action that directs packets to this table */
	struct mlx5dv_dr_action		*ingress_action;
};

struct port_matcher_tbl {
	struct tbl		tbl;
	struct mlx5dv_dr_matcher		*match;
	unsigned int		port_no_bits;
	uint8_t ipproto;
	bool use_dst;
	size_t match_bit_off;
	size_t match_bit_sz;
	struct mlx5dv_dr_rule		*rules[];
};

static DEFINE_BITMAP(tcp_listen_ports, 65536);
static DEFINE_BITMAP(udp_listen_ports, 65536);

/* level 0 flow table (root) */
static struct mlx5dv_dr_table		*root_tbl;
static struct mlx5dv_dr_matcher		*match_mac_and_tport;
static struct mlx5dv_dr_matcher		*match_just_mac;
static struct mlx5dv_dr_rule		*root_tcp_rule;
static struct mlx5dv_dr_rule		*root_udp_rule;
static struct mlx5dv_dr_rule		*root_catchall_rule;

/* level 1 flow tables */
static struct tbl		tcp_tbl;
static struct tbl		udp_tbl;

static struct mlx5dv_dr_matcher		*udp_tbl_dport_match;
static struct mlx5dv_dr_matcher		*tcp_tbl_dport_match;

/* level 2 flow tables */
static struct port_matcher_tbl		*tcp_dport_tbl;
static struct port_matcher_tbl		*tcp_sport_tbl;
static struct port_matcher_tbl		*udp_dport_tbl;
static struct port_matcher_tbl		*udp_sport_tbl;

/* last level flow groups */
static struct tbl		fg_tbl[NCPU];
static struct mlx5dv_dr_action		*fg_fwd_action[NCPU];
static unsigned int		fg_qp_assignment[NCPU];

static union match empty_match = {
	.size = sizeof(empty_match.buf)
};

enum dr_matcher_criteria {
	DR_MATCHER_CRITERIA_EMPTY		= 0,
	DR_MATCHER_CRITERIA_OUTER		= 1 << 0,
};

static int mlx5_tbl_init(struct tbl *tbl, int level, struct mlx5dv_dr_action *default_egress)
{
	struct mlx5dv_dr_action *action[1] = {default_egress};

	tbl->tbl = mlx5dv_dr_table_create(dmn, level);
	if (!tbl->tbl)
		return -errno;

	tbl->default_egress_match = mlx5dv_dr_matcher_create(tbl->tbl, 2, DR_MATCHER_CRITERIA_EMPTY, &empty_match.params);
	if (!tbl->default_egress_match)
		return -errno;

	tbl->ingress_action = mlx5dv_dr_action_create_dest_table(tbl->tbl);
	if (!tbl->ingress_action)
		return -errno;

	tbl->default_egress_rule = mlx5dv_dr_rule_create(
			  tbl->default_egress_match, &empty_match.params, 1, action);
	if (!tbl->default_egress_rule)
		return -errno;

	return 0;
}

static struct port_matcher_tbl *alloc_port_matcher(uint8_t ipproto,
		      bool use_dst, unsigned int port_bits)
{
	int i, ret, pos = 0;
	union match mask = {0};
	struct port_matcher_tbl *t;
	struct mlx5dv_dr_action *action[1];
	unsigned int nrules = 1 << port_bits;

	t = calloc(1, sizeof(*t) + nrules * sizeof(struct mlx5dv_dr_rule *));
	if (!t)
		return NULL;

	t->port_no_bits = port_bits;
	t->ipproto = ipproto;
	t->use_dst = use_dst;

	ret = mlx5_tbl_init(&t->tbl, 2, fg_tbl[0].ingress_action);
	if (ret)
		return NULL;

	if (ipproto == IPPROTO_TCP && use_dst) {
		t->match_bit_off = __devx_bit_off(fte_match_param, outer_headers.tcp_dport);
		t->match_bit_sz = __devx_bit_sz(fte_match_param, outer_headers.tcp_dport);
	} else if (ipproto == IPPROTO_TCP && !use_dst) {
		t->match_bit_off = __devx_bit_off(fte_match_param, outer_headers.tcp_sport);
		t->match_bit_sz = __devx_bit_sz(fte_match_param, outer_headers.tcp_sport);
	} else if (ipproto == IPPROTO_UDP && use_dst) {
		t->match_bit_off = __devx_bit_off(fte_match_param, outer_headers.udp_dport);
		t->match_bit_sz = __devx_bit_sz(fte_match_param, outer_headers.udp_dport);
	} else if (ipproto == IPPROTO_UDP && !use_dst) {
		t->match_bit_off = __devx_bit_off(fte_match_param, outer_headers.udp_sport);
		t->match_bit_sz = __devx_bit_sz(fte_match_param, outer_headers.udp_sport);
	} else {
		BUG();
	}

	mask.size = DEVX_ST_SZ_BYTES(fte_match_param);
	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_version, 4);
	_devx_set(mask.buf, __devx_mask(port_bits), t->match_bit_off, t->match_bit_sz);

	t->match = mlx5dv_dr_matcher_create(t->tbl.tbl, 0,
			      DR_MATCHER_CRITERIA_OUTER, &mask.params);
	if (!t->match)
		return NULL;

	for (i = 0; i < nrules; i++) {
		_devx_set(mask.buf, i, t->match_bit_off, t->match_bit_sz);
		action[0] = fg_tbl[pos++ % nr_rxq].ingress_action;
		t->rules[i] = mlx5dv_dr_rule_create(t->match, &mask.params, 1, action);
		if (!t->rules[i])
				return NULL;
	}

	return t;
}


static int mlx5_init_fg_tables(void)
{
	int i, ret;

	for (i = 0; i < nr_rxq; i++) {
		/* forward to qp 0 */
		fg_fwd_action[i] = mlx5dv_dr_action_create_dest_ibv_qp(rxqs[0].qp);
		if (!fg_fwd_action[i])
			return -errno;

		ret = mlx5_tbl_init(&fg_tbl[i], 3, fg_fwd_action[i]);
		if (ret)
			return ret;

		fg_qp_assignment[i] = 0;
	}

	return 0;
}

static int mlx5_init_udp(void)
{
	int ret;
	union match mask = {0};

	udp_dport_tbl = alloc_port_matcher(IPPROTO_UDP, true, PORT_MATCH_BITS);
	if (!udp_dport_tbl)
		return -EINVAL;

	udp_sport_tbl = alloc_port_matcher(IPPROTO_UDP, false, PORT_MATCH_BITS);
	if (!udp_sport_tbl)
		return -EINVAL;

	ret = mlx5_tbl_init(&udp_tbl, 1, udp_dport_tbl->tbl.ingress_action);
	if (ret)
		return ret;

	mask.size = DEVX_ST_SZ_BYTES(fte_match_param);
	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_version, 4);
	DEVX_SET(fte_match_param, mask.buf, outer_headers.udp_dport, __devx_mask(16));

	udp_tbl_dport_match = mlx5dv_dr_matcher_create(udp_tbl.tbl, 0,
		    DR_MATCHER_CRITERIA_OUTER, &mask.params);
	if (!udp_tbl_dport_match)
		return -errno;

	return 0;
}

static int mlx5_init_tcp(void)
{
	int ret;
	union match mask = {0};

	tcp_dport_tbl = alloc_port_matcher(IPPROTO_TCP, true, PORT_MATCH_BITS);
	if (!tcp_dport_tbl)
		return -EINVAL;

	tcp_sport_tbl = alloc_port_matcher(IPPROTO_TCP, false, PORT_MATCH_BITS);
	if (!tcp_sport_tbl)
		return -EINVAL;

	ret = mlx5_tbl_init(&tcp_tbl, 1, tcp_dport_tbl->tbl.ingress_action);
	if (ret)
		return ret;

	mask.size = DEVX_ST_SZ_BYTES(fte_match_param);
	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_version, 4);
	DEVX_SET(fte_match_param, mask.buf, outer_headers.tcp_dport, __devx_mask(16));

	tcp_tbl_dport_match = mlx5dv_dr_matcher_create(tcp_tbl.tbl, 0,
		    DR_MATCHER_CRITERIA_OUTER, &mask.params);
	if (!tcp_tbl_dport_match)
		return -errno;

	return 0;
}

static int mlx5_init_root_table(void)
{
	union match mask = {0};
	struct mlx5dv_dr_action *action[1];

	root_tbl = mlx5dv_dr_table_create(dmn, 0);
	if (!root_tbl)
		return -errno;

	mask.size = DEVX_ST_SZ_BYTES(fte_match_param);

	DEVX_SET(fte_match_param, mask.buf, outer_headers.dmac_47_16, __devx_mask(32));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.dmac_15_0, __devx_mask(16));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_protocol, __devx_mask(8));
	match_mac_and_tport = mlx5dv_dr_matcher_create(root_tbl, 0, DR_MATCHER_CRITERIA_OUTER, &mask.params);
	if (!match_mac_and_tport)
		return -errno;

	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_protocol, 0);
	match_just_mac = mlx5dv_dr_matcher_create(root_tbl, 1, DR_MATCHER_CRITERIA_OUTER, &mask.params);
	if (!match_just_mac)
		return -errno;

	DEVX_SET(fte_match_param, mask.buf, outer_headers.dmac_47_16, hton32(*(uint32_t *)&netcfg.mac.addr[0]));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.dmac_15_0, hton16(*(uint16_t *)&netcfg.mac.addr[4]));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_protocol, IPPROTO_TCP);
	action[0] = tcp_tbl.ingress_action;
	root_tcp_rule = mlx5dv_dr_rule_create(match_mac_and_tport, &mask.params, 1, action);
	if (!root_tcp_rule)
		return -errno;

	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_protocol, IPPROTO_UDP);
	action[0] = udp_tbl.ingress_action;
	root_udp_rule = mlx5dv_dr_rule_create(match_mac_and_tport, &mask.params, 1, action);
	if (!root_udp_rule)
		return -errno;

	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_protocol, 0);
	action[0] = fg_tbl[0].ingress_action;
	root_catchall_rule = mlx5dv_dr_rule_create(match_just_mac, &mask.params, 1, action);
	if (!root_catchall_rule)
		return -errno;

	return 0;
}

static int mlx5_register_flow(unsigned int affinity, struct trans_entry *e, void **handle_out)
{
	union match key = {0};

	struct port_matcher_tbl *dst_tbl;
	bitmap_ptr_t map;
	struct mlx5dv_dr_matcher *match;
	struct mlx5dv_dr_action *action[1];
	void *rule;

	if (e->match != TRANS_MATCH_3TUPLE)
		return -EINVAL;

	key.size = DEVX_ST_SZ_BYTES(fte_match_param);
	DEVX_SET(fte_match_param, key.buf, outer_headers.ip_version, 4);

	switch (e->proto) {
		case IPPROTO_TCP:
			map = tcp_listen_ports;
			match = tcp_tbl_dport_match;
			dst_tbl = tcp_sport_tbl;
			DEVX_SET(fte_match_param, key.buf, outer_headers.tcp_dport, e->laddr.port);
			break;
		case IPPROTO_UDP:
			map = udp_listen_ports;
			match = udp_tbl_dport_match;
			dst_tbl = udp_sport_tbl;
			DEVX_SET(fte_match_param, key.buf, outer_headers.udp_dport, e->laddr.port);
			break;
		default:
			return -EINVAL;
	}

	if (bitmap_atomic_test_and_set(map, e->laddr.port))
		return -EINVAL;

	action[0] = dst_tbl->tbl.ingress_action;

	spin_lock_np(&direct_rule_lock);
	rule = mlx5dv_dr_rule_create(match, &key.params, 1, action);
	spin_unlock_np(&direct_rule_lock);


	if (!rule) {
		bitmap_atomic_clear(map, e->laddr.port);
		return -errno;
	}

	*handle_out = rule;

	return 0;
}

static int mlx5_deregister_flow(struct trans_entry *e, void *handle)
{
	int ret;

	if (e->proto == IPPROTO_TCP)
		bitmap_atomic_clear(tcp_listen_ports, e->laddr.port);
	else if (e->proto == IPPROTO_UDP)
		bitmap_atomic_clear(udp_listen_ports, e->laddr.port);
	else
		return -EINVAL;

	spin_lock_np(&direct_rule_lock);
	ret = mlx5dv_dr_rule_destroy(handle);
	spin_unlock_np(&direct_rule_lock);

	return ret;
}

static int mlx5_steer_flows(unsigned int *new_fg_assignment)
{
	int i, ret = 0;
	struct tbl *tbl;
	struct ibv_qp *new_qp, *old_qp;


	postsend_lock(dmn);

	for (i = 0; i < nr_rxq; i++) {
		if (new_fg_assignment[i] == fg_qp_assignment[i])
			continue;

		tbl = &fg_tbl[i];
		new_qp = rxqs[new_fg_assignment[i]].qp;
		old_qp = rxqs[fg_qp_assignment[i]].qp;

		ret = switch_qp_action(tbl->default_egress_rule, dmn,
			    new_qp, old_qp);
		if (unlikely(ret))
			break;

		fg_qp_assignment[i] = new_fg_assignment[i];
	}

	postsend_unlock(dmn);

	return ret;

}

static uint32_t mlx5_get_flow_affinity(uint8_t ipproto, uint16_t local_port, struct netaddr remote)
{
	bitmap_ptr_t map = ipproto == IPPROTO_TCP ? tcp_listen_ports :
			  udp_listen_ports;

	if (bitmap_atomic_test(map, local_port))
		return (remote.port & PORT_MASK) % maxks;
	else
		return (local_port & PORT_MASK) % maxks;
}

static int mlx5_init_flows(int rxq_count)
{
	int ret;

	spin_lock_init(&direct_rule_lock);
	nr_rxq = rxq_count;

	dmn = mlx5dv_dr_domain_create(context,
		MLX5DV_DR_DOMAIN_TYPE_NIC_RX);

	if (!dmn)
		return -errno;

	ret = mlx5_init_fg_tables();
	if (ret)
		return ret;

	ret = mlx5_init_udp();
	if (ret)
		return ret;

	ret = mlx5_init_tcp();
	if (ret)
		return ret;

	ret = mlx5_init_root_table();
	if (ret)
		return ret;

	ret = mlx5dv_dr_domain_sync(dmn, MLX5DV_DR_DOMAIN_SYNC_FLAGS_SW);
	if (ret)
		return ret;

	ret = mlx5dv_dr_domain_sync(dmn, MLX5DV_DR_DOMAIN_SYNC_FLAGS_HW);
	if (ret)
		return ret;

	return 0;

}


static int mlx5_fs_have_work(struct hardware_q *rxq)
{
	return hardware_q_pending(rxq);
}

static struct net_driver_ops mlx5_net_ops_flow_steering = {
	.rx_batch = mlx5_gather_rx,
	.tx_single = mlx5_transmit_one,
	.steer_flows = mlx5_steer_flows,
	.register_flow = mlx5_register_flow,
	.deregister_flow = mlx5_deregister_flow,
	.get_flow_affinity = mlx5_get_flow_affinity,
	.rxq_has_work = mlx5_fs_have_work,
};


int mlx5_init_flow_steering(struct hardware_q **rxq_out,struct direct_txq **txq_out,
	             unsigned int nr_rxq, unsigned int nr_txq)
{
	int ret;

	ret = mlx5_common_init(rxq_out, txq_out, nr_rxq, nr_txq, false);
	if (ret)
		return ret;

	ret = mlx5_init_flows(nr_rxq);
	if (ret)
		return ret;

	net_ops = mlx5_net_ops_flow_steering;

	return 0;
}

#endif
