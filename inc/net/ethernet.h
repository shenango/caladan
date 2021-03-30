/*
 * ethernet.h - protocol definitions for ethernet frames
 *
 * Based on Freebsd's sys/net/ethernet.h.
 */

#pragma once

#include <base/types.h>
#include <base/compiler.h>

#define ETH_ADDR_LEN		6
#define ETH_TYPE_LEN		2
#define ETH_CRC_LEN		4
#define ETH_HDR_LEN		(ETH_ADDR_LEN * 2 + ETH_TYPE_LEN)
#define ETH_MIN_LEN		64
#define ETH_MAX_LEN		1518
#define	ETH_MAX_LEN_JUMBO	9018	/* max jumbo frame len, including CRC */
#define ETH_DEFAULT_MTU		1500
#define ETH_MAX_MTU		9000

struct eth_addr {
	uint8_t addr[ETH_ADDR_LEN];
} __packed;

#define ETH_ADDR_LOCAL_ADMIN	0x02 /* locally assigned */
#define ETH_ADDR_GROUP		0x01 /* multicast or broadcast */
#define ETH_ADDR_BROADCAST {.addr = {0xFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF},}
static const struct eth_addr eth_addr_broadcast = ETH_ADDR_BROADCAST;

static inline uint64_t eth_addr_to_uint64(struct eth_addr *addr)
{
	uint64_t val = 0;
	int i;

	for (i = 0; i < ETH_ADDR_LEN; i++)
		val |= (addr->addr[i] << (i * 8));

	return val;
}

static inline void uint64_to_eth_addr(uint64_t val, struct eth_addr *addr)
{
	int i;

	for (i = 0; i < ETH_ADDR_LEN; i++)
		addr->addr[i] = ((val >> (i * 8)) & 0xff);
}

static inline bool eth_addr_is_multicast(struct eth_addr *addr)
{
	return (addr->addr[0] & ETH_ADDR_GROUP);
}

static inline bool eth_addr_is_zero(struct eth_addr *addr)
{
	int i;

	for (i = 0; i < ETH_ADDR_LEN; i++) {
		if (addr->addr[i] != 0)
			return false;
	}

	return true;
}

struct eth_hdr {
	struct eth_addr	dhost;
	struct eth_addr	shost;
	uint16_t	type;
} __packed;

/*
 *  NOTE: 0x0000-0x05DC (0..1500) are generally IEEE 802.3 length fields.
 *  However, there are some conflicts.
 */

#define	ETHTYPE_8023		0x0004	/* IEEE 802.3 packet */
		   /* 0x0101 .. 0x1FF	   Experimental */
#define	ETHTYPE_PUP		0x0200	/* Xerox PUP protocol - see 0A00 */
#define	ETHTYPE_PUPAT		0x0200	/* PUP Address Translation - see 0A01 */
#define	ETHTYPE_SPRITE		0x0500	/* ??? */
			     /* 0x0400	   Nixdorf */
#define	ETHTYPE_NS		0x0600	/* XNS */
#define	ETHTYPE_NSAT		0x0601	/* XNS Address Translation (3Mb only) */
#define	ETHTYPE_DLOG1	 	0x0660	/* DLOG (?) */
#define	ETHTYPE_DLOG2 		0x0661	/* DLOG (?) */
#define	ETHTYPE_IP		0x0800	/* IP protocol */
#define	ETHTYPE_X75		0x0801	/* X.75 Internet */
#define	ETHTYPE_NBS		0x0802	/* NBS Internet */
#define	ETHTYPE_ECMA		0x0803	/* ECMA Internet */
#define	ETHTYPE_CHAOS 		0x0804	/* CHAOSnet */
#define	ETHTYPE_X25		0x0805	/* X.25 Level 3 */
#define	ETHTYPE_ARP		0x0806	/* Address resolution protocol */
#define	ETHTYPE_NSCOMPAT	0x0807	/* XNS Compatibility */
#define	ETHTYPE_FRARP 		0x0808	/* Frame Relay ARP (RFC1701) */
			     /* 0x081C	   Symbolics Private */
		    /* 0x0888 - 0x088A	   Xyplex */
