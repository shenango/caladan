/*-
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)ip.h	8.2 (Berkeley) 6/1/94
 * $FreeBSD$
 */

#pragma once

#include <base/types.h>
#include <base/byteorder.h>

/*
 * Definitions for internet protocol version 4.
 *
 * Per RFC 791, September 1981.
 */
#define	IPVERSION	4

#define MAKE_IP_ADDR(a, b, c, d)			\
	(((uint32_t) a << 24) | ((uint32_t) b << 16) |	\
	 ((uint32_t) c << 8) | (uint32_t) d)

#define IP_ADDR_STR_LEN	16

extern char *ip_addr_to_str(uint32_t addr, char *str);

/*
 * Structure of an internet header, naked of options.
 */
struct ip_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t	header_len:4,		/* header length */
		version:4;		/* version */
#endif
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t	version:4,		/* version */
		header_len:4;		/* header length */
#endif
	uint8_t tos;			/* type of service */
	uint16_t len;			/* total length */
	uint16_t id;			/* identification */
	uint16_t off;			/* fragment offset field */
#define	IP_RF 0x8000			/* reserved fragment flag */
#define	IP_DF 0x4000			/* dont fragment flag */
#define	IP_MF 0x2000			/* more fragments flag */
#define	IP_OFFMASK 0x1fff		/* mask for fragmenting bits */
	uint8_t ttl;			/* time to live */
	uint8_t proto;			/* protocol */
	uint16_t chksum;		/* checksum */
	uint32_t saddr;			/* source address */
	uint32_t daddr;			/* dest address */
} __packed __aligned(4);

#define	IP_MAXPACKET	65535		/* maximum packet size */

/*
 * Definitions for IP type of service (ip_tos).
 */
#define	IPTOS_LOWDELAY		0x10
#define	IPTOS_THROUGHPUT	0x08
#define	IPTOS_RELIABILITY	0x04
#define	IPTOS_MINCOST		0x02

/*
 * Definitions for IP precedence (also in ip_tos) (hopefully unused).
 */
#define	IPTOS_PREC_NETCONTROL		0xe0
#define	IPTOS_PREC_INTERNETCONTROL	0xc0
#define	IPTOS_PREC_CRITIC_ECP		0xa0
#define	IPTOS_PREC_FLASHOVERRIDE	0x80
#define	IPTOS_PREC_FLASH		0x60
#define	IPTOS_PREC_IMMEDIATE		0x40
#define	IPTOS_PREC_PRIORITY		0x20
#define	IPTOS_PREC_ROUTINE		0x00

/*
 * Definitions for DiffServ Codepoints as per RFC2474
 */
#define	IPTOS_DSCP_CS0		0x00
#define	IPTOS_DSCP_CS1		0x20
#define	IPTOS_DSCP_AF11		0x28
#define	IPTOS_DSCP_AF12		0x30
#define	IPTOS_DSCP_AF13		0x38
#define	IPTOS_DSCP_CS2		0x40
#define	IPTOS_DSCP_AF21		0x48
#define	IPTOS_DSCP_AF22		0x50
#define	IPTOS_DSCP_AF23		0x58
#define	IPTOS_DSCP_CS3		0x60
#define	IPTOS_DSCP_AF31		0x68
#define	IPTOS_DSCP_AF32		0x70
#define	IPTOS_DSCP_AF33		0x78
#define	IPTOS_DSCP_CS4		0x80
#define	IPTOS_DSCP_AF41		0x88
#define	IPTOS_DSCP_AF42		0x90
#define	IPTOS_DSCP_AF43		0x98
#define	IPTOS_DSCP_CS5		0xa0
#define	IPTOS_DSCP_EF		0xb8
#define	IPTOS_DSCP_CS6		0xc0
#define	IPTOS_DSCP_CS7		0xe0

/*
 * ECN (Explicit Congestion Notification) codepoints in RFC3168 mapped to the
 * lower 2 bits of the TOS field.
 */
#define	IPTOS_ECN_NOTECT	0x00	/* not-ECT */
#define	IPTOS_ECN_ECT1		0x01	/* ECN-capable transport (1) */
#define	IPTOS_ECN_ECT0		0x02	/* ECN-capable transport (0) */
#define	IPTOS_ECN_CE		0x03	/* congestion experienced */
#define	IPTOS_ECN_MASK		0x03	/* ECN field mask */

