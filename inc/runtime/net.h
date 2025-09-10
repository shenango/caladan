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