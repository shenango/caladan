#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <rte_arp.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#define RX_RING_SIZE 128
#define TX_RING_SIZE 128

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define MAX_CORES 64
#define UDP_MAX_PAYLOAD 1472
#define MAX_SAMPLES (100*1000*1000)
#define RANDOM_US 10

static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.max_rx_pkt_len = ETHER_MAX_LEN,
		.offloads = DEV_RX_OFFLOAD_IPV4_CKSUM,
		.mq_mode = ETH_MQ_RX_RSS,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_UDP,
		},
	},
	.txmode = {
		.offloads = DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_UDP_CKSUM,
	},
};

uint32_t kMagic = 0x6e626368; // 'nbch'

struct nbench_req {
  uint32_t magic;
  int nports;
};

struct nbench_resp {
  uint32_t magic;
  int nports;
  uint16_t ports[];
};

enum {
	MODE_UDP_CLIENT = 0,
	MODE_UDP_SERVER,
};

#define MAKE_IP_ADDR(a, b, c, d)			\
	(((uint32_t) a << 24) | ((uint32_t) b << 16) |	\
	 ((uint32_t) c << 8) | (uint32_t) d)

static unsigned int dpdk_port = 0;
static uint8_t mode;
struct rte_mempool *rx_mbuf_pool;
struct rte_mempool *tx_mbuf_pool;
static struct ether_addr my_eth;
static uint32_t my_ip;
static uint32_t server_ip;
static int seconds;
static size_t payload_len;
static unsigned int interval_us;
static unsigned int client_port;
static unsigned int server_port;
static unsigned int num_queues = 1;
struct ether_addr zero_mac = {
		.addr_bytes = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0}
};
struct ether_addr broadcast_mac = {
		.addr_bytes = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
};
uint16_t next_port = 50000;
static uint64_t snd_times[MAX_SAMPLES];
static uint64_t rcv_times[MAX_SAMPLES];
char *output_filename = NULL;

/* dpdk_netperf.c: simple implementation of netperf on DPDK */

static int str_to_ip(const char *str, uint32_t *addr)
{
	uint8_t a, b, c, d;
	if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) {
		return -EINVAL;
	}

	*addr = MAKE_IP_ADDR(a, b, c, d);
	return 0;
}

static int str_to_long(const char *str, long *val)
{
	char *endptr;

	*val = strtol(str, &endptr, 10);
	if (endptr == str || (*endptr != '\0' && *endptr != '\n') ||
	    ((*val == LONG_MIN || *val == LONG_MAX) && errno == ERANGE))
		return -EINVAL;
	return 0;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool, unsigned int n_queues)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = n_queues, tx_rings = n_queues;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;

	printf("initializing with %u queues\n", n_queues);

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

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
                                        rte_eth_dev_socket_id(port), NULL,
                                        mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Enable TX offloading */
	rte_eth_dev_info_get(0, &dev_info);
	txconf = &dev_info.default_txconf;

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
	rte_eth_macaddr_get(port, &my_eth);
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			(unsigned)port,
			my_eth.addr_bytes[0], my_eth.addr_bytes[1],
			my_eth.addr_bytes[2], my_eth.addr_bytes[3],
			my_eth.addr_bytes[4], my_eth.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);

	return 0;
}

/*
 * Send out an arp.
 */
