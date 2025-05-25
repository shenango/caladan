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
#include <net/chksum.h>
#include <runtime/net.h>
#include <runtime/smalloc.h>

#include "defs.h"

#define IP_ID_SEED	0x42345323
#define RX_PREFETCH_STRIDE 2

/* important global state */
struct net_cfg netcfg __aligned(CACHE_LINE_SIZE);
struct net_driver_ops net_ops;
unsigned int eth_mtu = ETH_DEFAULT_MTU;
static size_t extra_headroom;

/* TX buffer allocation */
static struct mempool net_tx_buf_mp;
static struct tcache *net_tx_buf_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, net_tx_buf_pt);

#ifdef SPLIT_TX
/* Pool of buffers for small TX packets */
static struct mempool net_tx_buf_sm_mp;
static struct tcache *net_tx_buf_sm_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, net_tx_buf_sm_pt);
#endif

/* slab allocator for mbuf structs */
static struct slab mbuf_slab;
static struct tcache *mbuf_tcache;
DEFINE_PERTHREAD(struct tcache_perthread, mbuf_pt);

struct tx_completion_data {
	struct mbuf	*m;
	char		payload[];	/* packet data */
} __attribute__((__packed__));

int net_init_mempool_threads(void)
{
	int i;

	for (i = 0; i < maxks; i++) {
		tcache_init_perthread(net_tx_buf_tcache,
			&perthread_get_remote(net_tx_buf_pt, i));

#ifdef SPLIT_TX
		tcache_init_perthread(net_tx_buf_sm_tcache,
			&perthread_get_remote(net_tx_buf_sm_pt, i));
#endif
	}

	return 0;
}

size_t calculate_egress_buf_size(void)
{
	size_t iok_headroom = 0;
	if (!cfg_directpath_enabled())
		iok_headroom = align_up(sizeof(struct tx_completion_data),
			                CACHE_LINE_SIZE);
	return align_up(iok_headroom + net_get_mtu() + eth_headroom() + MBUF_HEAD_LEN,
	                CACHE_LINE_SIZE * 2);
}

int net_init_mempool(void)
{
	int ret;
	size_t pool_sz = iok.tx_len;
#ifdef SPLIT_TX
	pool_sz /= 2;
#endif
	if (!cfg_directpath_enabled())
		extra_headroom = align_up(sizeof(struct tx_completion_data),
			                  CACHE_LINE_SIZE);

	ret = mempool_create(&net_tx_buf_mp, iok.tx_buf, pool_sz, PGSIZE_2MB,
			     calculate_egress_buf_size());
	if (unlikely(ret))
		return ret;

	net_tx_buf_tcache = mempool_create_tcache(&net_tx_buf_mp,
		"runtime_tx_bufs", TCACHE_DEFAULT_MAG_SIZE);
	if (unlikely(!net_tx_buf_tcache))
		return -ENOMEM;

#ifdef SPLIT_TX
	ret = mempool_create(&net_tx_buf_sm_mp, iok.tx_buf + pool_sz, pool_sz,
			     PGSIZE_2MB,
			     align_up(extra_headroom + SMALL_BUF_SIZE,
			              CACHE_LINE_SIZE));
	if (unlikely(ret))
		return ret;

	net_tx_buf_sm_tcache = mempool_create_tcache(&net_tx_buf_sm_mp,
		"runtime_tx_sm_bufs", TCACHE_DEFAULT_MAG_SIZE);
	if (unlikely(!net_tx_buf_sm_tcache))
		return -ENOMEM;
#endif

	return 0;
}


/*
 * RX Networking Functions
 */

static uint32_t do_toeplitz(uint32_t saddr, uint32_t daddr, uint16_t sport,
	                    uint16_t dport)
{
	const uint8_t *rss_key = iok.iok_info->rss_key;

	uint32_t i, j, map, ret = 0, input_tuple[] = {
		saddr, daddr, dport | sport << 16
	};

	for (j = 0; j < ARRAY_SIZE(input_tuple); j++) {
		for (map = input_tuple[j]; map;	map &= (map - 1)) {
			i = (uint32_t)__builtin_ctz(map);
			ret ^= hton32(((const uint32_t *)rss_key)[j]) << (31 - i) |
					(uint32_t)((uint64_t)(hton32(((const uint32_t *)rss_key)[j + 1])) >>
					(i + 1));
		}
	}

	return ret;
}

/**
 * compute_flow_affinity - compute rss hash for incoming packets
 * @local_port: the local port number
 * @remote: the remote network address
 *
 * Returns the 32 bit hash mod maxks
 *
 * copied from dpdk/lib/librte_hash/rte_thash.h
 */