#define	ETHTYPE_UBDEBUG		0x0900	/* Ungermann-Bass network debugger */
#define	ETHTYPE_IEEEPUP		0x0A00	/* Xerox IEEE802.3 PUP */
#define	ETHTYPE_IEEEPUPAT	0x0A01	/* Xerox IEEE802.3 PUP Address Translation */
#define	ETHTYPE_VINES 		0x0BAD	/* Banyan VINES */
#define	ETHTYPE_VINESLOOP	0x0BAE	/* Banyan VINES Loopback */
#define	ETHTYPE_VINESECHO	0x0BAF	/* Banyan VINES Echo */

/*		       0x1000 - 0x100F	   Berkeley Trailer */
/*
 * The ETHTYPE_NTRAILER packet types starting at ETHTYPE_TRAIL have
 * (type-ETHTYPE_TRAIL)*512 bytes of data followed
 * by an ETH type (as given above) and then the (variable-length) header.
 */
#define	ETHTYPE_TRAIL		0x1000	/* Trailer packet */
#define	ETHTYPE_NTRAILER	16

#define	ETHTYPE_DCA		0x1234	/* DCA - Multicast */
#define	ETHTYPE_VALID 		0x1600	/* VALID system protocol */
#define	ETHTYPE_DOGFIGHT	0x1989	/* Artificial Horizons ("Aviator" dogfight simulator [on Sun]) */
#define	ETHTYPE_RCL		0x1995	/* Datapoint Corporation (RCL lan protocol) */

					/* The following 3C0x types
					   are unregistered: */
#define	ETHTYPE_NBPVCD		0x3C00	/* 3Com NBP virtual circuit datagram (like XNS SPP) not registered */
#define	ETHTYPE_NBPSCD		0x3C01	/* 3Com NBP System control datagram not registered */
#define	ETHTYPE_NBPCREQ		0x3C02	/* 3Com NBP Connect request (virtual cct) not registered */
#define	ETHTYPE_NBPCRSP		0x3C03	/* 3Com NBP Connect response not registered */
#define	ETHTYPE_NBPCC		0x3C04	/* 3Com NBP Connect complete not registered */
#define	ETHTYPE_NBPCLREQ	0x3C05	/* 3Com NBP Close request (virtual cct) not registered */
#define	ETHTYPE_NBPCLRSP	0x3C06	/* 3Com NBP Close response not registered */
#define	ETHTYPE_NBPDG		0x3C07	/* 3Com NBP Datagram (like XNS IDP) not registered */
#define	ETHTYPE_NBPDGB		0x3C08	/* 3Com NBP Datagram broadcast not registered */
#define	ETHTYPE_NBPCLAIM	0x3C09	/* 3Com NBP Claim NetBIOS name not registered */
#define	ETHTYPE_NBPDLTE		0x3C0A	/* 3Com NBP Delete NetBIOS name not registered */
#define	ETHTYPE_NBPRAS		0x3C0B	/* 3Com NBP Remote adaptor status request not registered */
#define	ETHTYPE_NBPRAR		0x3C0C	/* 3Com NBP Remote adaptor response not registered */
#define	ETHTYPE_NBPRST		0x3C0D	/* 3Com NBP Reset not registered */

#define	ETHTYPE_PCS		0x4242	/* PCS Basic Block Protocol */
#define	ETHTYPE_IMLBLDIAG	0x424C	/* Information Modes Little Big LAN diagnostic */
#define	ETHTYPE_DIDDLE		0x4321	/* THD - Diddle */
#define	ETHTYPE_IMLBL		0x4C42	/* Information Modes Little Big LAN */
#define	ETHTYPE_SIMNET		0x5208	/* BBN Simnet Private */
#define	ETHTYPE_DECEXPER	0x6000	/* DEC Unassigned, experimental */
#define	ETHTYPE_MOPDL		0x6001	/* DEC MOP dump/load */
#define	ETHTYPE_MOPRC		0x6002	/* DEC MOP remote console */
#define	ETHTYPE_DECnet		0x6003	/* DEC DECNET Phase IV route */
#define	ETHTYPE_DN		ETHTYPE_DECnet	/* libpcap, tcpdump */
#define	ETHTYPE_LAT		0x6004	/* DEC LAT */
#define	ETHTYPE_DECDIAG		0x6005	/* DEC diagnostic protocol (at interface initialization?) */
#define	ETHTYPE_DECCUST		0x6006	/* DEC customer protocol */
#define	ETHTYPE_SCA		0x6007	/* DEC LAVC, SCA */
#define	ETHTYPE_AMBER		0x6008	/* DEC AMBER */
#define	ETHTYPE_DECMUMPS	0x6009	/* DEC MUMPS */
		    /* 0x6010 - 0x6014	   3Com Corporation */