static void send_arp(uint16_t op, struct ether_addr dst_eth, uint32_t dst_ip)
{
	struct rte_mbuf *buf;
	char *buf_ptr;
	struct ether_hdr *eth_hdr;
	struct arp_hdr *a_hdr;
	int nb_tx;

	buf = rte_pktmbuf_alloc(tx_mbuf_pool);
	if (buf == NULL)
		printf("error allocating arp mbuf\n");

	/* ethernet header */
	buf_ptr = rte_pktmbuf_append(buf, ETHER_HDR_LEN);
	eth_hdr = (struct ether_hdr *) buf_ptr;

	ether_addr_copy(&my_eth, &eth_hdr->s_addr);
	ether_addr_copy(&dst_eth, &eth_hdr->d_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_ARP);

	/* arp header */
	buf_ptr = rte_pktmbuf_append(buf, sizeof(struct arp_hdr));
	a_hdr = (struct arp_hdr *) buf_ptr;
	a_hdr->arp_hrd = rte_cpu_to_be_16(ARP_HRD_ETHER);
	a_hdr->arp_pro = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
	a_hdr->arp_hln = ETHER_ADDR_LEN;
	a_hdr->arp_pln = 4;
	a_hdr->arp_op = rte_cpu_to_be_16(op);

	ether_addr_copy(&my_eth, &a_hdr->arp_data.arp_sha);
	a_hdr->arp_data.arp_sip = rte_cpu_to_be_32(my_ip);
	ether_addr_copy(&dst_eth, &a_hdr->arp_data.arp_tha);
	a_hdr->arp_data.arp_tip = rte_cpu_to_be_32(dst_ip);

	nb_tx = rte_eth_tx_burst(dpdk_port, 0, &buf, 1);
	if (unlikely(nb_tx != 1)) {
		printf("error: could not send arp packet\n");
	}
}

/*
 * Validate this ethernet header. Return true if this packet is for higher
 * layers, false otherwise.
 */
static bool check_eth_hdr(struct rte_mbuf *buf)
{
	struct ether_hdr *ptr_mac_hdr;
	struct arp_hdr *a_hdr;

	ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct ether_hdr *);
	if (!is_same_ether_addr(&ptr_mac_hdr->d_addr, &my_eth) &&
			!is_broadcast_ether_addr(&ptr_mac_hdr->d_addr)) {
		/* packet not to our ethernet addr */
		return false;
	}

	if (ptr_mac_hdr->ether_type == rte_cpu_to_be_16(ETHER_TYPE_ARP)) {
		/* reply to ARP if necessary */
		a_hdr = rte_pktmbuf_mtod_offset(buf, struct arp_hdr *,
				sizeof(struct ether_hdr));
		if (a_hdr->arp_op == rte_cpu_to_be_16(ARP_OP_REQUEST)
				&& a_hdr->arp_data.arp_tip == rte_cpu_to_be_32(my_ip))
			send_arp(ARP_OP_REPLY, a_hdr->arp_data.arp_sha,
					rte_be_to_cpu_32(a_hdr->arp_data.arp_sip));
		return false;
	}

	if (ptr_mac_hdr->ether_type != rte_cpu_to_be_16(ETHER_TYPE_IPv4))
		/* packet not IPv4 */
		return false;

	return true;
}

/*
 * Return true if this IP packet is to us and contains a UDP packet,
 * false otherwise.
 */
static bool check_ip_hdr(struct rte_mbuf *buf)
{
	struct ipv4_hdr *ipv4_hdr;

	ipv4_hdr = rte_pktmbuf_mtod_offset(buf, struct ipv4_hdr *,
			ETHER_HDR_LEN);
	if (ipv4_hdr->dst_addr != rte_cpu_to_be_32(my_ip)
			|| ipv4_hdr->next_proto_id != IPPROTO_UDP)
		return false;

	return true;
}

/*
 * Run a netperf client
 */