static uint32_t compute_flow_affinity(uint8_t ipproto, uint16_t local_port,
	                              struct netaddr remote)
{
	uint32_t ret = do_toeplitz(remote.ip, netcfg.addr, remote.port,
		                   local_port);
	return ret % (uint32_t)maxks;
}

static void net_rx_send_completion(shmptr_t data)
{
	struct kthread *k;

	union txcmdq_cmd cmd;
	cmd.reserved = 0;
	cmd.txcmd = TXCMD_NET_COMPLETE;

	k = getk();
	if (unlikely(!lrpc_send(&k->txcmdq, cmd.lrpc_cmd, data))) {
		WARN();
	}
	putk();
}



static struct mbuf *net_rx_alloc_mbuf(shmptr_t data, union rxq_cmd cmd)
{
	struct mbuf *m;
	void *buf;
	const void *src_buf;

	src_buf = shmptr_to_ptr(&netcfg.rx_region, data, cmd.len);

	/* allocate the buffer to store the payload */
	m = smalloc(cmd.len + MBUF_HEAD_LEN);
	if (unlikely(!m))
		goto out;

	buf = (unsigned char *)m + MBUF_HEAD_LEN;

	/* copy the payload and release the buffer back to the iokernel */
	memcpy(buf, src_buf, cmd.len);

