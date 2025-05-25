/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * dpdk.c - DPDK initialization for the iokernel dataplane
 */

#include <inttypes.h>
#include <rte_bus.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>

#include <base/log.h>

#include "defs.h"
#include "sched.h"

#define RX_RING_SIZE 256
#define TX_RING_SIZE 256

#define IOKERNEL_MTU 1500

#define MLX5_RX_RING_SIZE 2048
#define MLX5_TX_RING_SIZE 2048

char *nic_pci_addr_str;
struct pci_addr nic_pci_addr;

char **dpdk_argv;
int dpdk_argc;

struct rte_eth_rss_conf rss_conf;
bool rss_conf_present;

#define DPDK_PORT 0

static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.mtu = IOKERNEL_MTU,
		.offloads = RTE_ETH_RX_OFFLOAD_IPV4_CKSUM,
		.mq_mode = RTE_ETH_MQ_RX_RSS | RTE_ETH_MQ_RX_RSS_FLAG,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV4_UDP,
		},
	},
	.txmode = {
		.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM,
	},
};

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int dpdk_port_init(uint8_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	char eth_name[RTE_ETH_NAME_MAX_LEN];
	const uint16_t rx_rings = 1, tx_rings = 1;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;
	struct rte_eth_rxconf *rxconf;

	/* Get default device configuration */
	rte_eth_dev_info_get(port, &dev_info);
	rxconf = &dev_info.default_rxconf;
	rxconf->rx_free_thresh = 64;

	bool is_mlx5 =
	       !strncmp(dev_info.driver_name, "mlx5_pci",
	                strlen("mlx5_pci")) ||
	       !strncmp(dev_info.driver_name, "net_mlx5", strlen("net_mlx5"));

	if (is_mlx5) {
		nb_rxd = MLX5_RX_RING_SIZE;
		nb_txd = MLX5_TX_RING_SIZE;
		port_conf.lpbk_mode = 1;
		dataplane_mode = IOK_NET_MODE_DPDK_MLX5;
	}

	bool is_tap =
	       !strncmp(dev_info.driver_name, "net_tap", strlen("net_tap"));

	cfg.allow_loopback = true;

	// Enable internal loopbacking if using TAP.
	// NOTE: other drivers likely need this enabled also.
	if (is_tap) {
		// the TAP driver copies the buffer before sending to emulate TX
		// offloads. This is broken on recent versions that try to
		// allocate a new buffer from the same mempool, since our TX
		// mempool only attaches to external buffers.
		cfg.tx_offloads_disabled = iok_info->no_tx_offloads = true;
		dataplane_mode = IOK_NET_MODE_DPDK_TAP;
	}

	// bnxt requires packets to be at least 52 bytes.
	if (!strncmp(dev_info.driver_name, "net_bnxt", strlen("net_bnxt")))
		iok_info->min_pkt_size = 52;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port), rxconf, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Enable TX offloading */
	txconf = &dev_info.default_txconf;
	txconf->tx_rs_thresh = 64;
	txconf->tx_free_thresh = 64;

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port), txconf);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct rte_ether_addr addr;
	rte_eth_macaddr_get(port, &addr);
	if (is_tap) {
		// When joining the TAP device to a Linux bridge interface,
		// traffic from some sources are not properly delivered to the
		// TAP device unless the MAC address is different from the
		// assigned one.
		addr.addr_bytes[5] += 1;
	}
	memcpy(&iok_info->host_mac, &addr, sizeof(iok_info->host_mac));
	log_info("dpdk: driver: %s port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			" %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "",
			dev_info.driver_name, (unsigned)port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	/* record the PCI address */
	if (!nic_pci_addr_str && dev_info.device &&
	    rte_bus_find_by_device(dev_info.device) &&
	    rte_eth_dev_get_name_by_port(port, eth_name) == 0 &&
	    pci_str_to_addr(eth_name, &nic_pci_addr) == 0) {
		nic_pci_addr_str = "";
		memcpy(&iok_info->directpath_pci, &nic_pci_addr, sizeof(nic_pci_addr));
	}

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);

	/* record the RSS hash key */
	rss_conf.rss_key = iok_info->rss_key;
	rss_conf.rss_key_len = ARRAY_SIZE(iok_info->rss_key);
	retval = rte_eth_dev_rss_hash_conf_get(port, &rss_conf);
	if (retval == 0) {
		rss_conf_present = true;
		iok_info->rss_key_len = rss_conf.rss_key_len;
	} else if (!is_tap) {
		log_warn("Couldn't query RSS parameters");
	}
	return 0;
}

/*
 * Log some ethernet port stats.
 */
void dpdk_print_eth_stats(void)
{
	int ret;
	struct rte_eth_stats stats;

	ret = rte_eth_stats_get(dp.port, &stats);
	if (ret)
		log_debug("dpdk: error getting eth stats");


	fprintf(stderr, "eth stats for port %d at time %"PRIu64"\n", dp.port, microtime());
	fprintf(stderr, "RX-packets: %"PRIu64" RX-dropped: %"PRIu64" RX-bytes: %"PRIu64"\n",
			stats.ipackets, stats.imissed, stats.ibytes);
	fprintf(stderr,"TX-packets: %"PRIu64" TX-bytes: %"PRIu64"\n", stats.opackets,
			stats.obytes);
	fprintf(stderr,"RX-error: %"PRIu64" TX-error: %"PRIu64" RX-mbuf-fail: %"PRIu64"\n",
			stats.ierrors, stats.oerrors, stats.rx_nombuf);
}

/*
 * Initialize dpdk, must be done as soon as possible.
 */
