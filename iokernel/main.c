/*
 * main.c - initialization and main dataplane loop for the iokernel
 */

#include <rte_ethdev.h>
#include <rte_lcore.h>

#include <base/init.h>
#include <base/log.h>
#include <base/stddef.h>

#include <sys/utsname.h>

#include <unistd.h>

#include "defs.h"
#include "sched.h"

#define LOG_INTERVAL_US		(1000 * 1000)
struct iokernel_cfg cfg;
struct dataplane dp;

unsigned int vfio_prealloc_nrqs = 1;
bool vfio_prealloc_rmp = true;
uint32_t nr_vfio_prealloc;
bool stat_logging;
bool allowed_cores_supplied;
DEFINE_BITMAP(input_allowed_cores, NCPU);

struct init_entry {
	const char *name;
	int (*init)(void);
};

pthread_barrier_t init_barrier;

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
	IOK_INITIALIZER(proc_timer),

	/* control plane */
	IOK_INITIALIZER(control),

	/* data plane */
	IOK_INITIALIZER(dpdk),
	IOK_INITIALIZER(rx),
	IOK_INITIALIZER(tx),
	IOK_INITIALIZER(dp_clients),
	IOK_INITIALIZER(dpdk_late),
	IOK_INITIALIZER(directpath),
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

	if (stat_logging) {
		ret = stats_init();
		if (ret)
			return ret;
	}

	return 0;
}

static void dataplane_loop_vfio(void)
{
	bool work_done;

	log_info("main: core %u running dataplane. [Ctrl+C to quit]",
			rte_lcore_id());
	fflush(stdout);

	/* run until quit or killed */
	for (;;) {
		work_done = false;

		/* adjust core assignments */
		sched_poll();

		work_done |= directpath_poll();

		/* handle control messages */
		if (!work_done)
			dp_clients_rx_control_lrpcs();

		STAT_INC(LOOPS, 1);
	}
}

/*
 * The main dataplane thread.
 */
void dataplane_loop(void)
{
	bool work_done;
#if 0
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

		STAT_INC(LOOPS, 1);

#if 0
		if (microtime() > next_log_time) {
			dpdk_print_eth_stats();
			next_log_time += LOG_INTERVAL_US;
		}
#endif
	}
}

static void print_usage(void)
{
	printf("usage: POLICY [noht/core_list/nobw/mutualpair]\n");
	printf("\tsimple: a simplified scheduler policy intended for testing\n");
	printf("\tias: the Caladan scheduler policy (manages CPU interference)\n");
	printf("\tnuma: an incomplete and experimental policy for NUMA architectures\n");
}

int main(int argc, char *argv[])
{
	int i, ret;
	struct utsname utsname;

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
		sched_ops = &ias_ops;
	}

	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "noht")) {
			cfg.noht = true;
		} else if (!strcmp(argv[i], "nobw")) {
			cfg.nobw = true;
		} else if (!strcmp(argv[i], "no_hw_qdel")) {
			cfg.no_hw_qdel = true;
		} else if (!strcmp(argv[i], "stats")) {
			stat_logging = true;
		} else if (!strcmp(argv[i], "selfpair")) {
			cfg.ias_prefer_selfpair = true;
		} else if (!strcmp(argv[i], "vfioprealloc")) {
			if (i == argc - 1) {
				fprintf(stderr, "missing vfioprealloc argument\n");
				return -EINVAL;
			}
			char *token = strtok(argv[++i], ":");
			nr_vfio_prealloc = atoi(token);
			token = strtok(NULL, ":");
			if (!token) continue;
			vfio_prealloc_nrqs = atoi(token);
			token = strtok(NULL, ":");
			if (!token) continue;
			vfio_prealloc_rmp = atoi(token);
		} else if (!strcmp(argv[i], "vfio")) {
#ifndef DIRECTPATH
			log_err("please recompile with CONFIG_DIRECTPATH=y");
			return -EINVAL;
#endif
			cfg.vfio_directpath = true;
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
		} else if (!strcmp(argv[i], "numanode")) {
			if (sched_ops == &numa_ops) {
				fprintf(stderr, "Can't combine numanode argument with numa scheduler");
				return -EINVAL;
			}
			if (i == argc - 1) {
				fprintf(stderr, "missing numanode argument\n");
				return -EINVAL;
			}
			managed_numa_node = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "noidlefastwake")) {
			cfg.noidlefastwake = true;
		} else if (!strcmp(argv[i], "dpactiverss")) {
			cfg.directpath_active_rss = true;
		} else if (!strcmp(argv[i], "nohugepages")) {
			cfg.no_hugepages = true;
			cfg_transparent_hugepages_enabled = true;
		} else if (!strcmp(argv[i], "--")) {
			dpdk_argv = &argv[i+1];
			dpdk_argc = argc - i - 1;
			break;
		} else if (string_to_bitmap(argv[i], input_allowed_cores, NCPU)) {
			fprintf(stderr, "invalid cpu list: %s\n", argv[i]);
			fprintf(stderr, "example list: 0-24,26-48:2,49-255\n");
			return -EINVAL;
		} else {
			allowed_cores_supplied = true;
		}
	}

	ret = uname(&utsname);
	if (ret < 0) {
		log_err("failed to get utsname");
		return ret;
	}

	if (strstr(utsname.release, "azure")) {
		log_info("Detected Azure VM, using Azure ARP mode");
		cfg.azure_arp_mode = true;
	}

	pthread_barrier_init(&init_barrier, NULL, 2);

	ret = run_init_handlers("iokernel", iok_init_handlers,
			ARRAY_SIZE(iok_init_handlers));
	if (ret)
		return ret;

	iok_info->cycles_per_us = cycles_per_us;
	iok_info->external_directpath_enabled = cfg.vfio_directpath;
	iok_info->external_directpath_rmp = vfio_prealloc_rmp;
	iok_info->transparent_hugepages = cfg_transparent_hugepages_enabled;

	pthread_barrier_wait(&init_barrier);

	ksched_uintr_init();

	if (cfg.vfio_directpath)
		dataplane_loop_vfio();
	else
		dataplane_loop();
	return 0;
}
