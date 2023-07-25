/*
 * queue.h - shared memory queues between the iokernel and the runtimes
 */

#pragma once

#include <base/stddef.h>

/* preamble to ingress network packets */
struct rx_net_hdr {
	unsigned long completion_data; /* a tag to help complete the request */
	unsigned int len;	/* the length of the payload */
	unsigned int rss_hash;	/* the HW RSS 5-tuple hash */
	unsigned int csum_type; /* the type of checksum */
	unsigned int csum;	/* 16-bit one's complement */
	char	     payload[];	/* packet data */
};

/* preamble to egress network packets */
struct tx_net_hdr {
	unsigned long completion_data; /* a tag to help complete the request */
	unsigned int len;	/* the length of the payload */
	unsigned int olflags;	/* offload flags */
	unsigned short pad;	/* because of 14 byte ethernet header */
	char	     payload[];	/* packet data */
} __attribute__((__packed__));

/* possible values for @csum_type above */
enum {
	/*
	 * Hardware did not provide checksum information.
	 */
	CHECKSUM_TYPE_NEEDED = 0,

	/*
	 * The checksum was verified by hardware and found to be valid.
	 */
	CHECKSUM_TYPE_UNNECESSARY,

	/* 
	 * Hardware provided a 16 bit one's complement sum from after the LL
	 * header to the end of the packet. VLAN tags (if present) are included
	 * in the sum. This is the most robust checksum type because it's useful
	 * even if the NIC can't parse the headers.
	 */
	CHECKSUM_TYPE_COMPLETE,
};

/* possible values for @olflags above */
#define OLFLAG_IP_CHKSUM	BIT(0)	/* enable IP checksum generation */
#define OLFLAG_TCP_CHKSUM	BIT(1)	/* enable TCP checksum generation */
#define OLFLAG_IPV4		BIT(2)  /* indicates the packet is IPv4 */
#define OLFLAG_IPV6		BIT(3)  /* indicates the packet is IPv6 */

/*
 * RX queues: IOKERNEL -> RUNTIMES
 * These queues multiplex several different types of requests.
 */
enum {
	RX_NET_RECV = 0,	/* points to a struct rx_net_hdr */
	RX_NET_COMPLETE,	/* contains tx_net_hdr.completion_data */
	RX_REFILL_BUFS,		/* runtime should replenish RX work queues */
	RX_CALL_NR,		/* number of commands */
};


/*
 * TX packet queues: RUNTIMES -> IOKERNEL
 * These queues are only for network packets and can experience HOL blocking.
 */
enum {
	TXPKT_NET_XMIT = 0,	/* points to a struct tx_net_hdr */
	TXPKT_NR,		/* number of commands */
};


/*
 * TX command queues: RUNTIMES -> IOKERNEL
 * These queues handle a variety of commands, and typically they are handled
 * much faster by the IOKERNEL than packets, so no HOL blocking.
 */
enum {
	TXCMD_NET_COMPLETE = 0,	/* contains rx_net_hdr.completion_data */
	TXCMD_NR,		/* number of commands */
};
