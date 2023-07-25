#include <pthread.h>
#include <stdio.h>

#include <base/log.h>

#include "sched.h"
#include "defs.h"

#define BUFSIZE 4096

uint64_t stats[NR_STATS];

static const char *stat_names[] = {
	"RX_UNREGISTERED_MAC",
	"RX_UNICAST_FAIL",
	"RX_BROADCAST_FAIL",
	"RX_UNHANDLED",
	"PARKED_THREAD_BUSY_WAKE",
	"PARK_FAST_REWAKE",
	"TX_COMPLETION_OVERFLOW",
	"TX_COMPLETION_FAIL",
	"RX_PULLED",
	"COMMANDS_PULLED",
	"COMPLETION_DRAINED",
	"COMPLETION_ENQUEUED",
	"LOOPS",
	"TX_PULLED",
	"TX_BACKPRESSURE",
	"SCHED_RUN",
	"PREEMPT",
	"RX_REFILL",
	"DIRECTPATH_EVENTS",
};

BUILD_ASSERT(ARRAY_SIZE(stat_names) == NR_STATS);

static void print_stats(void)
{
	int i;
	uint64_t now, cur_stats[NR_STATS];
	static uint64_t last_stats[NR_STATS];

	barrier();
	now = rdtsc();
	for (i = 0; i < NR_STATS; i++)
		cur_stats[i] = ACCESS_ONCE(stats[i]);
	barrier();

	for (i = 0; i < NR_STATS; i++) {
		printf("%lu %s %lu\n", now, stat_names[i],
			   cur_stats[i] - last_stats[i]);
		last_stats[i] = cur_stats[i];
	}

	fflush(stdout);
}

static void *print_stats_thread(void *arg)
{
	cpu_set_t cpuset;
	int ret;

	CPU_ZERO(&cpuset);
	CPU_SET(sched_ctrl_core, &cpuset);

	ret = sched_setaffinity(thread_gettid(), sizeof(cpu_set_t), &cpuset);
	if (ret < 0) {
		log_warn("log: failed to pin to contorl core with err %d", errno);
		return NULL;
	}

	while (true) {
		print_stats();
		sleep(1);
	}
}

int stats_init(void)
{
	pthread_t tid;

	return pthread_create(&tid, NULL, print_stats_thread, NULL);
}

