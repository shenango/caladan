/*
 * queue.h - shared memory queues between the iokernel and the runtimes
 */

#pragma once

#include <stdint.h>
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

	CHECKSUM_TYPE_NR,
};

/* possible values for @olflags above */
#define OLFLAG_IP_CHKSUM	BIT(0)	/* enable IP checksum generation */
#define OLFLAG_TCP_CHKSUM	BIT(1)	/* enable TCP checksum generation */
#define OLFLAG_IPV4		BIT(2)  /* indicates the packet is IPv4 */
#define OLFLAG_IPV6		BIT(3)  /* indicates the packet is IPv6 */
#define TXFLAG_LOCAL		BIT(4)  /* indicates the packet is local */
#define TXFLAG_BROADCAST	BIT(5)  /* indicates a broadcast packet */

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

BUILD_ASSERT(RX_CALL_NR <= UINT16_MAX);

union rxq_cmd {
	struct {
		uint16_t	rxcmd;
		uint16_t	len;
		uint16_t	data_offset;
		uint16_t	csum_type; // top bit must be 0.
	};
	uint64_t		lrpc_cmd;
};

BUILD_ASSERT(CHECKSUM_TYPE_NR < INT16_MAX);
BUILD_ASSERT(sizeof(union rxq_cmd) == sizeof(uint64_t));


/*
 * TX packet queues: RUNTIMES -> IOKERNEL
 * These queues are only for network packets and can experience HOL blocking.
 */
enum {
	TXPKT_NET_XMIT = 0,     /* points to a struct tx_net_hdr */
	TXPKT_NR,               /* number of commands */
};

BUILD_ASSERT(TXPKT_NR <= UINT16_MAX);

union txpkt_xmit_cmd {
	struct {
		uint16_t	txcmd;
		uint16_t	len;
		uint16_t	olflags;
		char		pad;
		char		reserved; // Must not be used (lrpc parity bit).
	};
	uint64_t		lrpc_cmd;
};

BUILD_ASSERT(sizeof(union txpkt_xmit_cmd) == sizeof(uint64_t));

/*
 * TX command queues: RUNTIMES -> IOKERNEL
 * These queues handle a variety of commands, and typically they are handled
 * much faster by the IOKERNEL than packets, so no HOL blocking.
 */
enum {
	TXCMD_NET_COMPLETE = 0,	/* contains rx_net_hdr.completion_data */
	TXCMD_NR,		/* number of commands */
};

BUILD_ASSERT(TXCMD_NR <= UINT16_MAX);

union txcmdq_cmd {
	struct {
		uint16_t	txcmd;
		uint16_t	data_offset;
		uint32_t	reserved; // Upper bit must be zero.
	};
	uint64_t		lrpc_cmd;
};

BUILD_ASSERT(sizeof(union txcmdq_cmd) == sizeof(uint64_t));
