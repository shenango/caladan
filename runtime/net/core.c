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
struct net_driver_ops net_ops;
unsigned int eth_mtu = ETH_DEFAULT_MTU;

/* TX buffer allocation */
static struct mempool net_tx_buf_mp;
static struct tcache *net_tx_buf_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, net_tx_buf_pt);

int net_init_mempool_threads(void)
{
	int i;

	for (i = 0; i < maxks; i++)
		tcache_init_perthread(net_tx_buf_tcache,
			&perthread_get_remote(net_tx_buf_pt, i));

	return 0;
}

int net_init_mempool(void)
{
	int ret;

	ret = mempool_create(&net_tx_buf_mp, iok.tx_buf, iok.tx_len, PGSIZE_2MB,
			     align_up(net_get_mtu() + MBUF_HEAD_LEN + MBUF_DEFAULT_HEADROOM,
				      CACHE_LINE_SIZE * 2));
	if (unlikely(ret))
		return ret;

	net_tx_buf_tcache = mempool_create_tcache(&net_tx_buf_mp,
		"runtime_tx_bufs", TCACHE_DEFAULT_MAG_SIZE);
	if (unlikely(!net_tx_buf_tcache))
		return -ENOMEM;

	return 0;
}


/*
 * RX Networking Functions
 */

/**
 * compute_flow_affinity - compute rss hash for incoming packets
 * @local_port: the local port number
 * @remote: the remote network address
 *
 * Returns the 32 bit hash mod maxks
 *
 * copied from dpdk/lib/librte_hash/rte_thash.h
 */
static uint32_t compute_flow_affinity(uint8_t ipproto, uint16_t local_port, struct netaddr remote)
{
	log_warn_ratelimited("flow affinity not enabled for iokernel datapath");
	return 0;
	const uint8_t *rss_key = iok.iok_info->rss_key;

	uint32_t i, j, map, ret = 0, input_tuple[] = {
		remote.ip, netcfg.addr, local_port | remote.port << 16
	};

	for (j = 0; j < ARRAY_SIZE(input_tuple); j++) {
		for (map = input_tuple[j]; map;	map &= (map - 1)) {
			i = (uint32_t)__builtin_ctz(map);
			ret ^= hton32(((const uint32_t *)rss_key)[j]) << (31 - i) |
					(uint32_t)((uint64_t)(hton32(((const uint32_t *)rss_key)[j + 1])) >>
					(i + 1));
		}
	}

	return ret % (uint32_t)maxks;
}

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
	m = smalloc(hdr->len + MBUF_HEAD_LEN);
	if (unlikely(!m))
		goto out;

	buf = (unsigned char *)m + MBUF_HEAD_LEN;

	/* copy the payload and release the buffer back to the iokernel */
	memcpy(buf, hdr->payload, hdr->len);

	mbuf_init(m, buf, hdr->len, 0);
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

static void net_rx_one(struct mbuf *m)
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
		return;
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
		net_rx_trans(m);
		break;

	case IPPROTO_DIRECTPATH_ARP_ENCAP:
		net_rx_arp(m);
		break;

	default:
		goto drop;
	}

	return;

drop:
	log_warn_ratelimited("dropping");
	mbuf_drop(m);
}

/**
 * net_rx_batch - handles a batch of ingress packets
 * @ms: an array of ingress packets
 * @nr: the size of the @ms array
 */
void net_rx_batch(struct mbuf **ms, unsigned int nr)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (i + RX_PREFETCH_STRIDE < nr)
			prefetch(ms[i + RX_PREFETCH_STRIDE]->data);
		net_rx_one(ms[i]);
	}
}

