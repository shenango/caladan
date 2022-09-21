/*
 * mlx5_init_common.c - Initialize mlx5 driver in one of several possible modes
 */

#ifdef DIRECTPATH

#include <base/log.h>

#include "mlx5.h"

static int null_register_flow(unsigned int a, struct trans_entry *e, void **h)
{
	return 0;
}

static int null_deregister_flow(struct trans_entry *e, void *h)
{
	return 0;
}

static uint32_t null_get_flow_affinity(uint8_t ipproto, uint16_t local_port,
	                                   struct netaddr remote)
{
	log_warn_once("flow affinity not enabled");
	return 0;
}

static struct net_driver_ops mlx5_default_net_ops = {
	.rx_batch = mlx5_gather_rx,
	.tx_single = mlx5_transmit_one,
	.rxq_has_work = mlx5_rxq_busy,
	.steer_flows = NULL,
	.register_flow = null_register_flow,
	.deregister_flow = null_deregister_flow,
	.get_flow_affinity = null_get_flow_affinity,
};

int mlx5_init_thread(void)
{
	struct kthread *k = myk();
	struct hardware_queue_spec *hs;
	struct mlx5_rxq *v = &rxqs[k->kthread_idx];

	k->directpath_rxq = &v->rxq;
	k->directpath_txq = &txqs[k->kthread_idx].txq;
	v->shadow_tail = &k->q_ptrs->directpath_rx_tail;

	if (directpath_mode == DIRECTPATH_MODE_EXTERNAL)
		return 0;

	hs = &iok.threads[k->kthread_idx].direct_rxq;
	hs->descriptor_log_size = __builtin_ctz(sizeof(struct mlx5_cqe64));
	hs->nr_descriptors = v->cq.cnt;
	hs->descriptor_table = ptr_to_shmptr(&netcfg.tx_region,
		v->cq.cqes, (1 << hs->descriptor_log_size) * hs->nr_descriptors);
	hs->parity_byte_offset = offsetof(struct mlx5_cqe64, op_own);
	hs->parity_bit_mask = MLX5_CQE_OWNER_MASK;
	hs->hwq_type = HWQ_MLX5;
	hs->consumer_idx = ptr_to_shmptr(&netcfg.tx_region, v->shadow_tail,
	                                 sizeof(uint32_t));

	return 0;
}

int mlx5_init(void)
{
	int ret;

	/* Install default handlers, different configurations may override */
	net_ops = mlx5_default_net_ops;

	if (directpath_mode == DIRECTPATH_MODE_EXTERNAL) {
		// hardware queue information will be provided later by the iokernel
		cfg_request_hardware_queues = true;
		log_err("directpath_init: selected external mode");
		return 0;
	}

	if (directpath_mode == DIRECTPATH_MODE_FLOW_STEERING ||
	    directpath_mode == DIRECTPATH_MODE_ALLOW_ANY) {

		/* try to initialize in DevX mode */
		ret = mlx5_verbs_init_context(false);
		if (ret == 0) {
			directpath_mode = DIRECTPATH_MODE_FLOW_STEERING;
			log_err("directpath_init: selected flow steering mode");

			ret = mlx5_verbs_init(false);
			if (ret)
				return ret;

			return mlx5_init_flow_steering();
		}
	}

	assert(directpath_mode == DIRECTPATH_MODE_QUEUE_STEERING ||
	       directpath_mode == DIRECTPATH_MODE_ALLOW_ANY);

	directpath_mode = DIRECTPATH_MODE_QUEUE_STEERING;
	log_err("directpath_init: selected queue steering mode");

	ret = mlx5_verbs_init_context(true);
	if (ret)
		return ret;

	ret = mlx5_verbs_init(true);
	if (ret)
		return ret;

	return mlx5_init_queue_steering();
}

#endif
