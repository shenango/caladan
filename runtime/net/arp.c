/*
 * arp.c - support for address resolution protocol (ARP)
 */

#include <stddef.h>

#include <base/lock.h>
#include <base/log.h>
#include <base/hash.h>
#include <net/arp.h>
#include <runtime/rculist.h>
#include <runtime/timer.h>
#include <runtime/smalloc.h>
#include <runtime/sync.h>

#include "defs.h"

#define ARP_SEED		0xD4812A53
#define ARP_TABLE_CAPACITY	1024
#define ARP_RETRIES		3
#define ARP_RETRY_TIME		ONE_SECOND
#define ARP_REPROBE_TIME	(10 * ONE_SECOND)

enum {
	/* the MAC address is being probed */
	ARP_STATE_PROBING = 0,
	/* the MAC address is valid */
	ARP_STATE_VALID,
	/* the MAC address is probably valid but is being confirmed */
	ARP_STATE_VALID_BUT_REPROBING,
	/* Statically configured arp entry */
	ARP_STATE_STATIC,
};

/* A single entry in the ARP table. */
struct arp_entry {
	/* accessed by RCU sections */
	uint32_t		state;
	uint32_t		ip;
	struct eth_addr		eth;
	struct rcu_hlist_node	link;

	/* accessed only with @arp_lock */
	struct mbufq		q;
	struct rcu_head		rcuh;
	uint64_t		ts;
	int			tries_left;
	int			pad;
};

static DEFINE_SPINLOCK(arp_lock);
static size_t nr_dynamic_entries;
static thread_t *arp_worker_th;
static struct rcu_hlist_head arp_tbl[ARP_TABLE_CAPACITY];

static inline int hash_ip(uint32_t ip)
{
	return hash_crc32c_one(ARP_SEED, ip) % ARP_TABLE_CAPACITY;
}

static struct arp_entry *lookup_entry(int idx, uint32_t daddr)
{
	struct arp_entry *e;
	struct rcu_hlist_node *node;

	rcu_hlist_for_each(&arp_tbl[idx], node, true) {
		e = rcu_hlist_entry(node, struct arp_entry, link);
		if (e->ip == daddr)
			return e;
	}

	return NULL;
}

static void release_entry(struct rcu_head *h)
{
	struct arp_entry *e = container_of(h, struct arp_entry, rcuh);
	sfree(e);
}

static void delete_entry(struct arp_entry *e)
{
	rcu_hlist_del(&e->link);

	/* free any mbufs waiting for an ARP response */
	while (!mbufq_empty(&e->q)) {
		struct mbuf *m = mbufq_pop_head(&e->q);
		net_error(m, EHOSTUNREACH);
		mbuf_free(m);
	}

	assert(e->state != ARP_STATE_STATIC);
	nr_dynamic_entries--;

	rcu_free(&e->rcuh, release_entry);
}

static void insert_entry(struct arp_entry *e, int idx)
{
	rcu_hlist_add_head(&arp_tbl[idx], &e->link);
	if (e->state != ARP_STATE_STATIC) {
		if (++nr_dynamic_entries == 1) {
			if (arp_worker_th) {
				thread_ready(arp_worker_th);
				arp_worker_th = NULL;
			}
		}
	}
}

static struct arp_entry *create_entry(uint32_t daddr)
{
	struct arp_entry *e = smalloc(sizeof(*e));
	if (!e)
		return NULL;

	e->ip = daddr;
	e->state = ARP_STATE_PROBING;
	e->ts = microtime();
	e->tries_left = ARP_RETRIES;
	mbufq_init(&e->q);
	return e;
}

static void arp_send(uint16_t op, struct eth_addr dhost, uint32_t daddr)
{
	struct mbuf *m;
	struct arp_hdr *arp_hdr;
	struct arp_hdr_ethip *arp_hdr_ethip;

	m = net_tx_alloc_mbuf();
	if (unlikely(!m))
		return;

	arp_hdr = mbuf_put_hdr(m, *arp_hdr);
	arp_hdr->htype = hton16(ARP_HTYPE_ETHER);
	arp_hdr->ptype = hton16(ETHTYPE_IP);
	arp_hdr->hlen = sizeof(struct eth_addr);
	arp_hdr->plen = sizeof(uint32_t);
	arp_hdr->op = hton16(op);

	arp_hdr_ethip = mbuf_put_hdr(m, *arp_hdr_ethip);
	arp_hdr_ethip->sender_mac = netcfg.mac;
	arp_hdr_ethip->sender_ip = hton32(netcfg.addr);
	arp_hdr_ethip->target_mac = dhost;
	arp_hdr_ethip->target_ip = hton32(daddr);

	net_tx_eth(m, ETHTYPE_ARP, dhost);
}

