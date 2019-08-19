/*
 * main.c - initialization and main dataplane loop for the iokernel
 */

#include <rte_ethdev.h>
#include <rte_lcore.h>

#include <base/init.h>
#include <base/log.h>
#include <base/stddef.h>

#include "defs.h"
#include "sched.h"

#define LOG_INTERVAL_US		(1000 * 1000)
struct iokernel_cfg cfg;
struct dataplane dp;

bool allowed_cores_supplied;
DEFINE_BITMAP(input_allowed_cores, NCPU);

struct init_entry {
	const char *name;
	int (*init)(void);
};

#define IOK_INITIALIZER(name) \
	{__cstr(name), &name ## _init}

/* iokernel subsystem initialization */
static const struct init_entry iok_init_handlers[] = {
	/* base */
	IOK_INITIALIZER(base),

	/* general iokernel */
	IOK_INITIALIZER(ksched),
	IOK_INITIALIZER(sched),
	IOK_INITIALIZER(simple),
	IOK_INITIALIZER(mis),
	IOK_INITIALIZER(numa),
	IOK_INITIALIZER(ias),

	/* control plane */
	IOK_INITIALIZER(control),

	/* data plane */
	IOK_INITIALIZER(dpdk),
	IOK_INITIALIZER(rx),
	IOK_INITIALIZER(tx),
	IOK_INITIALIZER(dp_clients),
	IOK_INITIALIZER(dpdk_late),
};

static int run_init_handlers(const char *phase, const struct init_entry *h,
		int nr)
{
	int i, ret;

	log_debug("entering '%s' init phase", phase);
	for (i = 0; i < nr; i++) {
		log_debug("init -> %s", h[i].name);
		ret = h[i].init();
		if (ret) {
			log_debug("failed, ret = %d", ret);
			return ret;
		}
	}

	return 0;
}

/*
 * The main dataplane thread.
 */
void dataplane_loop(void)
{
	bool work_done;
#ifdef STATS
	uint64_t next_log_time = microtime();
#endif

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	if (rte_eth_dev_socket_id(dp.port) > 0
			&& rte_eth_dev_socket_id(dp.port) != (int) rte_socket_id())
		log_warn("main: port %u is on remote NUMA node to polling thread.\n\t"
				"Performance will not be optimal.", dp.port);

	log_info("main: core %u running dataplane. [Ctrl+C to quit]",
			rte_lcore_id());
	fflush(stdout);

	/* run until quit or killed */
	for (;;) {
		work_done = false;

		/* handle a burst of ingress packets */
		work_done |= rx_burst();

		/* adjust core assignments */
		sched_poll();

		/* drain overflow completion queues */
		work_done |= tx_drain_completions();

		/* send a burst of egress packets */
		work_done |= tx_burst();

		/* process a batch of commands from runtimes */
		work_done |= commands_rx();

		/* handle control messages */
		if (!work_done)
			dp_clients_rx_control_lrpcs();

		STAT_INC(BATCH_TOTAL, IOKERNEL_RX_BURST_SIZE);

#ifdef STATS
		if (microtime() > next_log_time) {
			print_stats();
			dpdk_print_eth_stats();
			next_log_time += LOG_INTERVAL_US;
		}
#endif
	}
}

static void print_usage(void)
{
	printf("usage: POLICY [noht/core_list]\n");
	printf("\tsimple: the standard, basic scheduler policy\n");
	printf("\tmis: a policy aware of microarchitectural interference\n");
	printf("\tnuma: a policy aware of NUMA architectures\n");
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc >= 3) {
		if (!strcmp(argv[2], "noht"))
			cfg.noht = true;
		else {
			allowed_cores_supplied = true;
			ret = string_to_bitmap(argv[2], input_allowed_cores, NCPU);
			if (ret) {
				fprintf(stderr, "invalid cpu list: %s\n", argv[1]);
				fprintf(stderr, "example list: 0-24,26-48:2,49-255\n");
				return ret;
			}
		}
	}

	if (argc >= 2) {
		if (!strcmp(argv[1], "simple")) {
			sched_ops = &simple_ops;
		} else if (!strcmp(argv[1], "mis")) {
			sched_ops = &mis_ops;
		} else if (!strcmp(argv[1], "numa")) {
			sched_ops = &numa_ops;
		} else if (!strcmp(argv[1], "ias")) {
			if (cfg.noht) {
				fprintf(stderr, "ias can't be used w/ noht\n");
				return -EINVAL;
			}
			sched_ops = &ias_ops;
		} else {
			print_usage();
			return -EINVAL;
		}
	} else {
		sched_ops = &simple_ops;
	}

	ret = run_init_handlers("iokernel", iok_init_handlers,
			ARRAY_SIZE(iok_init_handlers));
	if (ret)
		return ret;

	dataplane_loop();
	return 0;
}
