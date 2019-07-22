/*
 * stat.c - support for statistics and counters
 */

#include <string.h>
#include <stdio.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <runtime/thread.h>
#include <runtime/udp.h>

#include "defs.h"

/* port 40 is permanently reserved, so should be fine for now */
#define STAT_PORT	40

static const char *stat_names[] = {
	/* scheduler counters */
	"reschedules",
	"sched_cycles",
	"program_cycles",
	"threads_stolen",
	"softirqs_stolen",
	"softirqs_local",
	"parks",
	"preemptions",
	"core_migrations",
	"local_runs",
	"remote_runs",
	"local_wakes",
	"remote_wakes",

	/* network stack counters */
	"rx_bytes",
	"rx_packets",
	"tx_bytes",
	"tx_packets",
	"drops",
	"rx_tcp_in_order",
	"rx_tcp_out_of_order",
	"rx_tcp_text_cycles",

	"rx_hw_drop",

};

/* must correspond exactly to STAT_* enum definitions in defs.h */
BUILD_ASSERT(ARRAY_SIZE(stat_names) == STAT_NR);

static int append_stat(char *pos, size_t len, const char *name, uint64_t val)
{
	return snprintf(pos, len, "%s:%ld,", name, val);
}

static ssize_t stat_write_buf(char *buf, size_t len)
{
	uint64_t stats[STAT_NR];
	char *pos = buf, *end = buf + len;
	int i, j, ret;

	memset(stats, 0, sizeof(stats));

	/* gather stats from each kthread */
	for (i = 0; i < nrks; i++) {
		for (j = 0; j < STAT_NR; j++)
			stats[j] += ks[i]->stats[j];
	}

	/* write out the stats to the buffer */
	for (j = 0; j < STAT_NR; j++) {
		ret = append_stat(pos, end - pos, stat_names[j], stats[j]);
		if (ret < 0) {
			return -EINVAL;
		} else if (ret >= end - pos) {
			return -E2BIG;
		}

		pos += ret;
	}

	/* report the clock rate */
	ret = append_stat(pos, end - pos, "cycles_per_us", cycles_per_us);
	if (ret < 0) {
		return -EINVAL;
	} else if (ret >= end - pos) {
		return -E2BIG;
	}
	pos += ret;

	ret = append_stat(pos, end - pos, "tsc", rdtsc());
	if (ret < 0) {
		return -EINVAL;
	} else if (ret >= end - pos) {
		return -E2BIG;
	}

	pos += ret;
	pos[-1] = '\0'; /* clip off last ',' */
	return pos - buf;
}

static void stat_worker(void *arg)
{
	const size_t cmd_len = strlen("stat");
	char buf[UDP_MAX_PAYLOAD];
	struct netaddr laddr, raddr;
	udpconn_t *c;
	ssize_t ret, len;

	laddr.ip = 0;
	laddr.port = STAT_PORT;

	ret = udp_listen(laddr, &c);
	if (ret) {
		log_err("stat: udp_listen failed, ret = %ld", ret);
		return;
	}

	while (true) {
		ret = udp_read_from(c, buf, UDP_MAX_PAYLOAD, &raddr);
		if (ret < cmd_len)
			continue;
		if (strncmp(buf, "stat", cmd_len) != 0)
			continue;

		len = stat_write_buf(buf, UDP_MAX_PAYLOAD);
		if (len < 0) {
			log_err("stat: couldn't generate stat buffer");
			continue;
		}
		assert(len <= UDP_MAX_PAYLOAD);

		ret = udp_write_to(c, buf, len, &raddr);
		WARN_ON(ret != len);
	}
}

/**
 * stat_init_late - starts the stat responder thread
 *
 * Returns 0 if succesful.
 */
int stat_init_late(void)
{
	return thread_spawn(stat_worker, NULL);
}
