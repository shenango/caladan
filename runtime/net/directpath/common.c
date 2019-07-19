
#include <base/mempool.h>

#include "defs.h"

#ifdef DIRECTPATH

static struct direct_rxq *rxq_out[NCPU];
static struct direct_txq *txq_out[NCPU];

struct net_driver_ops net_ops;

struct mempool directpath_buf_mp;
struct tcache *directpath_buf_tcache;
DEFINE_PERTHREAD(struct tcache_perthread, directpath_buf_pt);

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

	rx_len = RX_BUF_BOOL_SZ(maxks);
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
	struct kthread *k = myk();
	struct hardware_queue_spec *hs;
	struct direct_rxq *rxq = rxq_out[k->kthread_idx];

	rxq->shadow_tail = &k->q_ptrs->directpath_rx_tail;
	hs = &iok.threads[k->kthread_idx].direct_rxq;

	hs->descriptor_size = (1 << rxq->descriptor_log_size);
	hs->nr_descriptors = rxq->nr_descriptors;
	hs->descriptor_table = ptr_to_shmptr(&netcfg.tx_region, rxq->descriptor_table, hs->descriptor_size * hs->nr_descriptors);
	hs->parity_byte_offset = rxq->parity_byte_offset;
	hs->parity_bit_mask = rxq->parity_bit_mask;
	hs->hwq_type = rxq->hwq_type;
	hs->consumer_idx = ptr_to_shmptr(&netcfg.tx_region, rxq->shadow_tail, sizeof(uint32_t));

	k->directpath_rxq = rxq;
	k->directpath_txq = txq_out[k->kthread_idx];

	tcache_init_perthread(directpath_buf_tcache, &perthread_get(directpath_buf_pt));

	return 0;
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

#endif