static void do_client(uint8_t port)
{
	uint64_t start_time, end_time, next_send_time;
	struct rte_mbuf *bufs[BURST_SIZE];
	struct rte_mbuf *buf;
	struct ether_hdr *ptr_mac_hdr;
	struct arp_hdr *a_hdr;
	char *buf_ptr;
	struct ether_hdr *eth_hdr;
	struct ipv4_hdr *ipv4_hdr;
	struct udp_hdr *udp_hdr;
	uint32_t nb_tx, nb_rx, i;
	uint64_t reqs = 0;
	struct ether_addr server_eth;
	struct nbench_req *control_req;
	struct nbench_resp *control_resp;
	bool setup_port = false;
	uint64_t interval_cycles, time_received;
	uint32_t max_random_cycles;

	/* Verify that we have enough space for all the datapoints */
	uint32_t samples = seconds / ((float) interval_us / (1000*1000));
	if (samples > MAX_SAMPLES)
		rte_exit(EXIT_FAILURE, "Too many samples: %d\n", samples);

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	if (rte_eth_dev_socket_id(port) > 0 &&
        rte_eth_dev_socket_id(port) != (int)rte_socket_id())
        printf("WARNING, port %u is on remote NUMA node to polling thread.\n\t"
               "Performance will not be optimal.\n", port);

	printf("\nCore %u running in client mode. [Ctrl+C to quit]\n",
			rte_lcore_id());

	/* get the mac address of the server via ARP */
	while (true) {
		send_arp(ARP_OP_REQUEST, broadcast_mac, server_ip);
		sleep(1);

		nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
		if (nb_rx == 0)
			continue;

		for (i = 0; i < nb_rx; i++) {
			buf = bufs[i];

			ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct ether_hdr *);
			if (!is_same_ether_addr(&ptr_mac_hdr->d_addr, &my_eth)) {
					/* packet not to our ethernet addr */
					continue;
			}

			if (ptr_mac_hdr->ether_type == rte_cpu_to_be_16(ETHER_TYPE_ARP)) {
				/* this is an ARP */
				a_hdr = rte_pktmbuf_mtod_offset(buf, struct arp_hdr *,
						sizeof(struct ether_hdr));
				if (a_hdr->arp_op == rte_cpu_to_be_16(ARP_OP_REPLY) &&
						is_same_ether_addr(&a_hdr->arp_data.arp_tha, &my_eth) &&
						a_hdr->arp_data.arp_tip == rte_cpu_to_be_32(my_ip)) {
					/* got a response from server! */
					ether_addr_copy(&a_hdr->arp_data.arp_sha, &server_eth);
					goto got_mac;
				}
			}
		}
	}
