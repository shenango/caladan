/*
 * ping.c - simple ping utility
 */

#include <sys/time.h>

#include <base/compiler.h>
#include <base/log.h>
#include <net/icmp.h>
#include <net/ping.h>

#include "defs.h"

static uint16_t ping_id;

int net_ping_init(void)
{
	ping_id = rand();
	return 0;
}

void net_send_ping(uint16_t seq_num, uint32_t daddr)
{
	struct mbuf *m;
	struct ping_payload *payload;

	log_debug("ping: sending ping with id %u, seq_num %u to %u", ping_id,
			seq_num, daddr);

	m = net_tx_alloc_mbuf();
	if (unlikely(!m))
		return;

	/* add send timestamp to payload */
	payload = mbuf_push_hdr(m, struct ping_payload);
	gettimeofday(&payload->tx_time, NULL);

	if (unlikely(net_tx_icmp(m, ICMP_ECHO, 0, daddr, ping_id, seq_num) != 0))
		mbuf_free(m);
}

/*
 * Subtract 2 timeval structs: out -= in. Assume out >= in.
 */
static void timeval_subtract(struct timeval *out, const struct timeval *in)
{
	if ((out->tv_usec -= in->tv_usec) < 0) {
		--out->tv_sec;
		out->tv_usec += 1000000;
	}
	out->tv_sec -= in->tv_sec;
}

void net_recv_ping(const struct ping_payload *payload,
		const struct icmp_pkt *icmp_pkt)
{
	struct timeval tmp_time;
	uint32_t latency_us;

	if (icmp_pkt->icmp_id != ping_id) {
		/* this ICMP pkt is not for us */
		return;
	}

	/* determine latency */
	gettimeofday(&tmp_time, NULL);
	timeval_subtract(&tmp_time, &payload->tx_time);
	latency_us = tmp_time.tv_sec * 1000000 + tmp_time.tv_usec;

	log_debug("ping: received ping with seq_num %u, latency %u us",
			icmp_pkt->icmp_seq, latency_us);
}
