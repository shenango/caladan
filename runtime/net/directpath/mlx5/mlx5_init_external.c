/*
 * mlx5_init_external.c - install a set of externally created mlx5 queue pairs
 */

#ifdef DIRECTPATH

#include <sys/mman.h>

#include <base/log.h>
#include <iokernel/control.h>

#include "mlx5.h"

static int mlx5_init_ext_thread_rx(struct shm_region *reg,
                                   struct directpath_queue_spec *spec,
                                   struct kthread *k, uint32_t lkey)
{
	int ret;
	struct mlx5_rxq *v = &rxqs[k->kthread_idx];
	void *buf, *dbr;

	if (unlikely(spec->rx_cq.stride != sizeof(struct mlx5_cqe64)))
		return -EINVAL;

	buf = shmptr_to_ptr(reg, spec->rx_cq.buf,
	                    spec->rx_cq.nr_entries * spec->rx_cq.stride);
	dbr = shmptr_to_ptr(reg, spec->rx_cq.dbrec, CACHE_LINE_SIZE);

	if (unlikely(!buf || !dbr))
		return -EINVAL;

	ret = mlx5_init_cq(&v->cq, buf, spec->rx_cq.nr_entries, dbr);
	if (unlikely(ret))
		return ret;

	buf = shmptr_to_ptr(reg, spec->rx_wq.buf,
	                    spec->rx_wq.nr_entries * spec->rx_wq.stride);
	dbr = shmptr_to_ptr(reg, spec->rx_wq.dbrec, CACHE_LINE_SIZE);

	if (unlikely(!buf || !dbr))
		return -EINVAL;

	return mlx5_init_rxq_wq(v, buf, dbr, spec->rx_wq.nr_entries,
	                        spec->rx_wq.stride, lkey);
}

static int mlx5_init_ext_thread_tx(struct shm_region *reg,
                                   struct directpath_queue_spec *spec,
                                   struct kthread *k, uint32_t lkey,
                                   void *bfreg)
{
	struct mlx5_txq *t = &txqs[k->kthread_idx];
	int ret;
	void *buf, *dbr;

	if (unlikely(spec->tx_cq.stride != sizeof(struct mlx5_cqe64)))
		return -EINVAL;

	buf = shmptr_to_ptr(reg, spec->tx_wq.buf,
	                    spec->tx_wq.nr_entries * spec->tx_wq.stride);
	dbr = shmptr_to_ptr(reg, spec->tx_wq.dbrec, CACHE_LINE_SIZE);

	if (unlikely(!buf || !dbr))
		return -EINVAL;

	ret = mlx5_init_txq_wq(t, buf, dbr, spec->tx_wq.nr_entries,
	                       spec->tx_wq.stride, lkey, spec->sqn, bfreg);
	if (unlikely(ret))
		return ret;

	buf = shmptr_to_ptr(reg, spec->tx_cq.buf,
	                    spec->tx_cq.nr_entries * spec->tx_cq.stride);
	dbr = shmptr_to_ptr(reg, spec->tx_cq.dbrec, CACHE_LINE_SIZE);

	if (unlikely(!buf || !dbr))
		return -EINVAL;

	return mlx5_init_cq(&t->cq, buf, spec->tx_cq.nr_entries, dbr);
}

int mlx5_init_ext_late(struct directpath_spec *spec, int bar_fd, int mem_fd)
{
	int ret, last_uarn = -1;
	struct shm_region memfd_reg;
	unsigned int i;
	void *bar_reg = NULL, *bfreg;

	/* map the provided shared memory */
	memfd_reg.len = spec->memfd_region_size;
	memfd_reg.base = mmap(NULL, memfd_reg.len, PROT_READ | PROT_WRITE,
		                  MAP_SHARED, mem_fd, 0);
	if (unlikely(memfd_reg.base == MAP_FAILED)) {
		log_err("mlx5_ext: failed to map memfd region (errno %d)", errno);
		return -1;
	}

	/*
	 * The NIC memory registration is done using the VA of the shared memory
	 * region as mapped in the iokernel. This computes the offset to add to
	 * every runtime-process VA when sending VAs directly to the NIC.
	 */
	rx_mr_offset = spec->va_base - (uintptr_t)netcfg.tx_region.base;
	tx_mr_offset = rx_mr_offset;

	/* set up each queue pair */
	for (i = 0; i < maxks; i++) {
		ret = mlx5_init_ext_thread_rx(&memfd_reg, &spec->qs[i], ks[i],
			                          spec->mr);
		if (unlikely(ret))
			return ret;

		/* map the assigned BAR page (UAR) with the doorbell */
		if (!bar_reg || spec->qs[i].uarn != last_uarn) {
			bar_reg = mmap(NULL, PGSIZE_4KB, PROT_READ | PROT_WRITE,
				           MAP_SHARED, bar_fd,
				           spec->offs + PGSIZE_4KB * spec->qs[i].uarn);
			if (unlikely(bar_reg == MAP_FAILED)) {
				log_err("failed to mmap bfreg");
				return -1;
			}
			last_uarn = spec->qs[i].uarn;
		}
		bfreg = bar_reg + spec->qs[i].uar_offset;

		ret = mlx5_init_ext_thread_tx(&memfd_reg, &spec->qs[i], ks[i],
			                          spec->mr, bfreg);
		if (unlikely(ret))
			return ret;
	}

	close(mem_fd);
	close(bar_fd);

	return mlx5_init_queue_steering();
}

#endif
