/*
 * queue.h - shared memory queues between the iokernel and the runtimes
 */

#pragma once

#include <stdint.h>
#include <base/stddef.h>

/* possible values for csum_type */
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

BUILD_ASSERT(CHECKSUM_TYPE_NR <= UINT8_MAX);

/* possible values for @olflags above */
#define OLFLAG_IP_CHKSUM	BIT(0)	/* enable IP checksum generation */
#define OLFLAG_TCP_CHKSUM	BIT(1)	/* enable TCP checksum generation */
#define OLFLAG_IPV4		BIT(2)  /* indicates the packet is IPv4 */
#define OLFLAG_IPV6		BIT(3)  /* indicates the packet is IPv6 */
#define TXFLAG_LOCAL		BIT(4)  /* indicates the packet is local */
#define TXFLAG_BROADCAST	BIT(5)  /* indicates a broadcast packet */
#define TXFLAG_LOCAL_HINT	BIT(6)  /* contains rss hash/IP for loopback hint */

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
		uint16_t	pad;
		uint16_t	csum_type; // Top bit must be 0.
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

BUILD_ASSERT(TXPKT_NR <= UINT8_MAX);

union txpkt_xmit_cmd {
	struct {
		uint32_t	dst_ip;
		union {
			uint16_t	len;
			uint16_t	data_offset;
		};
		uint8_t		olflags;
		uint8_t		txcmd; // Top bit must be 0.
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
       TXCMD_NET_COMPLETE = 0, /* contains rx_net_hdr.completion_data */
       TXCMD_NR,               /* number of commands */
};

BUILD_ASSERT(TXCMD_NR <= UINT16_MAX);

union txcmdq_cmd {
	struct {
		uint16_t	txcmd;
		uint16_t	pad;
		uint32_t	reserved; // Upper bit must be zero.
	};
	uint64_t		lrpc_cmd;
};

BUILD_ASSERT(sizeof(union txcmdq_cmd) == sizeof(uint64_t));

static inline uint64_t txpkt_to_payload(uint64_t ptr, uint16_t rss)
{
	assert(!(ptr >> 48));
	return ptr | ((uint64_t)rss << 48);
}

static inline uint64_t ptr_from_txpkt_payload(uint64_t payload)
{
	return payload & ((1UL << 48) - 1);
}

static inline uint64_t rss_from_txpkt_payload(uint64_t payload)
{
	return payload >> 48;
}
