/*
 * stat.c - support for statistics and counters
 */

#include <string.h>
#include <stdio.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <base/tcache.h>
#include <base/thread.h>
#include <runtime/thread.h>
#include <runtime/udp.h>
#include <runtime/tcp.h>

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
	"rq_overflow",

	/* network stack counters */
	"rx_bytes",
	"rx_packets",
	"tx_bytes",
	"tx_packets",
	"drops",
	"rx_tcp_in_order",
	"rx_tcp_out_of_order",
	"rx_tcp_text_cycles",
	"txq_overflow",

	/* directpath counters */
	"flow_steering_cycles",
	"rx_hw_drop",

};

static const char *tc_stat_names[] = {
	"mag_free",
	"mag_alloc",
	"pool_alloc",
	"pool_free"
};


/* must correspond exactly to STAT_* enum definitions in defs.h */
BUILD_ASSERT(ARRAY_SIZE(stat_names) == STAT_NR);

static int append_stat(char **pos, char *end, const char *name, uint64_t val)
{
	int ret = snprintf(*pos, end - *pos, "%s:%ld,", name, val);
	if (ret < 0)
		return -EINVAL;
	if (ret >= end - *pos)
		return -E2BIG;
	*pos += ret;
	return 0;
}

static ssize_t stat_write_buf(char *buf, size_t len)
{
	uint64_t stats[STAT_NR], tc_stats[4];
	char *pos = buf, *end = buf + len;
	int i, j, ret;

	memset(stats, 0, sizeof(stats));
	memset(tc_stats, 0, sizeof(tc_stats));

	/* gather stats from each kthread */
	for (i = 0; i < maxks; i++) {
		for (j = 0; j < STAT_NR; j++)
			stats[j] += ks[i]->stats[j];
	}

	for_each_thread(i) {
		tc_stats[0] += perthread_get_remote(mag_free, i);
		tc_stats[1] += perthread_get_remote(mag_alloc, i);
		tc_stats[2] += perthread_get_remote(pool_alloc, i);
		tc_stats[3] += perthread_get_remote(pool_free, i);
	}

	/* write out the stats to the buffer */
	for (j = 0; j < STAT_NR; j++) {
		ret = append_stat(&pos, end, stat_names[j], stats[j]);
		if (ret)
			return ret;
	}

	for (j = 0; j < ARRAY_SIZE(tc_stats); j++) {
		ret = append_stat(&pos, end, tc_stat_names[j], tc_stats[j]);
		if (ret)
			return ret;
	}

	/* report the clock rate */
	ret = append_stat(&pos, end, "cycles_per_us", cycles_per_us);
	if (ret)
		return ret;

	ret = append_stat(&pos, end, "tsc", rdtsc());
	if (ret)
		return ret;

	pos[-1] = '\0'; /* clip off last ',' */
	return pos - buf;
}

static void stat_tcp_worker(void *arg)
{
	struct {
		size_t resp_size;
		char buf[65535];
	} resp;
	ssize_t ret, len, done;
	tcpconn_t *c = arg;

	while (true) {
		ret = tcp_read(c, resp.buf, sizeof(resp.buf));
		if (ret <= 0)
			goto done;

		len = stat_write_buf(resp.buf, sizeof(resp.buf));
		if (len < 0) {
			WARN();
			continue;
		}

		/* start with the size of the response body */
		resp.resp_size = len;

		done = 0;
		do {
			ret = tcp_write(c, (char *)&resp + done, sizeof(size_t) + len - done);
			if (ret < 0) {
				WARN_ON(ret != -EPIPE && ret != -ECONNRESET);
				goto done;
			}
			done += ret;
		} while (done < sizeof(size_t) + len);
	}

done:
	tcp_close(c);
	return;
}

static void stat_tcp_server(void *arg)
{
	struct netaddr laddr;
	tcpconn_t *c;
	tcpqueue_t *q;
	int ret;

	laddr.ip = 0;
	laddr.port = STAT_PORT;

	ret = tcp_listen(laddr, 4096, &q);
	BUG_ON(ret);

	while (true) {
		ret = tcp_accept(q, &c);
		BUG_ON(ret);
		ret = thread_spawn(stat_tcp_worker, c);
		WARN_ON(ret);
	}
}

static void stat_worker_udp(void *arg)
{
	const size_t cmd_len = strlen("stat");
	size_t payload_size = udp_get_payload_size();
	char buf[payload_size];
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
		ret = udp_read_from(c, buf, payload_size, &raddr);
		if (ret < cmd_len)
			continue;
		if (strncmp(buf, "stat", cmd_len) != 0)
			continue;

		len = stat_write_buf(buf, payload_size);
		if (len < 0) {
			log_err("stat: couldn't generate stat buffer");
			continue;
		}
		assert(len <= payload_size);

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
	int ret;

	ret = thread_spawn(stat_tcp_server, NULL);
	if (ret)
		return ret;

	return thread_spawn(stat_worker_udp, NULL);
}
