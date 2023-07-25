#pragma once

typedef unsigned char u8;

struct mlx5_ifc_ipv4_layout_bits {
	u8 reserved_at_0[0x60];
	u8 ipv4[0x20];
};

struct mlx5_ifc_ipv6_layout_bits {
	u8 ipv6[16][0x8];
};

union mlx5_ifc_ipv6_layout_ipv4_layout_auto_bits {
	struct mlx5_ifc_ipv6_layout_bits ipv6_layout;
	struct mlx5_ifc_ipv4_layout_bits ipv4_layout;
	u8 reserved_at_0[0x80];
};

struct mlx5_ifc_fte_match_set_lyr_2_4_bits {
	u8 smac_47_16[0x20];
	u8 smac_15_0[0x10];
	u8 ethertype[0x10];
	u8 dmac_47_16[0x20];
	u8 dmac_15_0[0x10];
	u8 first_prio[0x3];
	u8 first_cfi[0x1];
	u8 first_vid[0xc];
	u8 ip_protocol[0x8];
	u8 ip_dscp[0x6];
	u8 ip_ecn[0x2];
	u8 cvlan_tag[0x1];
	u8 svlan_tag[0x1];
	u8 frag[0x1];
	u8 ip_version[0x4];
	u8 tcp_flags[0x9];
	u8 tcp_sport[0x10];
	u8 tcp_dport[0x10];
	u8 reserved_at_c0[0x20];
	u8 udp_sport[0x10];
	u8 udp_dport[0x10];
	union mlx5_ifc_ipv6_layout_ipv4_layout_auto_bits src_ipv4_src_ipv6;
	union mlx5_ifc_ipv6_layout_ipv4_layout_auto_bits dst_ipv4_dst_ipv6;
};

struct mlx5_ifc_fte_match_set_misc_bits {
	u8 reserved_at_0[0x8];
	u8 source_sqn[0x18];
	u8 reserved_at_20[0x10];
	u8 source_port[0x10];
	u8 outer_second_prio[0x3];
	u8 outer_second_cfi[0x1];
	u8 outer_second_vid[0xc];
	u8 inner_second_prio[0x3];
	u8 inner_second_cfi[0x1];
	u8 inner_second_vid[0xc];
	u8 outer_second_cvlan_tag[0x1];
	u8 inner_second_cvlan_tag[0x1];
	u8 outer_second_svlan_tag[0x1];
	u8 inner_second_svlan_tag[0x1];
	u8 reserved_at_64[0xc];
	u8 gre_protocol[0x10];
	u8 gre_key_h[0x18];
	u8 gre_key_l[0x8];
	u8 vxlan_vni[0x18];
	u8 reserved_at_b8[0x8];
	u8 reserved_at_c0[0x20];
	u8 reserved_at_e0[0xc];
	u8 outer_ipv6_flow_label[0x14];
	u8 reserved_at_100[0xc];
	u8 inner_ipv6_flow_label[0x14];
	u8 reserved_at_120[0xe0];
};

struct mlx5_ifc_fte_match_mpls_bits {
	u8 mpls_label[0x14];
	u8 mpls_exp[0x3];
	u8 mpls_s_bos[0x1];
	u8 mpls_ttl[0x8];
};

struct mlx5_ifc_fte_match_set_misc2_bits {
	struct mlx5_ifc_fte_match_mpls_bits outer_first_mpls;
	struct mlx5_ifc_fte_match_mpls_bits inner_first_mpls;
	struct mlx5_ifc_fte_match_mpls_bits outer_first_mpls_over_gre;
	struct mlx5_ifc_fte_match_mpls_bits outer_first_mpls_over_udp;
	u8 reserved_at_80[0x100];
	u8 metadata_reg_a[0x20];
	u8 reserved_at_1a0[0x60];
};

struct mlx5_ifc_fte_match_set_misc3_bits {
	u8 inner_tcp_seq_num[0x20];
	u8 outer_tcp_seq_num[0x20];
	u8 inner_tcp_ack_num[0x20];
	u8 outer_tcp_ack_num[0x20];
	u8 reserved_at_auto1[0x8];
	u8 outer_vxlan_gpe_vni[0x18];
	u8 outer_vxlan_gpe_next_protocol[0x8];
	u8 outer_vxlan_gpe_flags[0x8];
	u8 reserved_at_a8[0x10];
	u8 icmp_header_data[0x20];
	u8 icmpv6_header_data[0x20];
	u8 icmp_type[0x8];
	u8 icmp_code[0x8];
	u8 icmpv6_type[0x8];
	u8 icmpv6_code[0x8];
	u8 reserved_at_1a0[0xe0];
};

enum {
	MLX5_MATCH_CRITERIA_ENABLE_OUTER_BIT,
	MLX5_MATCH_CRITERIA_ENABLE_MISC_BIT,
	MLX5_MATCH_CRITERIA_ENABLE_INNER_BIT,
	MLX5_MATCH_CRITERIA_ENABLE_MISC2_BIT,
	MLX5_MATCH_CRITERIA_ENABLE_MISC3_BIT
};

/* Flow matcher. */
struct mlx5_ifc_fte_match_param_bits {
	struct mlx5_ifc_fte_match_set_lyr_2_4_bits outer_headers;
	struct mlx5_ifc_fte_match_set_misc_bits misc_parameters;
	struct mlx5_ifc_fte_match_set_lyr_2_4_bits inner_headers;
	struct mlx5_ifc_fte_match_set_misc2_bits misc_parameters_2;
	struct mlx5_ifc_fte_match_set_misc3_bits misc_parameters_3;
};


union match {
	struct {
		size_t size;
		/**< Size of match value. Do NOT split size and key! */
		uint32_t buf[DEVX_ST_SZ_DW(fte_match_param)];
		/**< Matcher value. This value is used as the mask or as a key. */
	};
	struct mlx5dv_flow_match_parameters params;
};
