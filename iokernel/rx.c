/*
 * rx.c - the receive path for the I/O kernel (network -> runtimes)
 */

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include <base/log.h>
#include <iokernel/queue.h>
#include <iokernel/shm.h>

#include "defs.h"
#include "sched.h"

#define MBUF_CACHE_SIZE 250
#define RX_PREFETCH_STRIDE 2


/*
 * Prepend rx_net_hdr preamble to ingress packets.
 */
static struct rx_net_hdr *rx_prepend_rx_preamble(struct rte_mbuf *buf)
{
	struct rx_net_hdr *net_hdr;
	uint64_t masked_ol_flags;

	net_hdr = (struct rx_net_hdr *) rte_pktmbuf_prepend(buf,
			(uint16_t) sizeof(*net_hdr));
	RTE_ASSERT(net_hdr != NULL);

	net_hdr->completion_data = (unsigned long)buf;
	net_hdr->len = rte_pktmbuf_pkt_len(buf) - sizeof(*net_hdr);
	net_hdr->rss_hash = buf->hash.rss;
	masked_ol_flags = buf->ol_flags & RTE_MBUF_F_RX_IP_CKSUM_MASK;
	if (masked_ol_flags == RTE_MBUF_F_RX_IP_CKSUM_GOOD)
		net_hdr->csum_type = CHECKSUM_TYPE_UNNECESSARY;
	else
		net_hdr->csum_type = CHECKSUM_TYPE_NEEDED;
	net_hdr->csum = 0; /* unused for now */

	return net_hdr;
}

/**
 * rx_send_to_runtime - enqueues a command to an RXQ for a runtime
 * @p: the runtime's proc structure
 * @hash: the 5-tuple hash for the flow the command is related to
 * @cmd: the command to send
 * @payload: the command payload to send
 *
 * Returns true if the command was enqueued, otherwise a thread is not running
 * and can't be woken or the queue was full.
 */
bool rx_send_to_runtime(struct proc *p, uint32_t hash, uint64_t cmd,
			unsigned long payload)
{
	struct thread *th;

	if (likely(sched_threads_active(p) > 0)) {
		/* use the flow table to route to an active thread */
		th = &p->threads[p->flow_tbl[hash % p->thread_count]];
		thread_enable_sched_poll(th);
		return lrpc_send(&th->rxq, cmd, payload);
	}

	sched_add_core(p);
	if (unlikely(sched_threads_active(p) == 0)) {
		/* enqueue to an idle thread (to be woken later) */
		th = list_top(&p->idle_threads, struct thread, idle_link);
	} else {
		/* use the flow table to route to an active thread */
		th = &p->threads[p->flow_tbl[hash % p->thread_count]];
	}

	thread_enable_sched_poll(th);
	return lrpc_send(&th->rxq, cmd, payload);
}


static bool rx_send_pkt_to_runtime(struct proc *p, struct rx_net_hdr *hdr)
{
	shmptr_t shmptr;

	shmptr = ptr_to_shmptr(&dp.ingress_mbuf_region, hdr, sizeof(*hdr));
	return rx_send_to_runtime(p, hdr->rss_hash, RX_NET_RECV, shmptr);
}

static bool azure_arp_response(struct rte_mbuf *buf)
{
	struct rte_ether_hdr *ptr_mac_hdr;
	struct rte_arp_hdr *arphdr;
	static struct rte_ether_addr azure_eth_addr = {{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}};

	log_debug("sending an arp response");

	ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
	rte_ether_addr_copy(&ptr_mac_hdr->src_addr, &ptr_mac_hdr->dst_addr);
	rte_ether_addr_copy(&azure_eth_addr, &ptr_mac_hdr->src_addr);

	arphdr = rte_pktmbuf_mtod_offset(buf, struct rte_arp_hdr *,
                        sizeof(*ptr_mac_hdr));
	arphdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
	rte_ether_addr_copy(&azure_eth_addr, &arphdr->arp_data.arp_sha);
        rte_ether_addr_copy(&ptr_mac_hdr->dst_addr, &arphdr->arp_data.arp_tha);
	swapvars(arphdr->arp_data.arp_sip, arphdr->arp_data.arp_tip);

	return rte_eth_tx_burst(dp.port, 0, &buf, 1) == 1;
}

