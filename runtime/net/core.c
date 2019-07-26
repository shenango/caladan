/*
 * core.c - core networking infrastructure
 */

#include <stdio.h>

#include <base/log.h>
#include <base/mempool.h>
#include <base/slab.h>
#include <base/hash.h>
#include <base/thread.h>
#include <asm/chksum.h>
#include <runtime/net.h>
#include <runtime/smalloc.h>

#include "defs.h"

#define IP_ID_SEED	0x42345323
#define RX_PREFETCH_STRIDE 2

/* important global state */
struct net_cfg netcfg __aligned(CACHE_LINE_SIZE);

/* TX buffer allocation */
struct mempool net_tx_buf_mp;
static struct tcache *net_tx_buf_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, net_tx_buf_pt);

#define MBUF_RESERVED (align_up(sizeof(struct mbuf), CACHE_LINE_SIZE))


/*
 * RX Networking Functions
 */

static void net_rx_send_completion(unsigned long completion_data)
{
	struct kthread *k;

	k = getk();
	if (unlikely(!lrpc_send(&k->txcmdq, TXCMD_NET_COMPLETE,
				completion_data))) {
		WARN();
	}
	putk();
}

static struct mbuf *net_rx_alloc_mbuf(struct rx_net_hdr *hdr)
{
	struct mbuf *m;
	void *buf;

	/* allocate the buffer to store the payload */
	m = smalloc(hdr->len + MBUF_RESERVED);
	if (unlikely(!m))
		goto out;

	buf = (unsigned char *)m + MBUF_RESERVED;

	/* copy the payload and release the buffer back to the iokernel */
	memcpy(buf, hdr->payload, hdr->len);

	mbuf_init(m, buf, MBUF_DEFAULT_LEN - MBUF_RESERVED, 0);
	m->len = hdr->len;
	m->csum_type = hdr->csum_type;
	m->csum = hdr->csum;
	m->rss_hash = hdr->rss_hash;

	m->release = (void (*)(struct mbuf *))sfree;

out:
	net_rx_send_completion(hdr->completion_data);
	return m;
}

static inline bool ip_hdr_supported(const struct ip_hdr *iphdr)
{
	/* must be IPv4, no IP options, no IP fragments */
	return (iphdr->version == IPVERSION &&
		iphdr->header_len == sizeof(*iphdr) / sizeof(uint32_t) &&
		(iphdr->off & IP_MF) == 0);
}

/**
 * net_error - reports a network error so that it can be passed to higher layers
 * @m: the egress mbuf that triggered the error
 * @err: the suggested error code to report
 *
 * The mbuf data pointer must point to the network-layer (L3) hdr that failed.
 */
void net_error(struct mbuf *m, int err)
{
	const struct ip_hdr *iphdr;

	iphdr = mbuf_pull_hdr_or_null(m, *iphdr);
	if (unlikely(!iphdr))
		return;
	if (unlikely(!ip_hdr_supported(iphdr)))
		return;

	/* don't check length because ICMP may not provide the full payload */

	/* so far we only support error handling in UDP and TCP */
	if (iphdr->proto == IPPROTO_UDP || iphdr->proto == IPPROTO_TCP)
		trans_error(m, err);
}

static struct mbuf *net_rx_one(struct mbuf *m)
{
	const struct eth_hdr *llhdr;
	const struct ip_hdr *iphdr;
	uint16_t len;

	STAT(RX_PACKETS)++;
	STAT(RX_BYTES) += mbuf_length(m);

	/*
	 * Link Layer Processing (OSI L2)
	 */

	llhdr = mbuf_pull_hdr_or_null(m, *llhdr);
	if (unlikely(!llhdr))
		goto drop;

	/* handle ARP requests */
	if (ntoh16(llhdr->type) == ETHTYPE_ARP) {
		net_rx_arp(m);
		return NULL;
	}

	/* filter out requests we can't handle */
	BUILD_ASSERT(sizeof(llhdr->dhost.addr) == sizeof(netcfg.mac.addr));
	if (unlikely(ntoh16(llhdr->type) != ETHTYPE_IP ||
		     memcmp(llhdr->dhost.addr, netcfg.mac.addr,
			    sizeof(llhdr->dhost.addr)) != 0))
		goto drop;


	/*
	 * Network Layer Processing (OSI L3)
	 */

	mbuf_mark_network_offset(m);
	iphdr = mbuf_pull_hdr_or_null(m, *iphdr);
	if (unlikely(!iphdr))
		goto drop;

	/* Did HW checksum verification pass? */
	if (m->csum_type != CHECKSUM_TYPE_UNNECESSARY) {
		if (chksum_internet(iphdr, sizeof(*iphdr)))
			goto drop;
	}