static void arp_age_entry(uint64_t now_us, struct arp_entry *e)
{
	/* check if this entry has timed out */
	if (now_us - e->ts < ((e->state == ARP_STATE_VALID) ?
			      ARP_REPROBE_TIME : ARP_RETRY_TIME))
		return;

	switch (e->state) {
	case ARP_STATE_PROBING:
	case ARP_STATE_VALID_BUT_REPROBING:
		if (e->tries_left == 0) {
			delete_entry(e);
			return;
		}
		e->tries_left--;
		break;

	case ARP_STATE_VALID:
		e->state = ARP_STATE_VALID_BUT_REPROBING;
		e->tries_left = ARP_RETRIES;
		break;

	case ARP_STATE_STATIC:
		return;

	default:
		panic("arp: invalid entry state %d", e->state);
	}

	arp_send(ARP_OP_REQUEST, eth_addr_broadcast, e->ip);
	e->ts = microtime();
}

static void arp_worker(void *arg)
{
	struct arp_entry *e;
	struct rcu_hlist_node *node;
	uint64_t now_us;
	int i;

	/* wake up each second and update the ARP table */
	while (true) {
		now_us = microtime();

		spin_lock_np(&arp_lock);

		for (i = 0; i < ARP_TABLE_CAPACITY; i++) {
			rcu_hlist_for_each(&arp_tbl[i], node, true) {
				e = rcu_hlist_entry(node,
						    struct arp_entry, link);
				arp_age_entry(now_us, e);
			}
			if (unlikely(preempt_needed())) {
				spin_unlock_np(&arp_lock);
				spin_lock_np(&arp_lock);
			}
		}

		if (!nr_dynamic_entries) {
			arp_worker_th = thread_self();
			thread_park_and_unlock_np(&arp_lock);
		} else {
			spin_unlock_np(&arp_lock);
			timer_sleep(ONE_SECOND);
		}
	}
}

static void arp_update(uint32_t daddr, struct eth_addr dhost)
{
	struct mbufq q;
	int idx = hash_ip(daddr);
	struct arp_entry *e;

	mbufq_init(&q);

	spin_lock_np(&arp_lock);
	e = lookup_entry(idx, daddr);
	if (!e) {
		e = create_entry(daddr);
		if (unlikely(!e)) {
			spin_unlock_np(&arp_lock);
			return;
		}

		insert_entry(e, idx);
	} else if (load_acquire(&e->state) == ARP_STATE_STATIC) {
		spin_unlock_np(&arp_lock);
		return;
	}
	e->eth = dhost;
	e->ts = microtime();
	store_release(&e->state, ARP_STATE_VALID);
	mbufq_merge_to_tail(&q, &e->q);
	spin_unlock_np(&arp_lock);

	/* drain mbufs waiting for ARP response */
	while (!mbufq_empty(&q)) {
		struct mbuf *m = mbufq_pop_head(&q);
		net_tx_eth(m, ETHTYPE_IP, dhost);
	}
}

/**
 * net_rx_arp - receive an ARP packet
 * @m: the mbuf containing the ARP packet (eth hdr is stripped)
 */