int dpdk_init(void)
{
	unsigned int max_args;
	char buf[10], **argv;
	int i, ret, argc = 0;
	struct rte_eth_dev_info dev_info;

	max_args = 7 + dpdk_argc;
	argv = malloc(max_args * sizeof(char *));
	if (!argv)
		return -ENOMEM;

#define ARGV(strval)               \
	({                          \
		BUG_ON(argc == max_args); \
		argv[argc++] = (strval);  \
	})

	/* init args */
	ARGV("./iokerneld");
	ARGV("-l");
	/* use our assigned core */
	sprintf(buf, "%d", sched_dp_core);
	ARGV(buf);
	ARGV("--socket-mem=128");

	if (cfg.vfio_directpath) {
		ARGV("--vdev=net_tap0");
		ARGV("--allow");
		ARGV("0000:00:00.0");
	} else if (cfg.azure_arp_mode) {
		ARGV("--allow");
		ARGV("0000:00:00.0");
	} else if (nic_pci_addr_str) {
		ARGV("--allow");
		ARGV(nic_pci_addr_str);
	} else {
		ARGV("--vdev=net_tap0");
	}

	/* include any user-supplied arguments */
	for (i = 0; i < dpdk_argc; i++)
		ARGV(dpdk_argv[i]);

#undef ARGV

	/* initialize the Environment Abstraction Layer (EAL) */
	ret = rte_eal_init(argc, argv);
	free(argv);
	if (ret < 0) {
		log_err("dpdk: error with EAL initialization");
		return -1;
	}

	/* check that there is a port to send/receive on */
	if (!cfg.vfio_directpath && !rte_eth_dev_is_valid_port(DPDK_PORT)) {
		log_err("dpdk: no available ports");
		return -1;
	}

	if (rte_lcore_count() > 1)
		log_warn("dpdk: too many lcores enabled, only 1 used");

	if (!cfg.vfio_directpath) {
		dp.port = DPDK_PORT;
		rte_eth_dev_info_get(DPDK_PORT, &dev_info);
		dp.device = dev_info.device;
		dp.iova_mode_pa = rte_eal_iova_mode() == RTE_IOVA_PA;
	}


	return 0;
}

int do_dpdk_dma_map(void *buf, size_t len, size_t pgsize, uintptr_t *physaddrs)
{
	int ret;
	rte_iova_t iova;
	size_t i, nr_pages;
	void *pg;

	ret = rte_extmem_register(buf, len, NULL, 0, pgsize);
	if (unlikely(ret < 0)) {
		log_err("failed to register extmem %s", strerror(rte_errno));
		return -rte_errno;
	}

	if (!dp.iova_mode_pa) {
		// In IOVA VA mode, IOVAs are equal to VAs.
		ret = rte_dev_dma_map(dp.device, buf, (rte_iova_t)buf, len);
		if (unlikely(ret < 0)) {
			if (rte_errno == EOPNOTSUPP)
				return 0;
			ret = -rte_errno;
			goto fail_dma_map;
		}
		return 0;
	}

	// In PA mode, IOVAs are equal to PA and are not contiguous.
	nr_pages = div_up(len, pgsize);
	for (i = 0; i < nr_pages; i++) {
		pg = buf + i * pgsize;
		iova = physaddrs ? physaddrs[i] : rte_mem_virt2iova(pg);
		ret = rte_dev_dma_map(dp.device, pg, iova, pgsize);
		if (unlikely(ret < 0)) {
			if (rte_errno == EOPNOTSUPP)
				break;
			ret = -rte_errno;
			for (size_t j = 0; j < i; j++) {
				pg = buf + j * pgsize;
				iova = physaddrs ? physaddrs[j] : rte_mem_virt2iova(pg);
				rte_dev_dma_unmap(dp.device, pg, iova, pgsize);
			}
			goto fail_dma_map;
		}
	}

	return 0;

fail_dma_map:
	rte_extmem_unregister(buf, len);
	log_err("failed to map DMA memory: %s", strerror(-ret));
	assert(ret);
	return ret;
}

void do_dpdk_dma_unmap(void *buf, size_t len, size_t pgsize, uintptr_t *physaddrs)
{
	int ret;
	rte_iova_t iova;
	size_t i, nr_pages;
	void *pg;

	if (!dp.iova_mode_pa) {
		// In IOVA VA mode, IOVAs are equal to VAs.
		ret = rte_dev_dma_unmap(dp.device, buf, (rte_iova_t)buf, len);
		if (unlikely(ret < 0 && rte_errno != EOPNOTSUPP))
			log_err("failed dma unmap %s", strerror(rte_errno));
		goto deregister;
	}

	// In PA mode, IOVAs are equal to PA and are not contiguous.
	nr_pages = div_up(len, pgsize);
	for (i = 0; i < nr_pages; i++) {
		pg = buf + i * pgsize;
		iova = physaddrs ? physaddrs[i] : rte_mem_virt2iova(pg);
		ret = rte_dev_dma_unmap(dp.device, pg, iova, pgsize);
		if (ret < 0) {
			if (rte_errno != EOPNOTSUPP)
				log_err("failed dma unmap %s", strerror(rte_errno));
			goto deregister;
		}
	}

deregister:
	ret = rte_extmem_unregister(buf, len);
	if (unlikely(ret < 0))
		log_err("failed to unregister extmem %s", strerror(rte_errno));
}

/*
 * Additional dpdk initialization that must be done after rx init.
 */
int dpdk_late_init(void)
{

	if (cfg.vfio_directpath) {
		dataplane_mode = IOK_NET_MODE_VFIO_MLX5;
		return 0;
	}

	/* initialize port */
	if (dpdk_port_init(dp.port, dp.rx_mbuf_pool) != 0) {
		log_err("dpdk: cannot init port %"PRIu8 "\n", dp.port);
		return -1;
	}

	return 0;
}