got_mac:

	/* randomize inter-arrival times by up to RANDOM_US */
	srand(rte_get_timer_cycles());
	max_random_cycles = (float) RANDOM_US / (1000 * 1000) * rte_get_timer_hz();

	/* run for specified amount of time */
	start_time = rte_get_timer_cycles();
	interval_cycles = (float) interval_us / (1000 * 1000) * rte_get_timer_hz();
	next_send_time = start_time;
	while (rte_get_timer_cycles() <
			start_time + seconds * rte_get_timer_hz()) {
		buf = rte_pktmbuf_alloc(tx_mbuf_pool);
		if (buf == NULL)
			printf("error allocating tx mbuf\n");

		/* ethernet header */
		buf_ptr = rte_pktmbuf_append(buf, ETHER_HDR_LEN);
		eth_hdr = (struct ether_hdr *) buf_ptr;

		ether_addr_copy(&my_eth, &eth_hdr->s_addr);
		ether_addr_copy(&server_eth, &eth_hdr->d_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

		/* IPv4 header */
		buf_ptr = rte_pktmbuf_append(buf, sizeof(struct ipv4_hdr));
		ipv4_hdr = (struct ipv4_hdr *) buf_ptr;
		ipv4_hdr->version_ihl = 0x45;
		ipv4_hdr->type_of_service = 0;
		ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct ipv4_hdr) +
				sizeof(struct udp_hdr) + payload_len);
		ipv4_hdr->packet_id = 0;
		ipv4_hdr->fragment_offset = 0;
		ipv4_hdr->time_to_live = 64;
		ipv4_hdr->next_proto_id = IPPROTO_UDP;
		ipv4_hdr->hdr_checksum = 0;
		ipv4_hdr->src_addr = rte_cpu_to_be_32(my_ip);
		ipv4_hdr->dst_addr = rte_cpu_to_be_32(server_ip);

		/* UDP header + data */
		buf_ptr = rte_pktmbuf_append(buf,
				sizeof(struct udp_hdr) + payload_len);
		udp_hdr = (struct udp_hdr *) buf_ptr;
		udp_hdr->src_port = rte_cpu_to_be_16(client_port);
		udp_hdr->dst_port = rte_cpu_to_be_16(server_port);
		udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct udp_hdr)
				+ payload_len);
		udp_hdr->dgram_cksum = 0;
		memset(buf_ptr + sizeof(struct udp_hdr), 0xAB, payload_len);

		/* control data in case our server is running netbench_udp */
		control_req = (struct nbench_req *) (buf_ptr + sizeof(struct udp_hdr));
		control_req->magic = kMagic;
		control_req->nports = 1;

		buf->l2_len = ETHER_HDR_LEN;
		buf->l3_len = sizeof(struct ipv4_hdr);
		buf->ol_flags = PKT_TX_IP_CKSUM | PKT_TX_IPV4;

		/* send packet */
		snd_times[reqs] = rte_get_timer_cycles();
		nb_tx = rte_eth_tx_burst(port, 0, &buf, 1);

		if (unlikely(nb_tx != 1)) {
			printf("error: could not send packet\n");
		}

		nb_rx = 0;
		while (true) {
			nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
			time_received = rte_get_timer_cycles();
			if (nb_rx == 0)
				continue;

			for (i = 0; i < nb_rx; i++) {
				buf = bufs[i];

				if (!check_eth_hdr(buf))
					goto no_match;

				/* this packet is IPv4, check IP header */
				if (!check_ip_hdr(buf))
					goto no_match;

				/* check UDP header */
				udp_hdr = rte_pktmbuf_mtod_offset(buf, struct udp_hdr *,
						ETHER_HDR_LEN + sizeof(struct ipv4_hdr));
				if (udp_hdr->src_port != rte_cpu_to_be_16(server_port) ||
				    udp_hdr->dst_port != rte_cpu_to_be_16(client_port))
					goto no_match;

				if (!setup_port &&
				    udp_hdr->dgram_len != rte_cpu_to_be_16(sizeof(struct udp_hdr) +
									   payload_len)) {
					/* use port specified by netbench_udp server */
					control_resp = rte_pktmbuf_mtod_offset(buf, struct nbench_resp *,
							ETHER_HDR_LEN + sizeof(struct ipv4_hdr) +
							sizeof(struct udp_hdr));
					if (control_resp->nports != 1)
						goto no_match;
					server_port = control_resp->ports[0];

					/* reset start time so we don't include control message RTT */
					start_time = rte_get_timer_cycles();
					setup_port = true;
				}

				/* packet matches */
				rte_pktmbuf_free(buf);
				goto found_match;

			no_match:
				/* packet isn't what we're looking for, free it and rx again */
				rte_pktmbuf_free(buf);
			}
		}
	found_match:
		rcv_times[reqs++] = time_received;
		next_send_time += (interval_cycles + (rand() % max_random_cycles) -
				   max_random_cycles * 0.5);
		while (rte_get_timer_cycles() < next_send_time) {
		  /* spin until time for next packet */
		}
	}
	end_time = rte_get_timer_cycles();

	/* add up total cycles across all RTTs, skip first and last 10% */
	uint64_t total_cycles = 0;
	uint64_t included_samples = 0;
	for (i = reqs * 0.1; i < reqs * 0.9; i++) {
		total_cycles += rcv_times[i] - snd_times[i];
		included_samples++;
	}

	printf("ran for %f seconds, sent %"PRIu64" packets\n",
			(float) (end_time - start_time) / rte_get_timer_hz(), reqs);
	printf("client reqs/s: %f\n",
			(float) (reqs * rte_get_timer_hz()) / (end_time - start_time));
	printf("mean latency (us): %f\n", (float) total_cycles *
		1000 * 1000 / (included_samples * rte_get_timer_hz()));

	if (output_filename != NULL) {
		/* print all samples to output file */
		FILE *outfile = fopen(output_filename, "w");
		fprintf(outfile, "index,time_us\n");
		for (i = reqs * 0.1; i < reqs * 0.9; i++) {
			float time_us = ((float) (rcv_times[i] - snd_times[i]) * 1000 * 1000) /
				rte_get_timer_hz();
			fprintf(outfile, "%d,%f\n", i, time_us);
		}
		fclose(outfile);
	}
}