/*
 * Definitions for options.
 */
#define	IPOPT_COPIED(o)		((o)&0x80)
#define	IPOPT_CLASS(o)		((o)&0x60)
#define	IPOPT_NUMBER(o)		((o)&0x1f)

#define	IPOPT_CONTROL		0x00
#define	IPOPT_RESERVED1		0x20
#define	IPOPT_DEBMEAS		0x40
#define	IPOPT_RESERVED2		0x60

#define	IPOPT_EOL		0		/* end of option list */
#define	IPOPT_NOP		1		/* no operation */

#define	IPOPT_RR		7		/* record packet route */
#define	IPOPT_TS		68		/* timestamp */
#define	IPOPT_SECURITY		130		/* provide s,c,h,tcc */
#define	IPOPT_LSRR		131		/* loose source route */
#define	IPOPT_ESO		133		/* extended security */
#define	IPOPT_CIPSO		134		/* commerical security */
#define	IPOPT_SATID		136		/* satnet id */
#define	IPOPT_SSRR		137		/* strict source route */
#define	IPOPT_RA		148		/* router alert */

/*
 * Offsets to fields in options other than EOL and NOP.
 */
#define	IPOPT_OPTVAL		0		/* option ID */
#define	IPOPT_OLEN		1		/* option length */
#define	IPOPT_OFFSET		2		/* offset within option */
#define	IPOPT_MINOFF		4		/* min value of above */

/*
 * Time stamp option structure.
 */
struct	ip_timestamp {
	uint8_t code;			/* IPOPT_TS */
	uint8_t len;			/* size of structure (variable) */
	uint8_t ptr;			/* index of current entry */
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t flags:4,		/* flags, see below */
		overflow:4;		/* overflow counter */
#endif
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t overflow:4,		/* overflow counter */
		flags:4;		/* flags, see below */
#endif
	union  {
		uint32_t time[1];	/* network format */
		struct {
			uint32_t addr;
			uint32_t ipt_time;	/* network format */
		} ta[1];
	} u;
};

/* Flag bits for ipt_flg. */
#define	IPOPT_TS_TSONLY		0		/* timestamps only */
#define	IPOPT_TS_TSANDADDR	1		/* timestamps and addresses */
#define	IPOPT_TS_PRESPEC	3		/* specified modules only */

/* Bits for security (not byte swapped). */
#define	IPOPT_SECUR_UNCLASS	0x0000
#define	IPOPT_SECUR_CONFID	0xf135
#define	IPOPT_SECUR_EFTO	0x789a
#define	IPOPT_SECUR_MMMM	0xbc4d
#define	IPOPT_SECUR_RESTR	0xaf13
#define	IPOPT_SECUR_SECRET	0xd788
#define	IPOPT_SECUR_TOPSECRET	0x6bc5

/*
 * Internet implementation parameters.
 */
#define	MAXTTL		255		/* maximum time to live (seconds) */
#define	IPDEFTTL	64		/* default ttl, from RFC 1340 */
#define	IPFRAGTTL	60		/* time to live for frags, slowhz */
#define	IPTTLDEC	1		/* subtracted when forwarding */
#define	IP_MSS		576		/* default maximum segment size */

/*
 * This is the real IPv4 pseudo header, used for computing the TCP and UDP
 * checksums. For the Internet checksum, struct ipovly can be used instead.
 * For stronger checksums, the real thing must be used.
 */
struct ip_pseudo {
	uint32_t	saddr;		/* source internet address */
	uint32_t	daddr;		/* destination internet address */
	uint8_t		pad;		/* pad, must be zero */
	uint8_t		proto;		/* protocol */
	uint16_t	len;		/* protocol length */
};

/* Protocols common to RFC 1700, POSIX, and X/Open. */
#define	IPPROTO_IP		0		/* dummy for IP */
#define	IPPROTO_ICMP		1		/* control message protocol */
#define	IPPROTO_TCP		6		/* tcp */
#define	IPPROTO_UDP		17		/* user datagram protocol */

