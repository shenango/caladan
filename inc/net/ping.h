#pragma once

#include <sys/time.h>

#include <net/icmp.h>

struct ping_payload {
	struct timeval tx_time;
};

int net_ping_init();
void net_send_ping(uint16_t seq_num, uint32_t daddr);
void net_recv_ping(const struct ping_payload *payload,
		const struct icmp_pkt *icmp_pkt);
