/*
 * main.c - initialization and main dataplane loop for the iokernel
 */

#include <rte_ethdev.h>
#include <rte_lcore.h>

#include <base/init.h>
#include <base/log.h>
#include <base/stddef.h>

#include "defs.h"

#define LOG_INTERVAL_US		(1000 * 1000)
struct dataplane dp;

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
	IOK_INITIALIZER(cores),

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
void dataplane_loop()
{
	bool work_done;
#ifdef STATS
	uint64_t next_log_time = microtime();
#endif
	uint64_t now, last_time = microtime();

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

	/* run until quit or killed */
	for (;;) {
		work_done = false;

		/* handle a burst of ingress packets */
		work_done |= rx_burst();

		/* handle control messages */
		if (!work_done)
			dp_clients_rx_control_lrpcs();

		now = microtime();

		/* adjust core assignments */
		if (now - last_time > CORES_ADJUST_INTERVAL_US) {
			cores_adjust_assignments();
			last_time = now;
		}

		/* process a batch of commands from runtimes */
		work_done |= commands_rx();

		/* drain overflow completion queues */
		work_done |= tx_drain_completions();

		/* send a burst of egress packets */
		work_done |= tx_burst();

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

int main(int argc, char *argv[])
{
	int ret;

	ret = run_init_handlers("iokernel", iok_init_handlers,
			ARRAY_SIZE(iok_init_handlers));
	if (ret)
		return ret;

	dataplane_loop();
	return 0;
}