/* Protocols (RFC 1700) */
#define	IPPROTO_HOPOPTS		0		/* IP6 hop-by-hop options */
#define	IPPROTO_IGMP		2		/* group mgmt protocol */
#define	IPPROTO_GGP		3		/* gateway^2 (deprecated) */
#define	IPPROTO_IPV4		4		/* IPv4 encapsulation */
#define	IPPROTO_IPIP		IPPROTO_IPV4	/* for compatibility */
#define	IPPROTO_ST		7		/* Stream protocol II */
#define	IPPROTO_EGP		8		/* exterior gateway protocol */
#define	IPPROTO_PIGP		9		/* private interior gateway */
#define	IPPROTO_RCCMON		10		/* BBN RCC Monitoring */
#define	IPPROTO_NVPII		11		/* network voice protocol*/
#define	IPPROTO_PUP		12		/* pup */
#define	IPPROTO_ARGUS		13		/* Argus */
#define	IPPROTO_EMCON		14		/* EMCON */
#define	IPPROTO_XNET		15		/* Cross Net Debugger */
#define	IPPROTO_CHAOS		16		/* Chaos*/
#define	IPPROTO_MUX		18		/* Multiplexing */
#define	IPPROTO_MEAS		19		/* DCN Measurement Subsystems */
#define	IPPROTO_HMP		20		/* Host Monitoring */
#define	IPPROTO_PRM		21		/* Packet Radio Measurement */
#define	IPPROTO_IDP		22		/* xns idp */
#define	IPPROTO_TRUNK1		23		/* Trunk-1 */
#define	IPPROTO_TRUNK2		24		/* Trunk-2 */
#define	IPPROTO_LEAF1		25		/* Leaf-1 */
#define	IPPROTO_LEAF2		26		/* Leaf-2 */
#define	IPPROTO_RDP		27		/* Reliable Data */
#define	IPPROTO_IRTP		28		/* Reliable Transaction */
#define	IPPROTO_TP		29		/* tp-4 w/ class negotiation */
#define	IPPROTO_BLT		30		/* Bulk Data Transfer */
#define	IPPROTO_NSP		31		/* Network Services */
#define	IPPROTO_INP		32		/* Merit Internodal */
#define	IPPROTO_SEP		33		/* Sequential Exchange */
#define	IPPROTO_3PC		34		/* Third Party Connect */
#define	IPPROTO_IDPR		35		/* InterDomain Policy Routing */
#define	IPPROTO_XTP		36		/* XTP */
#define	IPPROTO_DDP		37		/* Datagram Delivery */
#define	IPPROTO_CMTP		38		/* Control Message Transport */
#define	IPPROTO_TPXX		39		/* TP++ Transport */
#define	IPPROTO_IL		40		/* IL transport protocol */
#define	IPPROTO_IPV6		41		/* IP6 header */
#define	IPPROTO_SDRP		42		/* Source Demand Routing */
#define	IPPROTO_ROUTING		43		/* IP6 routing header */
#define	IPPROTO_FRAGMENT	44		/* IP6 fragmentation header */
#define	IPPROTO_IDRP		45		/* InterDomain Routing*/
#define	IPPROTO_RSVP		46		/* resource reservation */
#define	IPPROTO_GRE		47		/* General Routing Encap. */
#define	IPPROTO_MHRP		48		/* Mobile Host Routing */
#define	IPPROTO_BHA		49		/* BHA */
#define	IPPROTO_ESP		50		/* IP6 Encap Sec. Payload */
#define	IPPROTO_AH		51		/* IP6 Auth Header */
#define	IPPROTO_INLSP		52		/* Integ. Net Layer Security */
#define	IPPROTO_SWIPE		53		/* IP with encryption */
#define	IPPROTO_NHRP		54		/* Next Hop Resolution */
#define	IPPROTO_MOBILE		55		/* IP Mobility */
#define	IPPROTO_TLSP		56		/* Transport Layer Security */
#define	IPPROTO_SKIP		57		/* SKIP */
#define	IPPROTO_ICMPV6		58		/* ICMP6 */
#define	IPPROTO_NONE		59		/* IP6 no next header */
#define	IPPROTO_DSTOPTS		60		/* IP6 destination option */
#define	IPPROTO_AHIP		61		/* any host internal protocol */
#define	IPPROTO_CFTP		62		/* CFTP */
#define	IPPROTO_HELLO		63		/* "hello" routing protocol */
#define	IPPROTO_SATEXPAK	64		/* SATNET/Backroom EXPAK */
#define	IPPROTO_KRYPTOLAN	65		/* Kryptolan */
#define	IPPROTO_RVD		66		/* Remote Virtual Disk */
#define	IPPROTO_IPPC		67		/* Pluribus Packet Core */
#define	IPPROTO_ADFS		68		/* Any distributed FS */
#define	IPPROTO_SATMON		69		/* Satnet Monitoring */
#define	IPPROTO_VISA		70		/* VISA Protocol */
#define	IPPROTO_IPCV		71		/* Packet Core Utility */
#define	IPPROTO_CPNX		72		/* Comp. Prot. Net. Executive */
#define	IPPROTO_CPHB		73		/* Comp. Prot. HeartBeat */
#define	IPPROTO_WSN		74		/* Wang Span Network */
#define	IPPROTO_PVP		75		/* Packet Video Protocol */
#define	IPPROTO_BRSATMON	76		/* BackRoom SATNET Monitoring */
#define	IPPROTO_ND		77		/* Sun net disk proto (temp.) */
#define	IPPROTO_WBMON		78		/* WIDEBAND Monitoring */
#define	IPPROTO_WBEXPAK		79		/* WIDEBAND EXPAK */
#define	IPPROTO_EON		80		/* ISO cnlp */
#define	IPPROTO_VMTP		81		/* VMTP */
#define	IPPROTO_SVMTP		82		/* Secure VMTP */
#define	IPPROTO_VINES		83		/* Banyon VINES */
#define	IPPROTO_TTP		84		/* TTP */
#define	IPPROTO_IGP		85		/* NSFNET-IGP */
#define	IPPROTO_DGP		86		/* dissimilar gateway prot. */
#define	IPPROTO_TCF		87		/* TCF */
#define	IPPROTO_IGRP		88		/* Cisco/GXS IGRP */
#define	IPPROTO_OSPFIGP		89		/* OSPFIGP */
#define	IPPROTO_SRPC		90		/* Strite RPC protocol */
#define	IPPROTO_LARP		91		/* Locus Address Resoloution */
#define	IPPROTO_MTP		92		/* Multicast Transport */
#define	IPPROTO_AX25		93		/* AX.25 Frames */
#define	IPPROTO_IPEIP		94		/* IP encapsulated in IP */
#define	IPPROTO_MICP		95		/* Mobile Int.ing control */
#define	IPPROTO_SCCSP		96		/* Semaphore Comm. security */
#define	IPPROTO_ETHERIP		97		/* Ethernet IP encapsulation */
#define	IPPROTO_ENCAP		98		/* encapsulation header */
#define	IPPROTO_APES		99		/* any private encr. scheme */
#define	IPPROTO_GMTP		100		/* GMTP*/
#define	IPPROTO_IPCOMP		108		/* payload compression (IPComp) */
#define	IPPROTO_SCTP		132		/* SCTP */
#define	IPPROTO_MH		135		/* IPv6 Mobility Header */
#define	IPPROTO_HIP		139		/* IP6 Host Identity Protocol */
#define	IPPROTO_SHIM6		140		/* IP6 Shim6 Protocol */
/* 101-254: Partly Unassigned */
#define	IPPROTO_PIM		103		/* Protocol Independent Mcast */
#define	IPPROTO_CARP		112		/* CARP */
#define	IPPROTO_PGM		113		/* PGM */
#define	IPPROTO_MPLS		137		/* MPLS-in-IP */
#define	IPPROTO_PFSYNC		240		/* PFSYNC */
#define	IPPROTO_RESERVED_253	253		/* Reserved */
#define	IPPROTO_RESERVED_254	254		/* Reserved */
#define	IPPROTO_DIRECTPATH_ARP_ENCAP	252 /* custom encapsulated ARP packet */
/* 255: Reserved */
/* BSD Private, local use, namespace incursion, no longer used */
#define	IPPROTO_OLD_DIVERT	254		/* OLD divert pseudo-proto */
#define	IPPROTO_RAW		255		/* raw IP packet */
#define	IPPROTO_MAX		256