	if (unlikely(!ip_hdr_supported(iphdr)))
		goto drop;

	len = ntoh16(iphdr->len) - sizeof(*iphdr);
	if (unlikely(mbuf_length(m) < len))
		goto drop;
	if (len < mbuf_length(m))
		mbuf_trim(m, mbuf_length(m) - len);

	switch(iphdr->proto) {
	case IPPROTO_ICMP:
		net_rx_icmp(m, iphdr, len);
		break;

	case IPPROTO_UDP:
	case IPPROTO_TCP:
		return m;

	default:
		goto drop;
	}

	return NULL;

drop:
	mbuf_drop(m);
	return NULL;
}

/**
 * net_rx_softirq_direct - handles ingress packet processing
 * This variant is intended for packets from directpath queues
 * @ms: an array of ingress packets
 * @nr: the size of the @ms array
 */
void net_rx_softirq_direct(struct mbuf **ms, unsigned int nr)
{
	struct mbuf *l4_reqs[SOFTIRQ_MAX_BUDGET];
	int i, l4idx = 0;

	for (i = 0; i < nr; i++) {
		l4_reqs[l4idx] = net_rx_one(ms[i]);
		if (l4_reqs[l4idx] != NULL)
			l4idx++;
	}

	/* handle transport protocol layer */
	if (l4idx > 0)
		net_rx_trans(l4_reqs, l4idx);
}

/**
 * net_rx_softirq - handles ingress packet processing
 * This variant is intended for packets from the IOKernel
 * @hdrs: an array of ingress packet headers
 * @nr: the size of the @hdrs array
 */
void net_rx_softirq(struct rx_net_hdr **hdrs, unsigned int nr)
{
	struct mbuf *m;
	struct mbuf *l4_reqs[SOFTIRQ_MAX_BUDGET];
	int i, l4idx = 0;

	for (i = 0; i < nr; i++) {
		if (i + RX_PREFETCH_STRIDE < nr)
			prefetch(hdrs[i + RX_PREFETCH_STRIDE]);
		m = net_rx_alloc_mbuf(hdrs[i]);
		if (unlikely(!m)) {
			STAT(DROPS)++;
			continue;
		}
		l4_reqs[l4idx] = net_rx_one(m);
		if (l4_reqs[l4idx] != NULL)
			l4idx++;
	}

	/* handle transport protocol layer */
	if (l4idx > 0)
		net_rx_trans(l4_reqs, l4idx);
}


/*
 * TX Networking Functions
 */

/**
 * net_tx_release_mbuf - the default TX mbuf release handler
 * @m: the mbuf to free
 *
 * Normally, this handler will get called automatically. If you override
 * mbuf.release(), call this method manually.
 */
void net_tx_release_mbuf(struct mbuf *m)
{
	preempt_disable();
	tcache_free(&perthread_get(net_tx_buf_pt), m);
	preempt_enable();
}

/**
 * net_tx_alloc_mbuf - allocates an mbuf for transmitting.
 *
 * Returns an mbuf, or NULL if out of memory.
 */
struct mbuf *net_tx_alloc_mbuf(void)
{
	struct mbuf *m;
	unsigned char *buf;

	preempt_disable();
	m = tcache_alloc(&perthread_get(net_tx_buf_pt));
	if (unlikely(!m)) {
		preempt_enable();
		log_warn_ratelimited("net: out of tx buffers");
		return NULL;
	}

	preempt_enable();

	buf = (unsigned char *)m + MBUF_RESERVED;

	mbuf_init(m, buf, MBUF_DEFAULT_LEN - MBUF_RESERVED,
		  MBUF_DEFAULT_HEADROOM);
	m->csum_type = CHECKSUM_TYPE_NEEDED;
	m->txflags = 0;
	m->release_data = 0;
	m->release = net_tx_release_mbuf;
	return m;
}

/* drains overflow queues */
void __noinline net_tx_drain_overflow(void)
{
	struct mbuf *m;
	struct kthread *k = myk();

	assert_preempt_disabled();

	/* drain TX packets */
	while (!mbufq_empty(&k->txpktq_overflow)) {
		m = mbufq_peak_head(&k->txpktq_overflow);
#ifndef DIRECTPATH
		shmptr_t shm = ptr_to_shmptr(&netcfg.tx_region,
				    mbuf_data(m), mbuf_length(m));
		if (!lrpc_send(&k->txpktq, TXPKT_NET_XMIT, shm))
			break;
#else
		if (net_ops.tx_single(k->directpath_txq, m))
			break;
#endif
		mbufq_pop_head(&k->txpktq_overflow);
		if (unlikely(preempt_needed()))
			return;
	}
}

