/*
 * tcp.h - Transmission Control Protocol (TCP) definitions
 *
 * Based on Freebsd, BSD licensed.
 */

#pragma once

#include <base/stddef.h>

typedef	uint32_t tcp_seq;

/*
 * TCP header.
 * Per RFC 793, September, 1981.
 */
struct tcp_hdr {
	uint16_t	sport;		/* source port */
	uint16_t	dport;		/* destination port */
	tcp_seq		seq;		/* sequence number */
	tcp_seq		ack;		/* acknowledgement number */
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t		x2:4,		/* (unused) */
			off:4;		/* data offset */
#endif
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t		off:4,		/* data offset */
			x2:4;		/* (unused) */
#endif
	uint8_t		flags;
#define	TCP_FIN		0x01
#define	TCP_SYN		0x02
#define	TCP_RST		0x04
#define	TCP_PUSH	0x08
#define	TCP_ACK		0x10
#define	TCP_URG		0x20
#define	TCP_ECE		0x40
#define	TCP_CWR		0x80
#define	TCP_FLAGS \
	(TCP_FIN|TCP_SYN|TCP_RST|TCP_PUSH|TCP_ACK|TCP_URG|TCP_ECE|TCP_CWR)
#define	PRINT_TCP_FLAGS	"\20\1FIN\2SYN\3RST\4PUSH\5ACK\6URG\7ECE\10CWR"

	uint16_t	win;		/* window */
	uint16_t	sum;		/* checksum */
	uint16_t	urp;		/* urgent pointer */
};

/*
 * TCP options.
 */
#define TCP_OPT_EOL	0 /* end of options */
#define TCP_OPT_NOP	1 /* used for padding */
#define TCP_OPT_MSS	2 /* maximum segment size negotiation */
#define TCP_OPT_WSCALE	3 /* window scaling factor */

#define TCP_OLEN_MSS	4
#define TCP_OLEN_WSCALE	3
