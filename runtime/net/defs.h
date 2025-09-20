/*
 * defs.h - local definitions for networking
 */

#pragma once

#include <net/mbuf.h>
#include <net/ethernet.h>
#include <net/ip.h>
#include <runtime/net.h>
#include <runtime/rculist.h>

#include "../defs.h"

#define SMALL_BUF_SIZE 128
#define MBUF_INL_DATA_SZ (2 * CACHE_LINE_SIZE)

/*
 * Network Error Reporting Functions
 */

extern void trans_error(struct mbuf *m, int err);
extern void net_error(struct mbuf *m, int err);


/*
 * RX Networking Functions
 */

extern void net_rx_arp(struct mbuf *m);
extern void net_rx_icmp(struct mbuf *m, const struct ip_hdr *iphdr,
			uint16_t len);
extern void net_rx_trans(struct mbuf *m);
extern void tcp_rx_closed(struct mbuf *m);
extern void tcp_free_rx_bufs(void);
extern void net_rx_batch(struct mbuf **ms, unsigned int nr);


/*
 * TX Networking Functions
 */

/* the size of the region before a buffer to store struct mbuf */
#define MBUF_HEAD_LEN (align_up(sizeof(struct mbuf), CACHE_LINE_SIZE))

extern int arp_lookup(uint32_t daddr, struct eth_addr *dhost_out,
		      struct mbuf *m, bool *is_local) __must_use_return;
extern struct mbuf *net_tx_alloc_mbuf(size_t header_len);
DECLARE_PERTHREAD(struct tcache_perthread, mbuf_pt);

#ifdef SPLIT_TX
extern struct mbuf *net_tx_alloc_mbuf_small(size_t header_len);
#endif

static inline struct mbuf *net_tx_alloc_mbuf_sz(size_t header_len, size_t len)
{
#ifdef SPLIT_TX
	if (header_len + len <= SMALL_BUF_SIZE)
		return net_tx_alloc_mbuf_small(header_len);
#endif
	return net_tx_alloc_mbuf(header_len);
}

static inline int validate_and_normalize_laddr(struct netaddr *laddr,
                                               uint32_t saddr,
                                               bool allow_zero_port)
{
	if (laddr->ip == 0) {
		if (saddr == MAKE_IP_ADDR(127, 0, 0, 1))
			laddr->ip = MAKE_IP_ADDR(127, 0, 0, 1);
		else
			laddr->ip = netcfg.addr;
	} else if (laddr->ip != netcfg.addr &&
	         laddr->ip != MAKE_IP_ADDR(127, 0, 0, 1))
		return -EINVAL;
	if (!allow_zero_port && laddr->port == 0)
		return -EINVAL;
	return 0;
}

extern void net_tx_release_mbuf(struct mbuf *m);
extern void net_tx_eth(struct mbuf *m, uint16_t proto,
		       const struct eth_addr *dhost, bool is_local);
extern int net_tx_ip(struct mbuf *m, uint8_t proto,
		     uint32_t daddr, uint32_t saddr) __must_use_return;
extern int net_tx_icmp(struct mbuf *m, uint8_t type, uint8_t code,
		uint32_t daddr, uint16_t id, uint16_t seq) __must_use_return;

static inline size_t eth_headroom(void)
{
	return sizeof(struct eth_hdr);
}

static inline size_t ip_headroom(void)
{
	return eth_headroom() + sizeof(struct ip_hdr);
}

extern size_t calculate_egress_buf_size(void);

/**
 * net_tx_ip - transmits an IP packet, or frees it on failure
 * @m: the mbuf to transmit
 * @proto: the transport protocol
 * @daddr: the destination IP address (in native byte order)
 *
 * The payload must start with the transport (L4) header. The IPv4 (L3) and
 * ethernet (L2) headers will be prepended by this function.
 *
 * @m must have been allocated with net_tx_alloc_mbuf().
 */
static inline void net_tx_ip_or_free(struct mbuf *m, uint8_t proto,
				     uint32_t daddr, uint32_t saddr)
{
	if (unlikely(net_tx_ip(m, proto, daddr, saddr) != 0))
		mbuf_free(m);
}

