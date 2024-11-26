/*
 * icmp.c - support for Internet Control Message Protocol (ICMP)
 */

#include <string.h>

#include <asm/chksum.h>
#include <base/compiler.h>
#include <base/log.h>
#include <net/icmp.h>
#include <net/ping.h>

#include "defs.h"

static void net_rx_icmp_echo(struct mbuf *m_in,
		const struct icmp_pkt *in_icmp_pkt, const struct ip_hdr *in_iphdr,
		uint16_t len)
{
	struct mbuf *m;
	struct icmp_hdr *out_icmp_hdr;

	log_debug("icmp: responding to icmp echo request");

	m = net_tx_alloc_mbuf(ip_headroom());
	if (unlikely(!m)) {
		mbuf_drop(m_in);
		return;
	}

	/* copy incoming ICMP hdr and data, set type and checksum */
	out_icmp_hdr = (struct icmp_hdr *)mbuf_put(m, len);
	memcpy(out_icmp_hdr, in_icmp_pkt, len);
	out_icmp_hdr->type = ICMP_ECHOREPLY;
	out_icmp_hdr->chksum = 0;
	out_icmp_hdr->chksum = chksum_internet((char *)out_icmp_hdr, len);

	/* send the echo reply */
	net_tx_ip_or_free(m, IPPROTO_ICMP, ntoh32(in_iphdr->saddr));
	mbuf_free(m_in);
}

static void net_rx_icmp_echo_reply(struct mbuf *m,
		const struct icmp_pkt *icmp_pkt, uint16_t len)
{
	struct ping_payload *payload;

	log_debug("icmp: received icmp echo reply");

	payload = mbuf_pull_hdr_or_null(m, *payload);
	if (unlikely(!payload))
		goto drop;

	net_recv_ping(payload, icmp_pkt);

drop:
	mbuf_free(m);
}

void net_rx_icmp(struct mbuf *m, const struct ip_hdr *iphdr, uint16_t len)
{
	struct icmp_pkt *icmp_pkt;

	icmp_pkt = (struct icmp_pkt *)mbuf_pull_or_null(m, ICMP_MINLEN);
	if (unlikely(!icmp_pkt))
		goto drop;

	switch (icmp_pkt->hdr.type) {
	case ICMP_ECHO:
		net_rx_icmp_echo(m, icmp_pkt, iphdr, len);
		break;
	case ICMP_ECHOREPLY:
		net_rx_icmp_echo_reply(m, icmp_pkt, len);
		break;
	default:
		log_err("icmp: type %d not yet supported", icmp_pkt->hdr.type);
		goto drop;
	}

	return;

drop:
	mbuf_drop(m);
}

int net_tx_icmp(struct mbuf *m, uint8_t type, uint8_t code, uint32_t daddr,
		uint16_t id, uint16_t seq)
{
	struct icmp_pkt *icmp_pkt;

	log_debug("icmp: sending icmp with type %u, code %u", type, code);

	/* populate ICMP header */
	icmp_pkt = (struct icmp_pkt *)mbuf_push(m, ICMP_MINLEN);
	icmp_pkt->hdr.type = ICMP_ECHO;
	icmp_pkt->hdr.code = 0;
	icmp_pkt->icmp_id = id;
	icmp_pkt->icmp_seq = seq;
	icmp_pkt->hdr.chksum = 0;
	icmp_pkt->hdr.chksum = chksum_internet((char *)icmp_pkt, mbuf_length(m));

	return net_tx_ip(m, IPPROTO_ICMP, daddr);
}
