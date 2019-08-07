
#include <base/kref.h>
#include <base/mempool.h>
#include <runtime/sync.h>

#include "defs.h"

#ifdef DIRECTPATH

static struct hardware_q *rxq_out[NCPU];
static struct direct_txq *txq_out[NCPU];

struct mempool directpath_buf_mp;
struct tcache *directpath_buf_tcache;
DEFINE_PERTHREAD(struct tcache_perthread, directpath_buf_pt);

bool cfg_directpath_enabled;

size_t directpath_rx_buf_pool_sz(unsigned int nrqs)
{
	return align_up(nrqs * (32 * RQ_NUM_DESC) * 16UL * MBUF_DEFAULT_LEN,
			PGSIZE_2MB);
}

void directpath_rx_completion(struct mbuf *m)
{
	preempt_disable();
	tcache_free(&perthread_get(directpath_buf_pt), (void *)m);
	preempt_enable();
}

static int rx_memory_init(void)
{
	int ret;
	size_t rx_len;
	void *rx_buf;

	rx_len = directpath_rx_buf_pool_sz(maxks);
	rx_buf = mem_map_anom(NULL, rx_len, PGSIZE_2MB, 0);
	if (rx_buf == MAP_FAILED)
		return -ENOMEM;

	ret = mempool_create(&directpath_buf_mp, rx_buf, rx_len,
			     PGSIZE_2MB, MBUF_DEFAULT_LEN);
	if (ret)
		return ret;

	directpath_buf_tcache = mempool_create_tcache(&directpath_buf_mp,
		"runtime_rx_bufs", TCACHE_DEFAULT_MAG_SIZE);
	if (!directpath_buf_tcache)
		return -ENOMEM;

	return 0;

}

int directpath_init(void)
{
	int ret;

	if (!cfg_directpath_enabled)
		return 0;

	ret = rx_memory_init();
	if (ret)
		return ret;

	/* initialize mlx5 */
	ret = mlx5_init(rxq_out, txq_out, maxks, maxks);
	if (ret)
		return ret;

	return 0;

}

int directpath_init_thread(void)
{
	if (!cfg_directpath_enabled)
		return 0;

	struct kthread *k = myk();
	struct hardware_queue_spec *hs;
	struct hardware_q *rxq = rxq_out[k->kthread_idx];

	rxq->shadow_tail = &k->q_ptrs->directpath_rx_tail;
	hs = &iok.threads[k->kthread_idx].direct_rxq;

	hs->descriptor_log_size = rxq->descriptor_log_size;
	hs->nr_descriptors = rxq->nr_descriptors;
	hs->descriptor_table = ptr_to_shmptr(&netcfg.tx_region,
		rxq->descriptor_table, (1 << hs->descriptor_log_size) * hs->nr_descriptors);
	hs->parity_byte_offset = rxq->parity_byte_offset;
	hs->parity_bit_mask = rxq->parity_bit_mask;
	hs->hwq_type = HWQ_MLX5;
	hs->consumer_idx = ptr_to_shmptr(&netcfg.tx_region, rxq->shadow_tail, sizeof(uint32_t));

	k->directpath_rxq = rxq;
	k->directpath_txq = txq_out[k->kthread_idx];

	tcache_init_perthread(directpath_buf_tcache, &perthread_get(directpath_buf_pt));

	return 0;
}

static DEFINE_SPINLOCK(flow_worker_lock);
static thread_t *flow_worker_th;
static LIST_HEAD(flow_to_register);
static LIST_HEAD(flow_to_deregister);

static void flow_registration_worker(void *arg)
{
	int ret;
	struct flow_registration *f;

	while (true) {
		spin_lock_np(&flow_worker_lock);
		f = list_pop(&flow_to_register, struct flow_registration, flow_reg_link);
		if (f) {
			spin_unlock_np(&flow_worker_lock);
			ret = net_ops.register_flow(f->kthread_affinity, f->e, &f->hw_flow_handle);
			WARN_ON(ret);
			continue;
		}

		f = list_pop(&flow_to_deregister, struct flow_registration, flow_dereg_link);
		if (f) {
			spin_unlock_np(&flow_worker_lock);
			ret = net_ops.deregister_flow(f->e, f->hw_flow_handle);
			WARN_ON(ret);
			f->release(f->ref);
			continue;
		}

		flow_worker_th = thread_self();
		thread_park_and_unlock_np(&flow_worker_lock);
	}
}

void register_flow(struct flow_registration *f)
{
	if (!cfg_directpath_enabled)
		return;

	/* take a reference for the hardware flow table */
	kref_get(f->ref);

	spin_lock_np(&flow_worker_lock);
	list_add(&flow_to_register, &f->flow_reg_link);
	if (flow_worker_th) {
		thread_ready(flow_worker_th);
		flow_worker_th = NULL;
	}
	spin_unlock_np(&flow_worker_lock);

}

void deregister_flow(struct flow_registration *f)
{
	if (!cfg_directpath_enabled)
		return;

	spin_lock_np(&flow_worker_lock);
	list_add(&flow_to_deregister, &f->flow_dereg_link);
	if (flow_worker_th) {
		thread_ready(flow_worker_th);
		flow_worker_th = NULL;
	}
	spin_unlock_np(&flow_worker_lock);
}

int directpath_init_late(void)
{
	if (!cfg_directpath_enabled)
		return 0;

	return thread_spawn(flow_registration_worker, NULL);
}

#else

int directpath_init(void)
{
	return 0;
}

int directpath_init_thread(void)
{
	return 0;
}

int directpath_init_late(void)
{
	return 0;
}


#endif