	mbuf_init(m, buf, cmd.len, 0);
	m->len = cmd.len;
	m->csum_type = cmd.csum_type;
	m->release = (void (*)(struct mbuf *))sfree;

out:
	net_rx_send_completion(data);
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

static void handle_tx_completion(unsigned long payload)
{
	struct tx_completion_data *compl;
	char *buf;

	buf = shmptr_to_ptr(&netcfg.tx_region, payload, 0);
	compl = container_of_nocheck(buf, struct tx_completion_data, payload);
	mbuf_free(compl->m);
}

static void iokernel_softirq_poll(struct kthread *k)
{
	struct mbuf *m;
	union rxq_cmd cmd;
	unsigned long payload;

	while (true) {
		if (!lrpc_recv(&k->rxq, &cmd.lrpc_cmd, &payload))
			break;

		switch (cmd.rxcmd) {
		case RX_NET_RECV:
			m = net_rx_alloc_mbuf(payload, cmd);
			if (unlikely(!m)) {
				STAT(DROPS)++;
				continue;
			}
			net_rx_one(m);
			break;

		case RX_NET_COMPLETE:
			handle_tx_completion(payload);
			break;

		case RX_REFILL_BUFS:
			BUG_ON(!net_ops.trigger_rx_refill);
			net_ops.trigger_rx_refill();
			break;

		default:
			panic("net: invalid RXQ cmd '%hu'", cmd.rxcmd);
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
#ifdef SPLIT_TX
	bool lg = mempool_member(&net_tx_buf_mp, m);

	preempt_disable();
	if (lg) {
		tcache_free(perthread_ptr(net_tx_buf_pt), m);
	} else {
		tcache_free(perthread_ptr(net_tx_buf_sm_pt), m->head);
		tcache_free(perthread_ptr(mbuf_pt), m);
	}
	preempt_enable();
#else
	preempt_disable();
	tcache_free(perthread_ptr(net_tx_buf_pt), m);
	preempt_enable();
#endif
}

/**
 * net_tx_alloc_mbuf - allocates an mbuf for transmitting.
 *
 * Returns an mbuf, or NULL if out of memory.
 */
struct mbuf *net_tx_alloc_mbuf(size_t header_len)
{
	struct mbuf *m;
	unsigned char *buf;

	preempt_disable();
	m = tcache_alloc(perthread_ptr(net_tx_buf_pt));
	if (unlikely(!m)) {
		log_warn_ratelimited("net: out of tx buffers");
		preempt_enable();
		return NULL;
	}
	preempt_enable();

	buf = (unsigned char *)m + MBUF_HEAD_LEN;
	// Set headroom so completion header is likely not on the same cache
	// line as the payload.
	mbuf_init(m, buf, net_get_mtu() + eth_headroom() + extra_headroom,
		  extra_headroom + header_len);
	m->txflags = 0;
	m->release = net_tx_release_mbuf;
	return m;
}

#ifdef SPLIT_TX
struct mbuf *net_tx_alloc_mbuf_small(size_t header_len)
{
	struct mbuf *m;
	unsigned char *buf;

	preempt_disable();
	buf = tcache_alloc(perthread_ptr(net_tx_buf_sm_pt));
	if (unlikely(!buf)) {
		log_warn_ratelimited("net: out of tx buffers");
		preempt_enable();
		return NULL;
	}

	m = tcache_alloc(perthread_ptr(mbuf_pt));
	if (unlikely(!m)) {
		tcache_free(perthread_ptr(net_tx_buf_sm_pt), buf);
		log_warn_ratelimited("net: out of mbufs");
		preempt_enable();
		return NULL;
	}

	preempt_enable();

	mbuf_init(m, buf, SMALL_BUF_SIZE + extra_headroom,
		  header_len + extra_headroom);
	m->txflags = 0;
	m->release = net_tx_release_mbuf;
	return m;
}
#endif

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
	struct tx_completion_data *compl;
	union txpkt_xmit_cmd cmd;
	shmptr_t shm;
	uint64_t payload;
	uint16_t len = mbuf_length(m);


	if (netcfg.min_pkt_size && len < netcfg.min_pkt_size) {
		unsigned char *pad = mbuf_put(m, netcfg.min_pkt_size - len);
		memset(pad, 0, netcfg.min_pkt_size - len);
		len = netcfg.min_pkt_size;
	}

	cmd.dst_ip = m->tx_dst_ip;
	cmd.txcmd = TXPKT_NET_XMIT;
	cmd.len = len;
	shm = ptr_to_shmptr(&netcfg.tx_region, mbuf_data(m), cmd.len);
	cmd.olflags = m->txflags;
	payload = txpkt_to_payload(shm, (uint16_t)m->hash);

	assert_preempt_disabled();

	/* Record completion information just prior to the payload */
	compl = mbuf_push_hdr(m, *compl);
	compl->m = m;

	if (unlikely(!lrpc_send(&myk()->txpktq, cmd.lrpc_cmd, payload))) {
		mbuf_pull_hdr(m, *compl);
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
 * @is_local: this packet is destinated for an application on this machine
 *
 * The payload must start with the network (L3) header. The ethernet (L2)
 * header will be prepended by this function.
 *
 * @m must have been allocated with net_tx_alloc_mbuf().
 */
void net_tx_eth(struct mbuf *m, uint16_t type, const struct eth_addr *dhost, bool is_local)
{
	struct eth_hdr *eth_hdr;
	eth_hdr = mbuf_push_hdr(m, *eth_hdr);
	eth_hdr->shost = netcfg.mac;
	eth_hdr->dhost = *dhost;
	eth_hdr->type = hton16(type);
	m->txflags |= is_local ? TXFLAG_LOCAL : 0;
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

	if (netcfg.no_tx_offloads)
		iphdr->chksum = ipv4_cksum(iphdr);
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
	bool local;

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
	ret = arp_lookup(daddr, &dhost, m, &local);
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

	/* add hints for loopback via IOKernel */
	if (local && !cfg_directpath_enabled()) {
		mbuf_mark_dst_ip(m, daddr);
		m->hash = do_toeplitz(netcfg.addr, daddr, m->tx_l4_sport,
			              m->tx_l4_dport);
		m->txflags |= TXFLAG_LOCAL_HINT;
	}

	net_tx_eth(m, ETHTYPE_IP, &dhost, local);
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

	tcache_init_perthread(mbuf_tcache, &perthread_get(mbuf_pt));

	if (!cfg_directpath_external()) {
		tcache_init_perthread(net_tx_buf_tcache, &perthread_get(net_tx_buf_pt));
#ifdef SPLIT_TX
		tcache_init_perthread(net_tx_buf_sm_tcache, &perthread_get(net_tx_buf_sm_pt));
#endif
	}

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
	size_t sz;
	int ret;

	sz = sizeof(struct mbuf) + MBUF_INL_DATA_SZ;
	ret = slab_create(&mbuf_slab, "mbufs", sz, 0);
	if (ret)
		return ret;

	mbuf_tcache = slab_create_tcache(&mbuf_slab, TCACHE_DEFAULT_MAG_SIZE);
	if (!mbuf_tcache)
		return -ENOMEM;

	log_info("net: started network stack");
	net_dump_config();

	if (!cfg_directpath_enabled()) {
		net_ops = iokernel_ops;
		netcfg.no_tx_offloads = iok.iok_info->no_tx_offloads;
		netcfg.min_pkt_size = iok.iok_info->min_pkt_size;
		log_warn("****************************************************************************************");
		log_warn("*                           WARNING: DIRECTPATH DISABLED                               *");
		log_warn("*                                                                                      *");
		log_warn("*                        DO NOT USE FOR PERFORMANCE BENCHMARKS                         *");
		log_warn("*                                                                                      *");
		log_warn("****************************************************************************************");
	}

	if (cfg_directpath_external())
		return 0;

	return net_init_mempool();
}
