/*
 * dp_clients.c - functions for registering/unregistering dataplane clients
 */

#include <signal.h>
#include <unistd.h>

#include <rte_dev.h>
#include <rte_ether.h>
#include <rte_flow.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>

#include <base/log.h>
#include <base/lrpc.h>

#include "defs.h"
#include "sched.h"

#define IP_TO_PROC_ENTRIES	IOKERNEL_MAX_PROC

static struct lrpc_chan_out lrpc_data_to_control;
static struct lrpc_chan_in lrpc_control_to_data;

extern struct rte_eth_rss_conf rss_conf;
extern bool rss_conf_present;

static int dp_clients_setup_flow_tags(struct proc *p)
{
	int ret;

	struct rte_flow_action actions[3];
	struct rte_flow_action_mark mark_action;
	struct rte_flow_action_rss rss;
	struct rte_flow_attr attr;
	struct rte_flow_item pattern[2];
	struct rte_flow_item_ipv4 ip;
	struct rte_flow_item_ipv4 ip_mask;
	uint16_t queue = 0;

	if (!rss_conf_present)
		return 0;

	memset(&attr, 0, sizeof(attr));
	attr.ingress = 1;

	memset(&ip, 0, sizeof(ip));
	ip.hdr.dst_addr = htobe32(p->ip_addr);

	memset(&ip_mask, 0, sizeof(ip_mask));
	ip_mask.hdr.dst_addr = UINT32_MAX;

	memset(&rss, 0, sizeof(rss));
	rss.types = rss_conf.rss_hf;
	rss.key_len = rss_conf.rss_key_len;
	rss.key = rss_conf.rss_key;
	rss.queue_num = 1;
	rss.queue = &queue;

	memset(&mark_action, 0, sizeof(mark_action));
	mark_action.id = p->uniqid;

	memset(pattern, 0, sizeof(pattern));
	pattern[0].type = RTE_FLOW_ITEM_TYPE_IPV4;
	pattern[0].spec = &ip;
	pattern[0].mask = &ip_mask;
	pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

	memset(actions, 0, sizeof(actions));
	actions[0].type = RTE_FLOW_ACTION_TYPE_RSS;
	actions[0].conf = &rss;
	actions[1].type = RTE_FLOW_ACTION_TYPE_MARK;
	actions[1].conf = &mark_action;
	actions[2].type = RTE_FLOW_ACTION_TYPE_END;

	ret = rte_flow_validate(dp.port, &attr, pattern, actions, NULL);
	if (unlikely(ret))
		return ret;
	p->flow = rte_flow_create(dp.port, &attr, pattern, actions, NULL);
	if (unlikely(!p->flow))
		return -1;
	return 0;
}

static void dp_clients_destroy_flow_tags(struct proc *p)
{
	int ret;

	if (unlikely(!p->flow))
		return;

	ret = rte_flow_destroy(dp.port, p->flow, NULL);
	if (unlikely(ret))
		log_err("dp_clients: failed to remove HW flow rule");
}

/*
 * Add a new client.
 */
static void dp_clients_add_client(struct proc *p)
{
	int ret;

	if (!sched_attach_proc(p)) {
		p->dp_clients_idx = dp.nr_clients;
		dp.clients[dp.nr_clients++] = p;
		dp.clients_by_id[p->uniqid] = p;
		if (dp.nr_clients >= 2 && cfg.allow_loopback)
			dp.loopback_en = true;
	} else {
		log_err("dp_clients: failed to attach proc.");
		p->attach_fail = true;
		kill(p->pid, SIGKILL);
		proc_put(p);
		return;
	}

	ret = rte_hash_lookup(dp.ip_to_proc, &p->ip_addr);
	if (ret != -ENOENT) {
		log_err("Duplicate IP address detected.");
		p->attach_fail_dup = true;
		goto fail;
	}

	ret = rte_hash_add_key_data(dp.ip_to_proc, &p->ip_addr, p);
	if (ret < 0) {
		log_err("dp_clients: failed to add IP to hash table in add_client");
		goto fail;
	}

	if (!p->has_directpath) {
		ret = do_dpdk_dma_map(p->region.base, p->region.len,
			              proc_pgsize(p), p->page_paddrs);
		if (ret < 0) {
			log_err("dp_clients: failed to do dma map");
			goto fail;
		}

		ret = dp_clients_setup_flow_tags(p);
		if (ret < 0)
			log_warn_once("dp_clients: flow tags unavailable");
	}

	if (p->has_vfio_directpath)
		directpath_dataplane_attach(p);

	return;

fail:
	p->attach_fail = true;
	dp_clients_remove_client(p);
}