static void rx_one_pkt(struct rte_mbuf *buf)
{
	int ret;
	struct proc *p;
	struct rte_arp_hdr *arphdr;
	struct rte_ether_hdr *ptr_mac_hdr;
	struct rte_ether_addr *ptr_dst_addr;
	struct rte_ipv4_hdr *iphdr;
	struct rx_net_hdr *net_hdr;
	uint16_t ether_type;
	uint32_t dst_ip;

	ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
	ptr_dst_addr = &ptr_mac_hdr->dst_addr;
	log_debug("rx: rx packet with MAC %02" PRIx8 " %02" PRIx8 " %02"
		  PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8,
		  ptr_dst_addr->addr_bytes[0], ptr_dst_addr->addr_bytes[1],
		  ptr_dst_addr->addr_bytes[2], ptr_dst_addr->addr_bytes[3],
	  ptr_dst_addr->addr_bytes[4], ptr_dst_addr->addr_bytes[5]);

	ether_type = rte_be_to_cpu_16(ptr_mac_hdr->ether_type);

	if (likely(ether_type == ETHTYPE_IP)) {
		iphdr = rte_pktmbuf_mtod_offset(buf, struct rte_ipv4_hdr *,
			sizeof(*ptr_mac_hdr));
		dst_ip = rte_be_to_cpu_32(iphdr->dst_addr);
	} else if (ether_type == ETHTYPE_ARP) {
		arphdr = rte_pktmbuf_mtod_offset(buf, struct rte_arp_hdr *,
			sizeof(*ptr_mac_hdr));
		dst_ip = rte_be_to_cpu_32(arphdr->arp_data.arp_tip);

		// Azure's faked ARP replies always go to the default NIC
		// address, so broadcast them to all runtimes.
		if (cfg.azure_arp_mode &&
		    arphdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REPLY)) {
			bool success;
			int n_sent = 0;
			net_hdr = rx_prepend_rx_preamble(buf);
			for (int i = 0; i < dp.nr_clients; i++) {
				success = rx_send_pkt_to_runtime(dp.clients[i], net_hdr);
				if (success) {
					n_sent++;
				} else {
					STAT_INC(RX_BROADCAST_FAIL, 1);
					log_debug_ratelimited("rx: failed to enqueue broadcast "
					                      "packet to runtime");
				}
			}
			if (n_sent == 0)
				rte_pktmbuf_free(buf);
			else
				rte_mbuf_refcnt_update(buf, n_sent - 1);
			return;
		}
	} else {
		log_debug("unrecognized ether type");
		goto fail_free;
	}

	/* lookup runtime by IP in hash table */
	ret = rte_hash_lookup_data(dp.ip_to_proc, &dst_ip, (void **)&p);
	if (unlikely(ret < 0)) {

		if (cfg.azure_arp_mode && ether_type == ETHTYPE_ARP &&
		    arphdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST) &&
		    azure_arp_response(buf))
			return;

		STAT_INC(RX_UNREGISTERED_MAC, 1);
		goto fail_free;
	}

	net_hdr = rx_prepend_rx_preamble(buf);
	if (!rx_send_pkt_to_runtime(p, net_hdr)) {
		STAT_INC(RX_UNICAST_FAIL, 1);
		goto fail_free;
	}

	if (unlikely(p->has_directpath)) {
		if (!cfg.azure_arp_mode && ether_type == ETHTYPE_IP)
			log_warn_ratelimited("delivering an IP packet to a directpath runtime");
	}

	return;

fail_free:
	/* anything else */
	log_debug("rx: unhandled packet with MAC %x %x %x %x %x %x",
		 ptr_dst_addr->addr_bytes[0], ptr_dst_addr->addr_bytes[1],
		 ptr_dst_addr->addr_bytes[2], ptr_dst_addr->addr_bytes[3],
		 ptr_dst_addr->addr_bytes[4], ptr_dst_addr->addr_bytes[5]);
	rte_pktmbuf_free(buf);
	STAT_INC(RX_UNHANDLED, 1);
}

/*
 * Process a batch of incoming packets.
 */
bool rx_burst(void)
{
	struct rte_mbuf *bufs[IOKERNEL_RX_BURST_SIZE];
	uint16_t nb_rx, i;

	/* retrieve packets from NIC queue */
	nb_rx = rte_eth_rx_burst(dp.port, 0, bufs, IOKERNEL_RX_BURST_SIZE);
	STAT_INC(RX_PULLED, nb_rx);
	if (nb_rx > 0)
		log_debug("rx: received %d packets on port %d", nb_rx, dp.port);

	for (i = 0; i < nb_rx; i++) {
		if (i + RX_PREFETCH_STRIDE < nb_rx) {
			prefetch(rte_pktmbuf_mtod(bufs[i + RX_PREFETCH_STRIDE],
				 char *));
		}
		rx_one_pkt(bufs[i]);
	}

	return nb_rx > 0;
}