static void iokernel_softirq_poll(struct kthread *k)
{
	struct rx_net_hdr *hdr;
	struct mbuf *m;
	uint64_t cmd;
	unsigned long payload;

	while (true) {
		if (!lrpc_recv(&k->rxq, &cmd, &payload))
			break;

		switch (cmd) {
		case RX_NET_RECV:
			hdr = shmptr_to_ptr(&netcfg.rx_region,
					    (shmptr_t)payload,
					    MBUF_DEFAULT_LEN);
			m = net_rx_alloc_mbuf(hdr);
			if (unlikely(!m)) {
				STAT(DROPS)++;
				continue;
			}
			net_rx_one(m);
			break;

		case RX_NET_COMPLETE:
			mbuf_free((struct mbuf *)payload);
			break;

		case RX_REFILL_BUFS:
			BUG_ON(!net_ops.trigger_rx_refill);
			net_ops.trigger_rx_refill();
			break;

		default:
			panic("net: invalid RXQ cmd '%ld'", cmd);
		}
	}
}

static void iokernel_softirq(void *arg)
{
	struct kthread *k = arg;

	while (true) {
		iokernel_softirq_poll(k);
		preempt_disable();
		k->iokernel_busy = false;
		thread_park_and_preempt_enable();
	}
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
	tcache_free(perthread_ptr(net_tx_buf_pt), m);
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
	m = tcache_alloc(perthread_ptr(net_tx_buf_pt));
	if (unlikely(!m)) {
		preempt_enable();
		log_warn_ratelimited("net: out of tx buffers");
		return NULL;
	}
	preempt_enable();

	buf = (unsigned char *)m + MBUF_HEAD_LEN;
	mbuf_init(m, buf, net_get_mtu(), MBUF_DEFAULT_HEADROOM);
	m->csum_type = CHECKSUM_TYPE_NEEDED;
	m->txflags = 0;
	m->release_data = 0;
	m->release = net_tx_release_mbuf;
	return m;
}

/* drains overflow queues */
int __noinline net_tx_drain_overflow(void)
{
	struct mbuf *m;
	struct kthread *k = myk();

	assert_preempt_disabled();

	/* drain TX packets */
	while (!mbufq_empty(&k->txpktq_overflow)) {

		if (unlikely(preempt_cede_needed(k)))
			return 1;

		m = mbufq_peak_head(&k->txpktq_overflow);
		if (net_ops.tx_single(m))
			return 1;
		mbufq_pop_head(&k->txpktq_overflow);
	}

	return 0;

}

static int net_tx_iokernel(struct mbuf *m)
{
	struct kthread *k = myk();
	unsigned int len = mbuf_length(m);
	struct tx_net_hdr *hdr;

	assert_preempt_disabled();

	hdr = mbuf_push_hdr(m, *hdr);
	hdr->completion_data = (unsigned long)m;
	hdr->len = len;
	hdr->olflags = m->txflags;
	shmptr_t shm = ptr_to_shmptr(&netcfg.tx_region, hdr, len + sizeof(*hdr));

	if (unlikely(!lrpc_send(&k->txpktq, TXPKT_NET_XMIT, shm))) {
		mbuf_pull_hdr(m, *hdr);
		return -1;
	}

	return 0;
}

