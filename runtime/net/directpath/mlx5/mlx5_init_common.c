/*
 * mlx5_init_common.c - Initialize mlx5 driver in one of several possible modes
 */

#ifdef DIRECTPATH

#include <base/log.h>

#include "mlx5.h"

bool cfg_directpath_strided = false;

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

static int parse_directpath_stride(const char *name, const char *val)
{
	cfg_directpath_strided = true;
	return 0;
}

static struct cfg_handler directpath_stride_handler = {
	.name = "directpath_strided_rx",
	.fn = parse_directpath_stride,
	.required = false,
};

REGISTER_CFG(directpath_stride_handler);

static void mlx5_softirq_strided(void *arg)
{
	int cnt;
	struct mlx5_rxq *v = arg;
	struct mbuf *ms[RUNTIME_RX_BATCH_SIZE];

	while (true) {
		cnt = mlx5_gather_rx_strided(v, ms, RUNTIME_RX_BATCH_SIZE);
		if (cnt)
			net_rx_batch(ms, cnt);
		preempt_disable();
		v->poll_th = thread_self();
		thread_park_and_preempt_enable();
	}
}

static void mlx5_softirq(void *arg)
{
	int cnt;
	struct mlx5_rxq *v = arg;
	struct mbuf *ms[RUNTIME_RX_BATCH_SIZE];

	while (true) {
		cnt = mlx5_gather_rx(v, ms, RUNTIME_RX_BATCH_SIZE);
		if (cnt)
			net_rx_batch(ms, cnt);
		preempt_disable();
		v->poll_th = thread_self();
		thread_park_and_preempt_enable();
	}
}

bool mlx5_rx_poll(unsigned int q_index)
{
	struct mlx5_rxq *v = &rxqs[q_index];
	thread_t *th = v->poll_th;

	if (!th || !mlx5_rxq_pending(v))
		return false;

	if (!__sync_bool_compare_and_swap(&v->poll_th, th, NULL))
		return false;

	thread_ready(th);
	return true;
}

bool mlx5_rx_poll_locked(unsigned int q_index)
{
	struct mlx5_rxq *v = &rxqs[q_index];
	thread_t *th = v->poll_th;

	if (!th || !mlx5_rxq_pending(v))
		return false;

	if (!__sync_bool_compare_and_swap(&v->poll_th, th, NULL))
		return false;

	thread_ready_locked(th);
	return true;
}

static struct net_driver_ops mlx5_default_net_ops = {
	.rx_poll = mlx5_rx_poll,
	.rx_poll_locked = mlx5_rx_poll_locked,
	.tx_single = mlx5_transmit_one,
	.steer_flows = NULL,
	.register_flow = null_register_flow,
	.deregister_flow = null_deregister_flow,
	.get_flow_affinity = null_get_flow_affinity,
};

int mlx5_init_thread(void)
{
	int ret;
	struct kthread *k = myk();
	struct hardware_queue_spec *hs;
	struct mlx5_rxq *v = &rxqs[k->kthread_idx];

	v->shadow_tail = &k->q_ptrs->directpath_rx_tail;

	if (cfg_directpath_strided)
		v->poll_th = thread_create(mlx5_softirq_strided, v);
	else
		v->poll_th = thread_create(mlx5_softirq, v);
	if (!v->poll_th)
		return -ENOMEM;

	ret = mlx5_rx_stride_init_thread();
	if (ret)
		return ret;

	if (cfg_directpath_mode == DIRECTPATH_MODE_EXTERNAL)
		return 0;

	hs = &iok.threads[k->kthread_idx].direct_rxq;
	hs->descriptor_log_size = __builtin_ctz(sizeof(struct mlx5_cqe64));
	hs->nr_descriptors = v->cq.cnt;
	hs->descriptor_table = ptr_to_shmptr(&netcfg.tx_region,
		v->cq.cqes, (1 << hs->descriptor_log_size) * hs->nr_descriptors);
	hs->parity_byte_offset = offsetof(struct mlx5_cqe64, op_own);
	hs->parity_bit_mask = MLX5_CQE_OWNER_MASK;
	hs->hwq_type = cfg_directpath_mode == DIRECTPATH_MODE_QUEUE_STEERING ?
		HWQ_MLX5_QSTEER : HWQ_MLX5;
	hs->consumer_idx = ptr_to_shmptr(&netcfg.tx_region, v->shadow_tail,
	                                 sizeof(uint32_t));

	return 0;
}

int mlx5_init(void)
{
	int ret;

	/* Install default handlers, different configurations may override */
	net_ops = mlx5_default_net_ops;

	ret = mlx5_rx_stride_init();
	if (ret)
		return ret;

	if (cfg_directpath_mode == DIRECTPATH_MODE_EXTERNAL) {
		// hardware queue information will be provided later by the iokernel
		if (cfg_directpath_strided)
			cfg_request_hardware_queues = DIRECTPATH_REQUEST_STRIDED_RMP;
		else
			cfg_request_hardware_queues = DIRECTPATH_REQUEST_REGULAR;
		log_err("directpath_init: selected external mode");
		return 0;
	}

	if (cfg_directpath_mode == DIRECTPATH_MODE_FLOW_STEERING ||
	    cfg_directpath_mode == DIRECTPATH_MODE_ALLOW_ANY) {

		/* try to initialize in DevX mode */
		ret = mlx5_verbs_init_context(false);
		if (ret == 0) {
			cfg_directpath_mode = DIRECTPATH_MODE_FLOW_STEERING;
			log_err("directpath_init: selected flow steering mode");

			ret = mlx5_verbs_init(false);
			if (ret)
				return ret;

			return mlx5_init_flow_steering();
		}
	}

	assert(cfg_directpath_mode == DIRECTPATH_MODE_QUEUE_STEERING ||
	       cfg_directpath_mode == DIRECTPATH_MODE_ALLOW_ANY);

	cfg_directpath_mode = DIRECTPATH_MODE_QUEUE_STEERING;
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