/*
 * Callback to unmap the shared memory used by a mempool when destroying it.
 */
static void rx_mempool_memchunk_free(struct rte_mempool_memhdr *memhdr,
		void *opaque)
{
	mem_unmap_shm(opaque);
}

/*
 * Create and initialize a packet mbuf pool in shared memory, based on
 * rte_pktmbuf_pool_create.
 */
static struct rte_mempool *rx_pktmbuf_pool_create_in_shm(const char *name,
		unsigned n, unsigned cache_size, uint16_t priv_size,
		uint16_t data_room_size, int socket_id)
{
	unsigned elt_size;
	struct rte_pktmbuf_pool_private mbp_priv = {0};
	struct rte_mempool *mp;
	int ret, heap_id;
	size_t pg_size, pg_shift, min_chunk_size, align, len;
	void *shbuf, *heap_area;

	/* create rte_mempool */
	if (RTE_ALIGN(priv_size, RTE_MBUF_PRIV_ALIGN) != priv_size) {
		log_err("rx: mbuf priv_size=%u is not aligned", priv_size);
		goto fail;
	}
	elt_size = sizeof(struct rte_mbuf) + (unsigned) priv_size
			+ (unsigned) data_room_size;
	mbp_priv.mbuf_data_room_size = data_room_size;
	mbp_priv.mbuf_priv_size = priv_size;

	mp = rte_mempool_create_empty(name, n, elt_size, cache_size,
			sizeof(struct rte_pktmbuf_pool_private), socket_id, 0);
	if (mp == NULL)
		goto fail;

	ret = rte_mempool_set_ops_byname(mp, RTE_MBUF_DEFAULT_MEMPOOL_OPS, NULL);
	if (ret != 0) {
		log_err("rx: error setting mempool handler");
		goto fail_free_mempool;
	}
	rte_pktmbuf_pool_init(mp, &mbp_priv);

	/* check necessary size and map shared memory */
	pg_size = PGSIZE_2MB;
	pg_shift = rte_bsf32(pg_size);
	len = rte_mempool_ops_calc_mem_size(mp, n, pg_shift, &min_chunk_size, &align);
	if (len > INGRESS_MBUF_SHM_SIZE) {
		log_err("rx: shared memory is too small for number of mbufs");
		goto fail_free_mempool;
	}

	shbuf = dp.ingress_mbuf_region.base;

	/* hack to make sure that this memory area is registered in DPDK */
	/* use rte_extmem_* and rte_dev_dma_map in the future */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	ret = rte_malloc_heap_create("rx_buf_heap");
	if (ret < 0)
		goto fail_unmap_memory;

	ret = rte_malloc_heap_memory_add("rx_buf_heap", shbuf, INGRESS_MBUF_SHM_SIZE, NULL, 0, PGSIZE_2MB);
	if (ret < 0)
		goto fail_unmap_memory;

	heap_id = rte_malloc_heap_get_socket("rx_buf_heap");
	if (heap_id < 0)
		goto fail_unmap_memory;
#pragma GCC diagnostic pop

	heap_area = rte_malloc_socket(NULL, len, PGSIZE_2MB, heap_id);
	if (!heap_area)
		goto fail_unmap_memory;

	/* populate mempool using shared memory */
	ret = rte_mempool_populate_virt(mp, heap_area, len, pg_size,
			rx_mempool_memchunk_free, heap_area);
	if (ret < 0) {
		log_err("rx: error populating mempool %d", ret);
		goto fail_unmap_memory;
	}

	rte_mempool_obj_iter(mp, rte_pktmbuf_init, NULL);

	return mp;

fail_unmap_memory:
	mem_unmap_shm(shbuf);
fail_free_mempool:
	rte_mempool_free(mp);
fail:
	log_err("rx: couldn't create pktmbuf pool %s", name);
	return NULL;
}

/*
 * Initialize rx state.
 */
int rx_init()
{
	if (cfg.vfio_directpath)
		return 0;

	/* create a mempool in shared memory to hold the rx mbufs */
	dp.rx_mbuf_pool = rx_pktmbuf_pool_create_in_shm("RX_MBUF_POOL",
			IOKERNEL_NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
			rte_socket_id());

	if (dp.rx_mbuf_pool == NULL) {
		log_err("rx: couldn't create rx mbuf pool");
		return -1;
	}

	return 0;
}