/**
 * mbuf_drop - frees an mbuf, counting it as a drop
 * @m: the mbuf to free
 */
static inline void mbuf_drop(struct mbuf *m)
{
	mbuf_free(m);
	STAT(DROPS)++;
}


/*
 * Transport protocol layer
 */

enum {
	/* match on protocol, source IP and port */
	TRANS_MATCH_3TUPLE = 0,
	/* match on protocol, source IP and port + dest IP and port */
	TRANS_MATCH_5TUPLE,
};

static inline void assert_valid_match(int match)
{
	assert(match == TRANS_MATCH_3TUPLE ||
	       match == TRANS_MATCH_5TUPLE);
}

struct trans_entry;

struct trans_ops {
	/* receive an ingress packet */
	void (*recv) (struct trans_entry *e, struct mbuf *m);
	/* propagate a network error */
	void (*err) (struct trans_entry *e, int err);
};

/* tracks usage of port numbers (typedef'd as bind_token_t) */
struct lport_entry {
	// protected by @trans_lock
	struct list_node link;
	size_t nr_active_entries;
	size_t nr_reserved_entries;

	// fields that do not change after creation
	struct netaddr laddr;
	uint8_t proto;
	bool reuse_port;
};

struct trans_entry {
	struct netaddr		laddr;
	int8_t			match;
	uint8_t			proto;
	struct netaddr		raddr;
	bool 			reuse_port;
	int8_t			pad;
	struct rcu_hlist_node	link;
	struct rcu_head		rcu;
	const struct trans_ops	*ops;
	bind_token_t		*bind_token;
};

BUILD_ASSERT(sizeof(struct trans_entry) == CACHE_LINE_SIZE);

/**
 * trans_init_3tuple - initializes a transport layer entry (3-tuple match)
 * @e: the entry to initialize
 * @proto: the IP protocol
 * @ops: operations to handle matching flows
 * @laddr: the local address
 */
static inline void trans_init_3tuple(struct trans_entry *e, uint8_t proto,
				     const struct trans_ops *ops,
				     struct netaddr laddr)
{
	e->match = TRANS_MATCH_3TUPLE;
	e->proto = proto;
	e->laddr = laddr;
	e->ops = ops;
	e->reuse_port = false;
	memset(&e->raddr, 0, sizeof(e->raddr));
}

/**
 * trans_init_5tuple - initializes a transport layer entry (5-tuple match)
 * @e: the entry to initialize
 * @proto: the IP protocol
 * @ops: operations to handle matching flows
 * @laddr: the local address
 * @raddr: the remote address
 */
static inline void trans_init_5tuple(struct trans_entry *e, uint8_t proto,
				     const struct trans_ops *ops,
				     struct netaddr laddr, struct netaddr raddr)
{
	e->match = TRANS_MATCH_5TUPLE;
	e->proto = proto;
	e->laddr = laddr;
	e->raddr = raddr;
	e->ops = ops;
	e->reuse_port = false;
}

extern int __trans_table_add(struct trans_entry *e, bind_token_t *token);

/**
 * trans_table_add - adds an entry to the match table
 * @e: the entry to add
 *
 * Returns 0 if successful, or -EADDRINUSE if a conflicting entry is already in
 * the table, or -EINVAL if the local port is zero.
 */
static inline int trans_table_add(struct trans_entry *e)
{
	return __trans_table_add(e, NULL);
}

extern int trans_table_add_with_ephemeral_port(struct trans_entry *e);
extern void trans_table_remove(struct trans_entry *e);


/*
 * Flow registration support
 */

struct flow_registration {
	unsigned int		kthread_affinity;

	struct trans_entry	*e;
	struct kref		*ref;
	void (*release)(struct kref *ref);

	void			*hw_flow_handle;
	struct list_node	flow_reg_link;
	struct list_node	flow_dereg_link;
};

#ifdef DIRECTPATH
extern void register_flow(struct flow_registration *f);
extern void deregister_flow(struct flow_registration *f);
#else
static inline void register_flow(struct flow_registration *f) {}
static inline void deregister_flow(struct flow_registration *f) {}
#endif