/*
 * Run a netperf server
 */
static int
do_server(void *arg)
{
	uint8_t port = dpdk_port;
	uint8_t queue = (uint64_t) arg;
	struct rte_mbuf *rx_bufs[BURST_SIZE];
	struct rte_mbuf *tx_bufs[BURST_SIZE];
	struct rte_mbuf *buf;
	uint16_t nb_rx, n_to_tx, nb_tx, i, j, q;
	struct ether_hdr *ptr_mac_hdr;
	struct ether_addr src_addr;
	struct ipv4_hdr *ptr_ipv4_hdr;
	uint32_t src_ip_addr;
	uint16_t tmp_port;
	struct nbench_req *control_req;
	struct nbench_resp *control_resp;

	printf("on server core with lcore_id: %d, queue: %d", rte_lcore_id(),
			queue);

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	if (rte_eth_dev_socket_id(port) > 0 &&
        rte_eth_dev_socket_id(port) != (int)rte_socket_id())
        printf("WARNING, port %u is on remote NUMA node to polling thread.\n\t"
               "Performance will not be optimal.\n", port);

	printf("\nCore %u running in server mode. [Ctrl+C to quit]\n",
			rte_lcore_id());

	/* Run until the application is quit or killed. */
	for (;;) {
		for (q = 0; q < num_queues; q++) {

			/* receive packets */
			nb_rx = rte_eth_rx_burst(port, q, rx_bufs, BURST_SIZE);

			if (nb_rx == 0)
				continue;

			n_to_tx = 0;
			for (i = 0; i < nb_rx; i++) {
				buf = rx_bufs[i];

				if (!check_eth_hdr(buf))
					goto free_buf;

				/* this packet is IPv4, check IP header */
				if (!check_ip_hdr(buf))
					goto free_buf;

				/* swap src and dst ether addresses */
				ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct ether_hdr *);
				ether_addr_copy(&ptr_mac_hdr->s_addr, &src_addr);
				ether_addr_copy(&ptr_mac_hdr->d_addr, &ptr_mac_hdr->s_addr);
				ether_addr_copy(&src_addr, &ptr_mac_hdr->d_addr);

				/* swap src and dst IP addresses */
				ptr_ipv4_hdr = rte_pktmbuf_mtod_offset(buf, struct ipv4_hdr *,
								ETHER_HDR_LEN);
				src_ip_addr = ptr_ipv4_hdr->src_addr;
				ptr_ipv4_hdr->src_addr = ptr_ipv4_hdr->dst_addr;
				ptr_ipv4_hdr->dst_addr = src_ip_addr;

				/* swap UDP ports */
				struct udp_hdr *udp_hdr;
				udp_hdr = rte_pktmbuf_mtod_offset(buf, struct udp_hdr *,
								ETHER_HDR_LEN + sizeof(struct ipv4_hdr));
				tmp_port = udp_hdr->src_port;
				udp_hdr->src_port = udp_hdr->dst_port;
				udp_hdr->dst_port = tmp_port;

				/* check if this is a control message and we need to reply with
				 * ports */
				control_req = rte_pktmbuf_mtod_offset(buf, struct nbench_req *,
								ETHER_HDR_LEN + sizeof(struct ipv4_hdr) +
								sizeof(struct udp_hdr));
				if (control_req->magic == kMagic) {
					rte_pktmbuf_append(buf, sizeof(struct nbench_resp) +
							sizeof(uint16_t) *
							control_req->nports -
							sizeof(struct nbench_req));
					control_resp = (struct nbench_resp *) control_req;

					/* add ports to response */
					for (j = 0; j < control_req->nports; j++) {
						/* simple port allocation */
						control_resp->ports[j] = rte_cpu_to_be_16(next_port++);
					}

					/* adjust lengths in UDP and IPv4 headers */
					payload_len = sizeof(struct nbench_resp) +
						sizeof(uint16_t) * control_req->nports;
					udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct udp_hdr) +
									payload_len);
					ptr_ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct ipv4_hdr) +
										sizeof(struct udp_hdr) + payload_len);

					/* enable computation of IPv4 checksum in hardware */
					ptr_ipv4_hdr->hdr_checksum = 0;
					buf->l2_len = ETHER_HDR_LEN;
					buf->l3_len = sizeof(struct ipv4_hdr);
					buf->ol_flags = PKT_TX_IP_CKSUM | PKT_TX_IPV4;
				}

				tx_bufs[n_to_tx++] = buf;
				continue;

			free_buf:
				/* packet wasn't sent, free it */
				rte_pktmbuf_free(buf);
			}

			/* transmit packets */
			nb_tx = rte_eth_tx_burst(port, q, tx_bufs, n_to_tx);

			if (nb_tx != n_to_tx)
				printf("error: could not transmit all packets: %d %d\n",
					n_to_tx, nb_tx);
		}
	}

	return 0;
}