void proc_release(struct ref *r)
{
	ssize_t ret;

	struct proc *p = container_of(r, struct proc, ref);
	if (!lrpc_send(&lrpc_data_to_control, CONTROL_PLANE_REMOVE_CLIENT,
			(unsigned long) p))
		log_err("dp_clients: failed to inform control of client removal");
	ret = write(data_to_control_efd, &(uint64_t){ 1 }, sizeof(uint64_t));
	WARN_ON(ret != sizeof(uint64_t));
}

/*
 * Remove a client. Notify control plane once removal is complete so that it
 * can delete its data structures.
 */
void dp_clients_remove_client(struct proc *p)
{
	int ret;
	struct rx_priv_data *pdata;

	// make sure we run once (could be instructed by control to remove this
	// process after we already started removing it)
	if (p->kill)
		return;

	dp.clients[p->dp_clients_idx] = dp.clients[--dp.nr_clients];
	dp.clients[p->dp_clients_idx]->dp_clients_idx = p->dp_clients_idx;

	dp.clients_by_id[p->uniqid] = NULL;

	if (dp.nr_clients < 2)
		dp.loopback_en = false;

	if (!p->attach_fail_dup) {
		ret = rte_hash_del_key(dp.ip_to_proc, &p->ip_addr);
		if (ret < 0)
			log_err("dp_clients: failed to remove IP from hash "
			        "table in remove client");
	}

	if (!p->has_directpath) {
		if (!p->attach_fail) {
			do_dpdk_dma_unmap(p->region.base, p->region.len,
				          proc_pgsize(p), p->page_paddrs);
			dp_clients_destroy_flow_tags(p);
		}
	}

	if (p->nr_overflows)
		list_del(&p->overflow_link);

	/* TODO: free queued packets/commands? */

	/* release cores assigned to this runtime */
	p->kill = true;
	if (p->has_vfio_directpath)
		directpath_dataplane_notify_kill(p);
	sched_detach_proc(p);

	for (size_t i = 0; i < p->thread_count; i++)
		unpoll_thread(&p->threads[i]);

	while (true) {
		pdata = list_pop(&p->owned_rx_bufs, struct rx_priv_data, link);
		if (!pdata)
			break;

		assert(pdata->owner == p);
		pdata->owner = NULL;
		struct rte_mbuf *m = (void *)pdata - sizeof(*m);
		rte_pktmbuf_free(m);
	}

	if (p->dataplane_error) {
		log_err("proc %d experienced a dataplane error", p->pid);
		kill(p->pid, SIGKILL);
	}

	proc_put(p);
}

/*
 * Process a batch of messages from the control plane.
 */
void dp_clients_rx_control_lrpcs(void)
{
	uint64_t cmd;
	unsigned long payload;
	uint16_t n_rx = 0;
	struct proc *p;

	while (n_rx < IOKERNEL_CONTROL_BURST_SIZE &&
			lrpc_recv(&lrpc_control_to_data, &cmd, &payload)) {
		p = (struct proc *) payload;

		switch (cmd)
		{
		case DATAPLANE_ADD_CLIENT:
			dp_clients_add_client(p);
			break;
		case DATAPLANE_REMOVE_CLIENT:
			dp_clients_remove_client(p);
			break;
		default:
			log_err("dp_clients: received unrecognized command %lu", cmd);
		}

		n_rx++;
	}
}

/*
 * Initialize channels for communicating with the I/O kernel control plane.
 */
int dp_clients_init(void)
{
	int ret;
	struct rte_hash_parameters hash_params = { 0 };

	ret = lrpc_init_in(&lrpc_control_to_data,
			lrpc_control_to_data_params.buffer, CONTROL_DATAPLANE_QUEUE_SIZE,
			lrpc_control_to_data_params.wb);
	if (ret < 0) {
		log_err("dp_clients: initializing LRPC from control plane failed");
		return -1;
	}

	ret = lrpc_init_out(&lrpc_data_to_control,
			lrpc_data_to_control_params.buffer, CONTROL_DATAPLANE_QUEUE_SIZE,
			lrpc_data_to_control_params.wb);
	if (ret < 0) {
		log_err("dp_clients: initializing LRPC to control plane failed");
		return -1;
	}

	dp.nr_clients = 0;

	/* initialize the hash table for mapping IPs to runtimes */
	hash_params.name = "ip_to_proc_hash_table";
	hash_params.entries = IP_TO_PROC_ENTRIES;
	hash_params.key_len = sizeof(uint32_t);
	hash_params.hash_func = rte_jhash;
	hash_params.hash_func_init_val = 0;
	hash_params.socket_id = rte_socket_id();
	dp.ip_to_proc = rte_hash_create(&hash_params);
	if (dp.ip_to_proc == NULL) {
		log_err("dp_clients: failed to create IP to proc hash table");
		return -1;
	}

	return 0;
}
