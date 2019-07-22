#include <base/log.h>
#include <runtime/sync.h>

#ifdef DIRECTPATH

#include "mlx5.h"
#include "mlx5_ifc.h"

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

/* root level flow table */
static struct mlx5dv_dr_table		*root_tbl;
static struct mlx5dv_dr_matcher		*match_mac_and_tport;
static struct mlx5dv_dr_matcher		*match_just_mac;
static struct mlx5dv_dr_rule		*root_tcp_rule;
static struct mlx5dv_dr_rule		*root_udp_rule;
static struct mlx5dv_dr_rule		*root_catchall_rule;

/* second level flow tables */
static struct tbl		tcp_tbl;
static struct tbl		udp_tbl;

static struct mlx5dv_dr_matcher		*tcp_tbl_5tuple_match;

#define UDP_SPORT_BITS		10
#define UDP_SPORT_NENTRIES		(1 << UDP_SPORT_BITS)
static struct mlx5dv_dr_matcher		*udp_sport_match;
static struct mlx5dv_dr_matcher		*udp_tbl_5tuple_match;
static struct mlx5dv_dr_rule		*udp_rules[UDP_SPORT_NENTRIES];

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
	DR_MATCHER_CRITERIA_MISC		= 1 << 1,
	DR_MATCHER_CRITERIA_INNER		= 1 << 2,
	DR_MATCHER_CRITERIA_MISC2		= 1 << 3,
	DR_MATCHER_CRITERIA_MISC3		= 1 << 4,
	DR_MATCHER_CRITERIA_MAX 		= 1 << 5,
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

static int mlx5_init_fg_tables(void)
{
	int i, ret;

	for (i = 0; i < nr_rxq; i++) {
		/* forward to qp 0 */
		fg_fwd_action[i] = mlx5dv_dr_action_create_dest_ibv_qp(rxqs[0].qp);
		if (!fg_fwd_action[i])
			return -errno;

		ret = mlx5_tbl_init(&fg_tbl[i], 2, fg_fwd_action[i]);
		if (ret)
			return ret;

		fg_qp_assignment[i] = 0;
	}

	return 0;
}

static int mlx5_init_udp(void)
{
	int ret;
	unsigned int i, pos = 0;
	struct mlx5dv_dr_action *action[1];
	union match mask = {0};
	mask.size = DEVX_ST_SZ_BYTES(fte_match_param);

	ret = mlx5_tbl_init(&udp_tbl, 1, fg_tbl[0].ingress_action);
	if (ret)
		return ret;

	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_version, 4);
	DEVX_SET(fte_match_param, mask.buf, outer_headers.udp_sport, __devx_mask(UDP_SPORT_BITS));

	udp_sport_match = mlx5dv_dr_matcher_create(udp_tbl.tbl, 1, DR_MATCHER_CRITERIA_OUTER, &mask.params);
	if (!udp_sport_match)
		return -errno;

	for (i = 0; i < UDP_SPORT_NENTRIES; i++) {
		DEVX_SET(fte_match_param, mask.buf, outer_headers.udp_sport, i);
		action[0] = fg_tbl[pos++ % nr_rxq].ingress_action;
		udp_rules[i] = mlx5dv_dr_rule_create(udp_sport_match, &mask.params, 1, action);
		if (!udp_rules[i])
				return -errno;
	}

	memset(mask.buf, 0, sizeof(mask.buf));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_version, 4);
	DEVX_SET(fte_match_param, mask.buf, outer_headers.udp_sport, __devx_mask(16));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.udp_dport, __devx_mask(16));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.src_ipv4_src_ipv6.ipv4_layout.ipv4, __devx_mask(32));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.dst_ipv4_dst_ipv6.ipv4_layout.ipv4, __devx_mask(32));

	udp_tbl_5tuple_match = mlx5dv_dr_matcher_create(udp_tbl.tbl, 0,
		    DR_MATCHER_CRITERIA_OUTER, &mask.params);
	if (!udp_tbl_5tuple_match)
		return -errno;

	return 0;
}


static int mlx5_init_transport_tables(void)
{
	int ret;
	union match mask = {0};

	ret = mlx5_tbl_init(&tcp_tbl, 1, fg_tbl[0].ingress_action);
	if (ret)
		return ret;

	ret = mlx5_init_udp();
	if (ret)
		return ret;

	mask.size = DEVX_ST_SZ_BYTES(fte_match_param);
	DEVX_SET(fte_match_param, mask.buf, outer_headers.ip_version, 4);
	DEVX_SET(fte_match_param, mask.buf, outer_headers.tcp_sport, __devx_mask(16));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.tcp_dport, __devx_mask(16));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.src_ipv4_src_ipv6.ipv4_layout.ipv4, __devx_mask(32));
	DEVX_SET(fte_match_param, mask.buf, outer_headers.dst_ipv4_dst_ipv6.ipv4_layout.ipv4, __devx_mask(32));

	tcp_tbl_5tuple_match = mlx5dv_dr_matcher_create(tcp_tbl.tbl, 0,
		    DR_MATCHER_CRITERIA_OUTER, &mask.params);
	if (!tcp_tbl_5tuple_match)
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

int mlx5_register_flow(unsigned int affinity, uint8_t ipproto, struct netaddr laddr, struct netaddr raddr, void **handle_out)
{
	union match key = {0};
	struct mlx5dv_dr_matcher *match;
	struct mlx5dv_dr_action *action[1];
	void *rule;

	if (affinity > nr_rxq)
		return -EINVAL;

	if (ipproto != IPPROTO_TCP && ipproto != IPPROTO_UDP)
		return - EINVAL;

	key.size = sizeof(key.buf);
	DEVX_SET(fte_match_param, key.buf, outer_headers.ip_version, 4);
	DEVX_SET(fte_match_param, key.buf, outer_headers.src_ipv4_src_ipv6.ipv4_layout.ipv4, raddr.ip);
	DEVX_SET(fte_match_param, key.buf, outer_headers.dst_ipv4_dst_ipv6.ipv4_layout.ipv4, laddr.ip);

	if (ipproto == IPPROTO_TCP) {
		DEVX_SET(fte_match_param, key.buf, outer_headers.tcp_sport, raddr.port);
		DEVX_SET(fte_match_param, key.buf, outer_headers.tcp_dport, laddr.port);
		match = tcp_tbl_5tuple_match;
	} else {
		DEVX_SET(fte_match_param, key.buf, outer_headers.udp_sport, raddr.port);
		DEVX_SET(fte_match_param, key.buf, outer_headers.udp_dport, laddr.port);
		match = udp_tbl_5tuple_match;
	}

	action[0] = fg_tbl[affinity].ingress_action;

	spin_lock_np(&direct_rule_lock);
	rule = mlx5dv_dr_rule_create(match, &key.params, 1, action);
	spin_unlock_np(&direct_rule_lock);

	if (!rule)
		return -errno;

	*handle_out = rule;

	return 0;
}

int mlx5_deregister_flow(void *handle)
{
	int ret;

	spin_lock_np(&direct_rule_lock);
	ret = mlx5dv_dr_rule_destroy(handle);
	spin_unlock_np(&direct_rule_lock);

	return ret;
}

int mlx5_steer_flows(unsigned int *new_fg_assignment)
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

int mlx5_init_flows(int rxq_count)
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

	ret = mlx5_init_transport_tables();
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

#endif