static void net_tx_raw(struct mbuf *m)
{
	struct kthread *k;
	unsigned int len = mbuf_length(m);

	k = getk();
	/* drain pending overflow packets first */
	if (!mbufq_empty(&k->txpktq_overflow))
		net_tx_drain_overflow();

	STAT(TX_PACKETS)++;
	STAT(TX_BYTES) += len;

#ifndef DIRECTPATH
	struct tx_net_hdr *hdr = mbuf_push_hdr(m, *hdr);
	hdr->completion_data = (unsigned long)m;
	hdr->len = len;
	hdr->olflags = m->txflags;
	shmptr_t shm = ptr_to_shmptr(&netcfg.tx_region, hdr, len + sizeof(*hdr));

	if (!lrpc_send(&k->txpktq, TXPKT_NET_XMIT, shm)) {
		mbufq_push_tail(&k->txpktq_overflow, m);
		STAT(TXQ_OVERFLOW)++;
	}
#else
	if (unlikely(net_ops.tx_single(k->directpath_txq, m))) {
		mbufq_push_tail(&k->txpktq_overflow, m);
		STAT(TXQ_OVERFLOW)++;
	}
#endif
	putk();
}

/**
 * net_tx_eth - transmits an ethernet packet
 * @m: the mbuf to transmit
 * @type: the ethernet type (in native byte order)
 * @dhost: the destination MAC address
 *
 * The payload must start with the network (L3) header. The ethernet (L2)
 * header will be prepended by this function.
 *
 * @m must have been allocated with net_tx_alloc_mbuf().
 *
 * Returns 0 if successful. If successful, the mbuf will be freed when the
 * transmit completes. Otherwise, the mbuf still belongs to the caller.
 */
int net_tx_eth(struct mbuf *m, uint16_t type, struct eth_addr dhost)
{
	struct eth_hdr *eth_hdr;

	eth_hdr = mbuf_push_hdr(m, *eth_hdr);
	eth_hdr->shost = netcfg.mac;
	eth_hdr->dhost = dhost;
	eth_hdr->type = hton16(type);
	net_tx_raw(m);
	return 0;
}

static void net_push_iphdr(struct mbuf *m, uint8_t proto, uint32_t daddr)
{
	struct ip_hdr *iphdr;

	/* TODO: Support "don't fragment" (DF) flag? */

	/* populate IP header */
	iphdr = mbuf_push_hdr(m, *iphdr);
	iphdr->version = IPVERSION;
	iphdr->header_len = 5;
	iphdr->tos = IPTOS_DSCP_CS0 | IPTOS_ECN_NOTECT;
	iphdr->len = hton16(mbuf_length(m));
	/* This must be unique across datagrams within a flow, see RFC 6864 */
	iphdr->id = hash_crc32c_two(IP_ID_SEED, rdtsc() ^ proto,
				    (uint64_t)daddr |
				    ((uint64_t)netcfg.addr << 32));
	iphdr->off = 0;
	iphdr->ttl = 64;
	iphdr->proto = proto;
	iphdr->chksum = 0;
	iphdr->saddr = hton32(netcfg.addr);
	iphdr->daddr = hton32(daddr);
}

static uint32_t net_get_ip_route(uint32_t daddr)
{
	/* simple IP routing */
	if ((daddr & netcfg.netmask) != (netcfg.addr & netcfg.netmask))
		daddr = netcfg.gateway;
	return daddr;
}

/**
 * net_tx_ip - transmits an IP packet
 * @m: the mbuf to transmit
 * @proto: the transport protocol
 * @daddr: the destination IP address (in native byte order)
 *
 * The payload must start with the transport (L4) header. The IPv4 (L3) and
 * ethernet (L2) headers will be prepended by this function.
 *
 * @m must have been allocated with net_tx_alloc_mbuf().
 *
 * Returns 0 if successful. If successful, the mbuf will be freed when the
 * transmit completes. Otherwise, the mbuf still belongs to the caller.
 */
int net_tx_ip(struct mbuf *m, uint8_t proto, uint32_t daddr)
{
	struct eth_addr dhost;
	int ret;

	/* prepend the IP header */
	net_push_iphdr(m, proto, daddr);

	/* ask NIC to calculate IP checksum */
	m->txflags |= OLFLAG_IP_CHKSUM | OLFLAG_IPV4;

	/* apply IP routing */
	daddr = net_get_ip_route(daddr);

	/* need to use ARP to resolve dhost */
	ret = arp_lookup(daddr, &dhost, m);
	if (unlikely(ret)) {
		if (ret == -EINPROGRESS) {
			/* ARP code now owns the mbuf */
			return 0;
		} else {
			/* An unrecoverable error occurred */
			mbuf_pull_hdr(m, struct ip_hdr);
			return ret;
		}
	}

	ret = net_tx_eth(m, ETHTYPE_IP, dhost);
	assert(!ret); /* can't fail as implemented so far */
	return 0;
}