static void net_tx_raw(struct mbuf *m)
{
	struct kthread *k;
	unsigned int len = mbuf_length(m);

	k = getk();

	STAT(TX_PACKETS)++;
	STAT(TX_BYTES) += len;

	/* drain pending overflow packets first */
	if (unlikely(!mbufq_empty(&k->txpktq_overflow))) {
		if (net_tx_drain_overflow()) {
			mbufq_push_tail(&k->txpktq_overflow, m);
			STAT(TXQ_OVERFLOW)++;
			putk();
			return;
		}
	}


	if (unlikely(net_ops.tx_single(m))) {
		mbufq_push_tail(&k->txpktq_overflow, m);
		STAT(TXQ_OVERFLOW)++;
	}

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
 */
void net_tx_eth(struct mbuf *m, uint16_t type, struct eth_addr dhost)
{
	struct eth_hdr *eth_hdr;

	eth_hdr = mbuf_push_hdr(m, *eth_hdr);
	eth_hdr->shost = netcfg.mac;
	eth_hdr->dhost = dhost;
	eth_hdr->type = hton16(type);
	net_tx_raw(m);
}

static void net_push_iphdr(struct mbuf *m, uint8_t proto, uint32_t daddr)
{
	struct ip_hdr *iphdr;

	/* populate IP header */
	iphdr = mbuf_push_hdr(m, *iphdr);
	iphdr->version = IPVERSION;
	iphdr->header_len = 5;
	iphdr->tos = IPTOS_DSCP_CS0 | IPTOS_ECN_NOTECT;
	iphdr->len = hton16(mbuf_length(m));
	iphdr->id = 0; /* see RFC 6864 */
	iphdr->off = hton16(IP_DF);
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

static int net_tx_local_loopback(struct mbuf *m_in, uint8_t proto)
{
	int ret;
	struct mbuf *m;
	void *buf;

	/* allocate the buffer to store the payload */
	m = smalloc(mbuf_length(m_in) + MBUF_HEAD_LEN);
	if (unlikely(!m))
		return -ENOMEM;

	buf = (unsigned char *)m + MBUF_HEAD_LEN;

	/* copy the payload and release the buffer */
	memcpy(buf, mbuf_data(m_in), mbuf_length(m_in));

	mbuf_init(m, buf, mbuf_length(m_in), 0);
	m->len = mbuf_length(m_in);
	m->csum_type = CHECKSUM_TYPE_UNNECESSARY;
	m->release = (void (*)(struct mbuf *))sfree;

	mbuf_mark_network_offset(m);
	mbuf_pull_hdr(m, struct ip_hdr);
	switch(proto) {
		case IPPROTO_UDP:
		case IPPROTO_TCP:
			/* spawn a thread to handle RX, caller may hold a lock */
			ret = thread_spawn((void (*)(void *))net_rx_trans, m);
			if (unlikely(ret)) {
				log_err_ratelimited("failed to spawn loopback thread");
				mbuf_drop(m);
			}
			break;
		default:
			/* don't support ping etc for now */
			mbuf_drop(m);
			break;
	}
	mbuf_free(m_in);
	return 0;
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
	mbuf_mark_network_offset(m);

	/* route loopbacks */
	if (daddr == netcfg.addr)
		return net_tx_local_loopback(m, proto);

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

	net_tx_eth(m, ETHTYPE_IP, dhost);
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
	for (i = 0; i < n; i++)
		net_tx_eth(ms[i], ETHTYPE_IP, dhost);

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
	struct kthread *k = myk();
	thread_t *th;

	th = thread_create(iokernel_softirq, k);
	if (!th)
		return -ENOMEM;

	k->iokernel_softirq = th;

	if (!cfg_directpath_external())
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
	log_info("  mac:\t\t%02X:%02X:%02X:%02X:%02X:%02X",
		 netcfg.mac.addr[0], netcfg.mac.addr[1], netcfg.mac.addr[2],
		 netcfg.mac.addr[3], netcfg.mac.addr[4], netcfg.mac.addr[5]);
	log_info("  mtu:\t\t%d", net_get_mtu());
}

static int
register_flow_iokernel(unsigned int affininty, struct trans_entry *e,
		       void **handle_out)
{
	return 0;
}

static int deregister_flow_iokernel(struct trans_entry *e, void *handle)
{
	return 0;
}

static struct net_driver_ops iokernel_ops = {
	.tx_single = net_tx_iokernel,
	.steer_flows = NULL,
	.register_flow =  register_flow_iokernel,
	.deregister_flow = deregister_flow_iokernel,
	.get_flow_affinity = compute_flow_affinity,
};


/**
 * net_init - initializes the network stack
 *
 * Returns 0 if successful.
 */
int net_init(void)
{
	log_info("net: started network stack");
	net_dump_config();

	if (!cfg_directpath_enabled())
		net_ops = iokernel_ops;

	if (cfg_directpath_external())
		return 0;

	return net_init_mempool();
}