void net_rx_arp(struct mbuf *m)
{
	uint16_t op;
	bool am_target;
	struct arp_hdr *arp_hdr;
	struct arp_hdr_ethip *arp_hdr_ethip;
	uint32_t sender_ip, target_ip;
	struct eth_addr sender_mac;

	arp_hdr = mbuf_pull_hdr_or_null(m, *arp_hdr);
	arp_hdr_ethip = mbuf_pull_hdr_or_null(m, *arp_hdr_ethip);
	if (!arp_hdr || !arp_hdr_ethip)
		goto out;

	/* make sure the arp header is valid */
	if (ntoh16(arp_hdr->htype) != ARP_HTYPE_ETHER ||
	    ntoh16(arp_hdr->ptype) != ETHTYPE_IP ||
	    arp_hdr->hlen != sizeof(struct eth_addr) ||
	    arp_hdr->plen != sizeof(uint32_t))
		goto out;

	op = ntoh16(arp_hdr->op);
	sender_ip = ntoh32(arp_hdr_ethip->sender_ip);
	target_ip = ntoh32(arp_hdr_ethip->target_ip);
	sender_mac = arp_hdr_ethip->sender_mac;

	/* refuse ARP packets with multicast source MAC's */
	if (eth_addr_is_multicast(&sender_mac))
		goto out;

	am_target = (netcfg.addr == target_ip);
	arp_update(sender_ip, sender_mac);

	if (am_target && op == ARP_OP_REQUEST) {
		log_debug("arp: responding to arp request "
				  "from IP %d.%d.%d.%d",
				  ((sender_ip >> 24) & 0xff),
				  ((sender_ip >> 16) & 0xff),
				  ((sender_ip >> 8) & 0xff),
				  (sender_ip & 0xff));

		arp_send(ARP_OP_REPLY, sender_mac, sender_ip);
	}

out:
	mbuf_free(m);
}

/**
 * arp_lookup - retrieve a MAC address for a given IP address
 * @daddr: the target IP address
 * @dhost_out: A buffer to store the MAC address
 * @m: the mbuf requiring the lookup (can be NULL, otherwise must start with
 * a network header (L3))
 *
 * Returns 0 and writes to @dhost_out if successful. Otherwise returns:
 * -ENOMEM: If out of memory
 * -EINPROGRESS: If the ARP request is still resolving. Takes ownership of @m.
 */
int arp_lookup(uint32_t daddr, struct eth_addr *dhost_out, struct mbuf *m)
{
	struct arp_entry *e, *newe = NULL;
	int idx = hash_ip(daddr);

	/* hot-path: @daddr hits in ARP cache */
	rcu_read_lock();
	e = lookup_entry(idx, daddr);
	if (likely(e && load_acquire(&e->state) != ARP_STATE_PROBING)) {
		*dhost_out = e->eth;
		rcu_read_unlock();
		return 0;
	}
	rcu_read_unlock();

	/* cold-path: solicit an ARP response */
	if (!e) {
		arp_send(ARP_OP_REQUEST, eth_addr_broadcast, daddr);
		newe = create_entry(daddr);
		if (!newe)
			return -ENOMEM;
	}

	/* check again for @daddr in ARP cache; we own @m going forward */
	spin_lock_np(&arp_lock);
	e = lookup_entry(idx, daddr);
	if (e) {
		/* entry already exists */
		if (newe)
			sfree(newe);
		if (e->state != ARP_STATE_PROBING) {
			*dhost_out = e->eth;
			spin_unlock_np(&arp_lock);
			return 0;
		}
	} else if (newe) {
		/* insert new entry */
		e = newe;
		insert_entry(e, idx);
	}

	/* enqueue the mbuf for later transmission */
	if (m && e)
		mbufq_push_tail(&e->q, m);
	spin_unlock_np(&arp_lock);

	/* if the entry was removed, assume unreachable and free */
	if (m && !e)
		mbuf_free(m);

	return -EINPROGRESS;
}

/**
 * arp_init - initializes the ARP subsystem
 *
 * Always returns 0 for success.
 */
int arp_init(void)
{
	int i;

	spin_lock_init(&arp_lock);
	for (i = 0; i < ARP_TABLE_CAPACITY; i++)
		rcu_hlist_init_head(&arp_tbl[i]);

	return 0;
}

/**
 * arp_init_late - starts the ARP worker thread
 *
 * Returns 0 if successful.
 */
int arp_init_late(void)
{
	int i, idx;
	struct arp_entry *e;

	spin_lock_np(&arp_lock);

	for (i = 0; i < arp_static_count; i++) {
		e = create_entry(static_entries[i].ip);
		if (!e)
			return -ENOMEM;
		idx = hash_ip(static_entries[i].ip);
		e->eth = static_entries[i].addr;
		e->state = ARP_STATE_STATIC;
		insert_entry(e, idx);
	}

	spin_unlock_np(&arp_lock);

	free(static_entries);

	arp_worker_th = thread_create(arp_worker, NULL);
	if (!arp_worker_th)
		return -ENOMEM;

	return 0;
}