/**
 * net_tx_ip_burst - transmits a burst of IP packets
 * @ms: an array of mbuf pointers to transmit
 * @n: the number of mbufs in @ms
 * @proto: the transport protocol
 * @daddr: the destination IP address (in native byte order)
 *
 * The payload must start with the transport (L4) header. The IPv4 (L3) and
 * ethernet (L2) headers will be prepended by this function.
 *
 * @ms must have been allocated with net_tx_alloc_mbuf().
 *
 * Returns 0 if successful. If successful, the mbufs will be freed when the
 * transmit completes. Otherwise, the mbufs still belongs to the caller. If
 * ARP doesn't have a cached entry, only the first mbuf will be transmitted
 * when the ARP request resolves.
 */
int net_tx_ip_burst(struct mbuf **ms, int n, uint8_t proto, uint32_t daddr)
{
	struct eth_addr dhost;
	int ret, i;

	assert(n > 0);

	/* prepare the mbufs */
	for (i = 0; i < n; i++) {
		/* prepend the IP header */
		net_push_iphdr(ms[i], proto, daddr);

		/* ask NIC to calculate IP checksum */
		ms[i]->txflags |= OLFLAG_IP_CHKSUM | OLFLAG_IPV4;
	}

	/* apply IP routing */
	daddr = net_get_ip_route(daddr);

	/* use ARP to resolve dhost */
	ret = arp_lookup(daddr, &dhost, ms[0]);
	if (unlikely(ret)) {
		if (ret == -EINPROGRESS) {
			/* ARP code now owns the first mbuf */
			return 0;
		} else {
			/* An unrecoverable error occurred */
			for (i = 0; i < n; i++)
				mbuf_pull_hdr(ms[i], struct ip_hdr);
			return ret;
		}
	}

	/* finally, transmit the packets */
	for (i = 0; i < n; i++) {
		ret = net_tx_eth(ms[i], ETHTYPE_IP, dhost);
		assert(!ret); /* can't fail as implemented so far */
	}

	return 0;
}

/**
 * str_to_netaddr - converts a string to an IPv4 address and port
 * @str: the string to convert
 * @addr: the location to store the parsed address
 *
 * Takes a string like "192.168.1.1:80" or "192.168.1.1" for an ephemeral port.
 *
 * Returns 0 if successful, otherwise -EINVAL if the parsing failed.
 */
int str_to_netaddr(const char *str, struct netaddr *addr)
{
	uint8_t a, b, c, d;
	uint16_t port;

	if(sscanf(str, "%hhu.%hhu.%hhu.%hhu:%hu",
	          &a, &b, &c, &d, &port) != 5) {
		port = 0; /* try with an ephemeral port */
		if (sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4)
			return -EINVAL;
	}

	addr->ip = MAKE_IP_ADDR(a, b, c, d);
	addr->port = port;
	return 0;
}

/**
 * net_init_thread - initializes per-thread state for the network stack
 *
 * Returns 0 (can't fail).
 */
int net_init_thread(void)
{
	tcache_init_perthread(net_tx_buf_tcache, &perthread_get(net_tx_buf_pt));
	return 0;
}


static void net_dump_config(void)
{
	char buf[IP_ADDR_STR_LEN];

	log_info("net: using the following configuration:");
	log_info("  addr:\t%s", ip_addr_to_str(netcfg.addr, buf));
	log_info("  netmask:\t%s", ip_addr_to_str(netcfg.netmask, buf));
	log_info("  gateway:\t%s", ip_addr_to_str(netcfg.gateway, buf));
	log_info("  mac:\t%02X:%02X:%02X:%02X:%02X:%02X",
		 netcfg.mac.addr[0], netcfg.mac.addr[1], netcfg.mac.addr[2],
		 netcfg.mac.addr[3], netcfg.mac.addr[4], netcfg.mac.addr[5]);
}

/**
 * net_init - initializes the network stack
 *
 * Returns 0 if successful.
 */
int net_init(void)
{
	int ret;

	ret = mempool_create(&net_tx_buf_mp, iok.tx_buf, iok.tx_len,
			     PGSIZE_2MB, MBUF_DEFAULT_LEN);
	if (ret)
		return ret;

	net_tx_buf_tcache = mempool_create_tcache(&net_tx_buf_mp,
		"runtime_tx_bufs", TCACHE_DEFAULT_MAG_SIZE);
	if (!net_tx_buf_tcache)
		return -ENOMEM;

	log_info("net: started network stack");
	net_dump_config();
	return 0;
}