#define	ETHTYPE_TRANSETHER	0x6558	/* Trans Ether Bridging (RFC1701)*/
#define	ETHTYPE_RAWFR		0x6559	/* Raw Frame Relay (RFC1701) */
#define	ETHTYPE_UBDL		0x7000	/* Ungermann-Bass download */
#define	ETHTYPE_UBNIU		0x7001	/* Ungermann-Bass NIUs */
#define	ETHTYPE_UBDIAGLOOP	0x7002	/* Ungermann-Bass diagnostic/loopback */
#define	ETHTYPE_UBNMC		0x7003	/* Ungermann-Bass ??? (NMC to/from UB Bridge) */
#define	ETHTYPE_UBBST		0x7005	/* Ungermann-Bass Bridge Spanning Tree */
#define	ETHTYPE_OS9		0x7007	/* OS/9 Microware */
#define	ETHTYPE_OS9NET		0x7009	/* OS/9 Net? */
		    /* 0x7020 - 0x7029	   LRT (England) (now Sintrom) */
#define	ETHTYPE_RACAL		0x7030	/* Racal-Interlan */
#define	ETHTYPE_PRIMENTS	0x7031	/* Prime NTS (Network Terminal Service) */
#define	ETHTYPE_CABLETRON	0x7034	/* Cabletron */
#define	ETHTYPE_CRONUSVLN	0x8003	/* Cronus VLN */
#define	ETHTYPE_CRONUS		0x8004	/* Cronus Direct */
#define	ETHTYPE_HP		0x8005	/* HP Probe */
#define	ETHTYPE_NESTAR		0x8006	/* Nestar */
#define	ETHTYPE_ATTSTANFORD	0x8008	/* AT&T/Stanford (local use) */
#define	ETHTYPE_EXCELAN		0x8010	/* Excelan */
#define	ETHTYPE_SG_DIAG		0x8013	/* SGI diagnostic type */
#define	ETHTYPE_SG_NETGAMES	0x8014	/* SGI network games */
#define	ETHTYPE_SG_RESV		0x8015	/* SGI reserved type */
#define	ETHTYPE_SG_BOUNCE	0x8016	/* SGI bounce server */
#define	ETHTYPE_APOLLODOMAIN	0x8019	/* Apollo DOMAIN */
#define	ETHTYPE_TYMSHARE	0x802E	/* Tymeshare */
#define	ETHTYPE_TIGAN		0x802F	/* Tigan, Inc. */
#define	ETHTYPE_REVARP		0x8035	/* Reverse addr resolution protocol */
#define	ETHTYPE_AEONIC		0x8036	/* Aeonic Systems */
#define	ETHTYPE_IPXNEW		0x8037	/* IPX (Novell Netware?) */
#define	ETHTYPE_LANBRIDGE	0x8038	/* DEC LANBridge */
#define	ETHTYPE_DSMD		0x8039	/* DEC DSM/DDP */
#define	ETHTYPE_ARGONAUT	0x803A	/* DEC Argonaut Console */
#define	ETHTYPE_VAXELN		0x803B	/* DEC VAXELN */
#define	ETHTYPE_DECDNS		0x803C	/* DEC DNS Naming Service */
#define	ETHTYPE_ENCRYPT		0x803D	/* DEC Ethernet Encryption */
#define	ETHTYPE_DECDTS		0x803E	/* DEC Distributed Time Service */
#define	ETHTYPE_DECLTM		0x803F	/* DEC LAN Traffic Monitor */
#define	ETHTYPE_DECNETBIOS	0x8040	/* DEC PATHWORKS DECnet NETBIOS Emulation */
#define	ETHTYPE_DECLAST		0x8041	/* DEC Local Area System Transport */
			     /* 0x8042	   DEC Unassigned */