/*
 * Initialize dpdk.
 */
static int dpdk_init(int argc, char *argv[])
{
	int args_parsed;

	/* Initialize the Environment Abstraction Layer (EAL). */
	args_parsed = rte_eal_init(argc, argv);
	if (args_parsed < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	/* Check that there is a port to send/receive on. */
	if (!rte_eth_dev_is_valid_port(0))
		rte_exit(EXIT_FAILURE, "Error: no available ports\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	rx_mbuf_pool = rte_pktmbuf_pool_create("MBUF_RX_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (rx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create rx mbuf pool\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	tx_mbuf_pool = rte_pktmbuf_pool_create("MBUF_TX_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (tx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create tx mbuf pool\n");

	return args_parsed;
}

static int parse_netperf_args(int argc, char *argv[])
{
	long tmp;

	/* argv[0] is still the program name */
	if (argc < 3) {
		printf("not enough arguments left: %d\n", argc);
		return -EINVAL;
	}

	str_to_ip(argv[2], &my_ip);

	if (!strcmp(argv[1], "UDP_CLIENT")) {
		mode = MODE_UDP_CLIENT;
		argc -= 3;
		if (argc < 6) {
			printf("not enough arguments left: %d\n", argc);
			return -EINVAL;
		}
		str_to_ip(argv[3], &server_ip);
		if (sscanf(argv[4], "%u", &client_port) != 1)
			return -EINVAL;
		if (sscanf(argv[5], "%u", &server_port) != 1)
			return -EINVAL;
		str_to_long(argv[6], &tmp);
		seconds = tmp;
		str_to_long(argv[7], &tmp);
		payload_len = tmp;
		str_to_long(argv[8], &tmp);
		interval_us = tmp;
		if (argc >= 7) {
			/* long output file name */
			output_filename = argv[9];
		}
	} else if (!strcmp(argv[1], "UDP_SERVER")) {
		mode = MODE_UDP_SERVER;
		argc -= 3;
		if (argc >= 1) {
			if (sscanf(argv[3], "%u", &num_queues) != 1)
				return -EINVAL;
		}
	} else {
		printf("invalid mode '%s'\n", argv[1]);
		return -EINVAL;
	}

	return 0;
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
	int args_parsed, res, lcore_id;
	uint64_t i;

	/* Initialize dpdk. */
	args_parsed = dpdk_init(argc, argv);

	/* initialize our arguments */
	argc -= args_parsed;
	argv += args_parsed;
	res = parse_netperf_args(argc, argv);
	if (res < 0)
		return 0;

	/* initialize port */
	if (mode == MODE_UDP_CLIENT && rte_lcore_count() > 1)
		printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");
	if (port_init(dpdk_port, rx_mbuf_pool, num_queues) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n", dpdk_port);

	if (mode == MODE_UDP_CLIENT)
		do_client(dpdk_port);
	else {
		i = 0;
		RTE_LCORE_FOREACH_SLAVE(lcore_id)
			rte_eal_remote_launch(do_server, (void *) i++, lcore_id);
		do_server((void *) i);
	}

	return 0;
}
