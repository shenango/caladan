/*
 * main.c - initialization and main dataplane loop for the iokernel
 */

#include <rte_ethdev.h>
#include <rte_lcore.h>

#include <base/init.h>
#include <base/log.h>
#include <base/stddef.h>

#include <unistd.h>

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
	IOK_INITIALIZER(hw_timestamp),

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
	printf("usage: POLICY [noht/core_list/nobw/mutualpair]\n");
	printf("\tsimple: the standard, basic scheduler policy\n");
	printf("\tias: a policy aware of CPU interference\n");
	printf("\tnuma: a policy aware of NUMA architectures\n");
}

int main(int argc, char *argv[])
{
	int i, ret;

	if (getuid() != 0) {
		fprintf(stderr, "Error: please run as root\n");
		return -EPERM;
	}

	if (argc >= 2) {
		if (!strcmp(argv[1], "simple")) {
			sched_ops = &simple_ops;
		} else if (!strcmp(argv[1], "numa")) {
			sched_ops = &numa_ops;
		} else if (!strcmp(argv[1], "ias")) {
			sched_ops = &ias_ops;
		} else {
			print_usage();
			return -EINVAL;
		}
	} else {
		sched_ops = &simple_ops;
	}

	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "noht")) {
			cfg.noht = true;
		} else if (!strcmp(argv[i], "nobw")) {
			cfg.nobw = true;
		} else if (!strcmp(argv[i], "no_hw_qdel")) {
			cfg.no_hw_qdel = true;
		} else if (!strcmp(argv[i], "selfpair")) {
			cfg.ias_prefer_selfpair = true;
		} else if (!strcmp(argv[i], "bwlimit")) {
			if (i == argc - 1) {
				fprintf(stderr, "missing bwlimit argument\n");
				return -EINVAL;
			}
			cfg.ias_bw_limit = atof(argv[++i]);
			log_info("setting bwlimit to %.5f", cfg.ias_bw_limit);
		} else if (!strcmp(argv[i], "nicpci")) {
			if (i == argc - 1) {
				fprintf(stderr, "missing nicpci argument\n");
				return -EINVAL;
			}
			nic_pci_addr_str = argv[++i];
			ret = pci_str_to_addr(nic_pci_addr_str, &nic_pci_addr);
			if (ret) {
				log_err("invalid pci address: %s", nic_pci_addr_str);
				return -EINVAL;
			}
		} else if (string_to_bitmap(argv[i], input_allowed_cores, NCPU)) {
			fprintf(stderr, "invalid cpu list: %s\n", argv[i]);
			fprintf(stderr, "example list: 0-24,26-48:2,49-255\n");
			return -EINVAL;
		} else {
			allowed_cores_supplied = true;
		}
	}

	ret = run_init_handlers("iokernel", iok_init_handlers,
			ARRAY_SIZE(iok_init_handlers));
	if (ret)
		return ret;

	dataplane_loop();
	return 0;
}