#define	ETHTYPE_PLANNING	0x8044	/* Planning Research Corp. */
		    /* 0x8046 - 0x8047	   AT&T */
#define	ETHTYPE_DECAM		0x8048	/* DEC Availability Manager for Distributed Systems DECamds (but someone at DEC says not) */
#define	ETHTYPE_EXPERDATA	0x8049	/* ExperData */
#define	ETHTYPE_VEXP		0x805B	/* Stanford V Kernel exp. */
#define	ETHTYPE_VPROD		0x805C	/* Stanford V Kernel prod. */
#define	ETHTYPE_ES		0x805D	/* Evans & Sutherland */
#define	ETHTYPE_LITTLE		0x8060	/* Little Machines */
#define	ETHTYPE_COUNTERPOINT	0x8062	/* Counterpoint Computers */
		    /* 0x8065 - 0x8066	   Univ. of Mass @ Amherst */
#define	ETHTYPE_VEECO		0x8067	/* Veeco Integrated Auto. */
#define	ETHTYPE_GENDYN		0x8068	/* General Dynamics */
#define	ETHTYPE_ATT		0x8069	/* AT&T */
#define	ETHTYPE_AUTOPHON	0x806A	/* Autophon */
#define	ETHTYPE_COMDESIGN	0x806C	/* ComDesign */
#define	ETHTYPE_COMPUGRAPHIC	0x806D	/* Compugraphic Corporation */
		    /* 0x806E - 0x8077	   Landmark Graphics Corp. */
#define	ETHTYPE_MATRA		0x807A	/* Matra */
#define	ETHTYPE_DDE		0x807B	/* Dansk Data Elektronik */
#define	ETHTYPE_MERIT		0x807C	/* Merit Internodal (or Univ of Michigan?) */
		    /* 0x807D - 0x807F	   Vitalink Communications */
#define	ETHTYPE_VLTLMAN		0x8080	/* Vitalink TransLAN III Management */
		    /* 0x8081 - 0x8083	   Counterpoint Computers */
		    /* 0x8088 - 0x808A	   Xyplex */
#define	ETHTYPE_ATALK		0x809B	/* AppleTalk */
#define	ETHTYPE_AT		ETHTYPE_ATALK		/* old NetBSD */
#define	ETHTYPE_APPLETALK	ETHTYPE_ATALK		/* HP-UX */
		    /* 0x809C - 0x809E	   Datability */
#define	ETHTYPE_SPIDER		0x809F	/* Spider Systems Ltd. */
			     /* 0x80A3	   Nixdorf */
		    /* 0x80A4 - 0x80B3	   Siemens Gammasonics Inc. */
		    /* 0x80C0 - 0x80C3	   DCA (Digital Comm. Assoc.) Data Exchange Cluster */
		    /* 0x80C4 - 0x80C5	   Banyan Systems */
#define	ETHTYPE_PACER		0x80C6	/* Pacer Software */
#define	ETHTYPE_APPLITEK	0x80C7	/* Applitek Corporation */
		    /* 0x80C8 - 0x80CC	   Intergraph Corporation */
		    /* 0x80CD - 0x80CE	   Harris Corporation */
		    /* 0x80CF - 0x80D2	   Taylor Instrument */
		    /* 0x80D3 - 0x80D4	   Rosemount Corporation */
#define	ETHTYPE_SNA		0x80D5	/* IBM SNA Services over Ethernet */
#define	ETHTYPE_VARIAN		0x80DD	/* Varian Associates */
		    /* 0x80DE - 0x80DF	   TRFS (Integrated Solutions Transparent Remote File System) */
		    /* 0x80E0 - 0x80E3	   Allen-Bradley */
		    /* 0x80E4 - 0x80F0	   Datability */
#define	ETHTYPE_RETIX		0x80F2	/* Retix */
#define	ETHTYPE_AARP		0x80F3	/* AppleTalk AARP */
		    /* 0x80F4 - 0x80F5	   Kinetics */
