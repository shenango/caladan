/*
 * net.h - shared network definitions
 */

#pragma once

#include <base/types.h>
#include <net/ip.h>

struct netaddr {
	uint32_t ip;
	uint16_t port;
} __packed;

// Some RX methods can request additional information from the network stack by
// passing in a pointer to this struct. The provider should set valid to false
// to detect whether the network stack filled in the fields.
struct aux_rx_pkt_data {
	/* network stack sets valid to true if it fills in these fields */
	bool valid;

	/* ip header packet info */
	uint32_t daddr;
	uint8_t tos;
};

// Some TX methods can provide additional information to the network stack by
// passing in a pointer to this struct. The provider should set fields_present
// to indicate which fields are present.
struct aux_tx_pkt_data {
	union {
		struct {
			unsigned int has_ecn:1;
		};
		unsigned int fields_present;
	};
	/* IP header ECN bits */
	uint8_t ecn;
};

struct lport_entry;
typedef struct lport_entry bind_token_t;

extern int str_to_netaddr(const char *str, struct netaddr *addr);
extern int trans_reserve_port(struct netaddr *laddr, uint8_t proto,
                              bool reuse_port, bind_token_t **token);
extern void trans_release_port(bind_token_t *token);

static inline int tcp_reserve_port(struct netaddr *laddr, bool reuse_port,
                                   bind_token_t **token)
{
	return trans_reserve_port(laddr, IPPROTO_TCP, reuse_port, token);
}

static inline void tcp_release_port(bind_token_t *token)
{
	trans_release_port(token);
}

static inline int udp_reserve_port(struct netaddr *laddr, bool reuse_port,
                                   bind_token_t **token)
{
	return trans_reserve_port(laddr, IPPROTO_UDP, reuse_port, token);
}

static inline void udp_release_port(bind_token_t *token)
{
	trans_release_port(token);
}

extern struct netaddr get_netaddr(bind_token_t *token);