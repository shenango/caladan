/*
 * dp_clients.c - functions for registering/unregistering dataplane clients
 */

#include <unistd.h>

#include <rte_ether.h>
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

static void dp_clients_remove_client(struct proc *p);

/*
 * Add a new client.
 */
static void dp_clients_add_client(struct proc *p)
{
	int ret;

	if (!sched_attach_proc(p)) {
		p->kill = false;
		p->dp_clients_idx = dp.nr_clients;
		dp.clients[dp.nr_clients++] = p;
	} else {
		log_err("dp_clients: failed to attach proc.");
		p->attach_fail = true;
		proc_put(p);
		return;
	}

	ret = rte_hash_lookup(dp.ip_to_proc, &p->ip_addr);
	if (ret != -ENOENT) {
		log_err("Duplicate IP address detected.");
		goto fail;
	}

	ret = rte_hash_add_key_data(dp.ip_to_proc, &p->ip_addr, p);
	if (ret < 0) {
		log_err("dp_clients: failed to add IP to hash table in add_client");
		goto fail;
	}

	if (!p->has_directpath) {
		ret = rte_extmem_register(p->region.base, p->region.len, NULL, 0, PGSIZE_2MB);
		if (ret < 0) {
			log_err("dp_clients: failed to register extmem for client");
			goto fail;
		}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
		ret = rte_dev_dma_map(dp.device, p->region.base, 0, p->region.len);
		if (ret < 0) {
			log_err("dp_clients: failed to map DMA memory for client");
			goto fail_extmem;
		}
#pragma GCC diagnostic pop
	}

	if (p->has_vfio_directpath)
		directpath_dataplane_attach(p);

	return;

fail_extmem:
	rte_extmem_unregister(p->region.base, p->region.len);
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
static void dp_clients_remove_client(struct proc *p)
{
	int ret;

	dp.clients[p->dp_clients_idx] = dp.clients[--dp.nr_clients];
	dp.clients[p->dp_clients_idx]->dp_clients_idx = p->dp_clients_idx;

	ret = rte_hash_del_key(dp.ip_to_proc, &p->ip_addr);
	if (ret < 0)
		log_err("dp_clients: failed to remove IP from hash table in remove "
		        "client");

	if (!p->has_directpath) {
		if (!p->attach_fail) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
			ret = rte_dev_dma_unmap(dp.device, p->region.base, 0, p->region.len);
			if (ret < 0)
				log_err("dp_clients: failed to unmap DMA memory for client");
#pragma GCC diagnostic pop
			ret = rte_extmem_unregister(p->region.base, p->region.len);
			if (ret < 0)
				log_err("dp_clients: failed to unregister extmem for client");
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