#define	ETHTYPE_APOLLO		0x80F7	/* Apollo Computer */
#define ETHTYPE_VLAN		0x8100	/* IEEE 802.1Q VLAN tagging (XXX conflicts) */
		    /* 0x80FF - 0x8101	   Wellfleet Communications (XXX conflicts) */
#define	ETHTYPE_BOFL		0x8102	/* Wellfleet; BOFL (Breath OF Life) pkts [every 5-10 secs.] */
#define	ETHTYPE_WELLFLEET	0x8103	/* Wellfleet Communications */
		    /* 0x8107 - 0x8109	   Symbolics Private */
#define	ETHTYPE_TALARIS		0x812B	/* Talaris */
#define	ETHTYPE_WATERLOO	0x8130	/* Waterloo Microsystems Inc. (XXX which?) */
#define	ETHTYPE_HAYES		0x8130	/* Hayes Microcomputers (XXX which?) */
#define	ETHTYPE_VGLAB		0x8131	/* VG Laboratory Systems */
		    /* 0x8132 - 0x8137	   Bridge Communications */
#define	ETHTYPE_IPX		0x8137	/* Novell (old) NetWare IPX (ECONFIG E option) */
#define	ETHTYPE_NOVELL		0x8138	/* Novell, Inc. */
		    /* 0x8139 - 0x813D	   KTI */
#define	ETHTYPE_MUMPS		0x813F	/* M/MUMPS data sharing */
#define	ETHTYPE_AMOEBA		0x8145	/* Vrije Universiteit (NL) Amoeba 4 RPC (obsolete) */
#define	ETHTYPE_FLIP		0x8146	/* Vrije Universiteit (NL) FLIP (Fast Local Internet Protocol) */
#define	ETHTYPE_VURESERVED	0x8147	/* Vrije Universiteit (NL) [reserved] */
#define	ETHTYPE_LOGICRAFT	0x8148	/* Logicraft */
#define	ETHTYPE_NCD		0x8149	/* Network Computing Devices */
#define	ETHTYPE_ALPHA		0x814A	/* Alpha Micro */
#define	ETHTYPE_SNMP		0x814C	/* SNMP over Ethernet (see RFC1089) */
		    /* 0x814D - 0x814E	   BIIN */
#define	ETHTYPE_TEC		0x814F	/* Technically Elite Concepts */
#define	ETHTYPE_RATIONAL	0x8150	/* Rational Corp */
		    /* 0x8151 - 0x8153	   Qualcomm */
		    /* 0x815C - 0x815E	   Computer Protocol Pty Ltd */
		    /* 0x8164 - 0x8166	   Charles River Data Systems */
#define	ETHTYPE_XTP		0x817D	/* Protocol Engines XTP */
#define	ETHTYPE_SGITW		0x817E	/* SGI/Time Warner prop. */
#define	ETHTYPE_HIPPI_FP	0x8180	/* HIPPI-FP encapsulation */
#define	ETHTYPE_STP		0x8181	/* Scheduled Transfer STP, HIPPI-ST */
		    /* 0x8182 - 0x8183	   Reserved for HIPPI-6400 */
		    /* 0x8184 - 0x818C	   SGI prop. */
#define	ETHTYPE_MOTOROLA	0x818D	/* Motorola */
#define	ETHTYPE_NETBEUI		0x8191	/* PowerLAN NetBIOS/NetBEUI (PC) */
		    /* 0x819A - 0x81A3	   RAD Network Devices */
		    /* 0x81B7 - 0x81B9	   Xyplex */
		    /* 0x81CC - 0x81D5	   Apricot Computers */
		    /* 0x81D6 - 0x81DD	   Artisoft Lantastic */
		    /* 0x81E6 - 0x81EF	   Polygon */
		    /* 0x81F0 - 0x81F2	   Comsat Labs */
		    /* 0x81F3 - 0x81F5	   SAIC */
		    /* 0x81F6 - 0x81F8	   VG Analytical */
		    /* 0x8203 - 0x8205	   QNX Software Systems Ltd. */
		    /* 0x8221 - 0x8222	   Ascom Banking Systems */
		    /* 0x823E - 0x8240	   Advanced Encryption Systems */
		    /* 0x8263 - 0x826A	   Charles River Data Systems */
		    /* 0x827F - 0x8282	   Athena Programming */
		    /* 0x829A - 0x829B	   Inst Ind Info Tech */
		    /* 0x829C - 0x82AB	   Taurus Controls */
		    /* 0x82AC - 0x8693	   Walker Richer & Quinn */
#define	ETHTYPE_ACCTON		0x8390	/* Accton Technologies (unregistered) */
#define	ETHTYPE_TALARISMC	0x852B	/* Talaris multicast */
#define	ETHTYPE_KALPANA		0x8582	/* Kalpana */
		    /* 0x8694 - 0x869D	   Idea Courier */
		    /* 0x869E - 0x86A1	   Computer Network Tech */
		    /* 0x86A3 - 0x86AC	   Gateway Communications */
#define	ETHTYPE_SECTRA		0x86DB	/* SECTRA */
#define	ETHTYPE_IPV6		0x86DD	/* IP protocol version 6 */
#define	ETHTYPE_DELTACON	0x86DE	/* Delta Controls */
#define	ETHTYPE_ATOMIC		0x86DF	/* ATOMIC */
		    /* 0x86E0 - 0x86EF	   Landis & Gyr Powers */
		    /* 0x8700 - 0x8710	   Motorola */
#define	ETHTYPE_RDP		0x8739	/* Control Technology Inc. RDP Without IP */
#define	ETHTYPE_MICP		0x873A	/* Control Technology Inc. Mcast Industrial Ctrl Proto. */
		    /* 0x873B - 0x873C	   Control Technology Inc. Proprietary */
#define	ETHTYPE_TCPCOMP		0x876B	/* TCP/IP Compression (RFC1701) */
#define	ETHTYPE_IPAS		0x876C	/* IP Autonomous Systems (RFC1701) */
#define	ETHTYPE_SECUREDATA	0x876D	/* Secure Data (RFC1701) */
#define	ETHTYPE_FLOWCONTROL	0x8808	/* 802.3x flow control packet */
#define	ETHTYPE_SLOW		0x8809	/* 802.3ad link aggregation (LACP) */
#define	ETHTYPE_PPP		0x880B	/* PPP (obsolete by PPPoE) */
#define	ETHTYPE_HITACHI		0x8820	/* Hitachi Cable (Optoelectronic Systems Laboratory) */
#define	ETHTYPE_MPLS		0x8847	/* MPLS Unicast */
#define	ETHTYPE_MPLS_MCAST	0x8848	/* MPLS Multicast */
#define	ETHTYPE_AXIS		0x8856	/* Axis Communications AB proprietary bootstrap/config */
#define	ETHTYPE_PPPOEDISC	0x8863	/* PPP Over Ethernet Discovery Stage */
#define	ETHTYPE_PPPOE		0x8864	/* PPP Over Ethernet Session Stage */
#define	ETHTYPE_LANPROBE	0x8888	/* HP LanProbe test? */
#define	ETHTYPE_PAE		0x888e	/* EAPOL PAE/802.1x */
#define ETHTYPE_8021AB		0x88cc	/* Link Layer Discovery Protocol (IEEE 802.1AB) */
#define	ETHTYPE_LOOPBACK	0x9000	/* Loopback: used to test interfaces */
#define	ETHTYPE_LBACK		ETHTYPE_LOOPBACK	/* DEC MOP loopback */
#define	ETHTYPE_XNSSM		0x9001	/* 3Com (Formerly Bridge Communications), XNS Systems Management */
#define	ETHTYPE_TCPSM		0x9002	/* 3Com (Formerly Bridge Communications), TCP/IP Systems Management */
#define	ETHTYPE_BCLOOP		0x9003	/* 3Com (Formerly Bridge Communications), loopback detection */
#define	ETHTYPE_DEBNI		0xAAAA	/* DECNET? Used by VAX 6220 DEBNI */
#define	ETHTYPE_SONIX		0xFAF5	/* Sonix Arpeggio */
#define	ETHTYPE_VITAL		0xFF00	/* BBN VITAL-LanBridge cache wakeups */
		    /* 0xFF00 - 0xFFOF	   ISC Bunker Ramo */

#define	ETHTYPE_MAX		0xFFFF	/* Maximum valid ethernet type, reserved */


