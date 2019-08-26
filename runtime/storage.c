/*
 * storage.c
 */

#include <runtime/storage.h>


uint32_t block_size;
uint64_t num_blocks;

#ifdef DIRECT_STORAGE
#include <stdio.h>
#include <base/hash.h>
#include <base/log.h>
#include <base/mempool.h>
#include <runtime/sync.h>

// Hack to prevent SPDK from pulling in extra headers here
#define SPDK_STDINC_H
#include <spdk/nvme.h>
#include <spdk/env.h>

#include "defs.h"

bool cfg_storage_enabled;
unsigned long device_latency_us = 10;

static struct spdk_nvme_ctrlr *controller;
static struct spdk_nvme_ns *spdk_namespace;

static __thread struct thread **cb_ths;
static __thread unsigned int nrcb_ths;

/* 4KB storage request buffers */
#define REQUEST_BUF_POOL_SZ (PGSIZE_2MB * 20)
#define REQUEST_BUF_SZ (4 * KB)
struct mempool storage_buf_mp;
static struct tcache *storage_buf_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, storage_buf_pt);

struct nvme_device {
	const char *name;
	unsigned long latency_us;
} known_devices[1] = {
	{
		.name = "INTEL SSDPED1D280GA",
		.latency_us = 10,
	}
};

/**
 * seq_complete - callback run after spdk nvme operation is complete
 *
 */
static void seq_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct thread *th = arg;
	cb_ths[nrcb_ths++] = th;
}

/**
 * probe_cb - callback run after nvme devices have been probed
 *
 */
static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		     struct spdk_nvme_ctrlr_opts *opts)
{
	opts->io_queue_size = UINT16_MAX;
	return true;
}

/**
 * attach_cb - callback run after nvme device has been attached
 *
 */
static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvme_ctrlr *ctrlr,
		      const struct spdk_nvme_ctrlr_opts *opts)
{
	int i, num_ns;
	const struct spdk_nvme_ctrlr_data *ctrlr_data;

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	if (num_ns > 1) {
		perror("more than 1 storage devices");
		exit(1);
	}
	if (num_ns == 0) {
		perror("no storage device");
		exit(1);
	}
	controller = ctrlr;
	ctrlr_data = spdk_nvme_ctrlr_get_data(ctrlr);
	spdk_namespace = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
	block_size = spdk_nvme_ns_get_sector_size(spdk_namespace);
	num_blocks = spdk_nvme_ns_get_num_sectors(spdk_namespace);

	for (i = 0; i < ARRAY_SIZE(known_devices); i++) {
		if (!strncmp((char *)ctrlr_data->mn, known_devices[i].name,
			           strlen(known_devices[i].name))) {
			device_latency_us = known_devices[i].latency_us;
			break;
		}
	}

	if (i == ARRAY_SIZE(known_devices))
		log_err("Warning: could not find latency profile for device %s,"
			      "using default latency of %lu us",
			      ctrlr_data->mn, device_latency_us);
}

static void *spdk_custom_allocator(size_t size, size_t align,
				   uint64_t *physaddr_out)
{
	void *out = iok_shm_alloc(size, align, NULL);

	if (out && physaddr_out)
		mem_lookup_page_phys_addr(out, PGSIZE_2MB, physaddr_out);

	return out;
}

/**
 * storage_init - initializes storage
 *
 */
int storage_init(void)
{
	int shm_id, rc;
	struct spdk_env_opts opts;
	void *buf;

	if (!cfg_storage_enabled)
		return 0;

	spdk_env_opts_init(&opts);
	opts.name = "shenango runtime";
	shm_id = rand_crc32c((uintptr_t)myk());
	if (shm_id < 0)
		shm_id = -shm_id;
	opts.shm_id = shm_id;

	spdk_nvme_allocator_hook = spdk_custom_allocator;

	if (spdk_env_init(&opts) < 0) {
		log_err("Unable to initialize SPDK env");
		return 1;
	}

	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		log_err("spdk_nvme_probe() failed");
		return 1;
	}

	if (controller == NULL) {
		log_err("no NVMe controllers found");
		return 1;
	}

	buf = mem_map_anom(NULL, REQUEST_BUF_POOL_SZ, PGSIZE_2MB, 0);
	if (buf == MAP_FAILED)
		return -ENOMEM;

	rc = spdk_mem_register(buf, REQUEST_BUF_POOL_SZ);
	if (rc)
		return rc;

	rc = mempool_create(&storage_buf_mp, buf, REQUEST_BUF_POOL_SZ,
			    PGSIZE_2MB, REQUEST_BUF_SZ);
	if (rc)
		return rc;

	storage_buf_tcache = mempool_create_tcache(
		&storage_buf_mp, "storagebufs", TCACHE_DEFAULT_MAG_SIZE);
	if (!storage_buf_tcache)
		return -ENOMEM;

	return 0;
}

/**
 * storage_init_thread - initializes storage (per-thread)
 */
int storage_init_thread(void)
{
	struct kthread *k = myk();
	struct hardware_queue_spec *hs =
		&iok.threads[k->kthread_idx].storage_hwq;
	struct storage_q *q = &k->storage_q;

	if (!cfg_storage_enabled)
		return 0;

	uint32_t max_xfer_size, entries, depth, nr_descriptors;
	uint32_t *consumer_idx;
	struct spdk_nvme_cpl *descriptor_table;
	void *qp_handle;
	struct spdk_nvme_io_qpair_opts opts;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(controller, &opts,
						  sizeof(opts));
	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(spdk_namespace);
	entries = (4096 - 1) / max_xfer_size + 2;
	depth = 64;
	if ((depth * entries) > opts.io_queue_size) {
		log_info("controller IO queue size %u less than required",
			 opts.io_queue_size);
		log_info(
			"Consider using lower queue depth or small IO size because "
			"IO requests may be queued at the NVMe driver.");
	}
	entries += 1;
	if (depth * entries > opts.io_queue_requests)
		opts.io_queue_requests = depth * entries;

	qp_handle =
		spdk_nvme_ctrlr_alloc_io_qpair(controller, &opts, sizeof(opts));
	if (qp_handle == NULL) {
		log_err("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed");
		return -1;
	}

	nvme_get_qp_info(qp_handle, &descriptor_table, &consumer_idx,
			 &nr_descriptors);
	nvme_set_shadow_ptr(qp_handle, &k->q_ptrs->storage_tail);

	/* intialize struct storage_q */
	spin_lock_init(&q->lock);
	q->outstanding_reqs = 0;
	q->spdk_qp_handle = qp_handle;
	q->hq.descriptor_table = descriptor_table;
	q->hq.consumer_idx = consumer_idx;
	q->hq.shadow_tail = &k->q_ptrs->storage_tail;
	q->hq.descriptor_log_size = __builtin_ctz(sizeof(struct spdk_nvme_cpl));
	BUILD_ASSERT(is_power_of_two(sizeof(struct spdk_nvme_cpl)));
	q->hq.nr_descriptors = nr_descriptors;
	q->hq.parity_byte_offset = offsetof(struct spdk_nvme_cpl, status);
	q->hq.parity_bit_mask = 0x1;

	/* inform iokernel of queue info */
	hs->descriptor_table =
		ptr_to_shmptr(&netcfg.tx_region, q->hq.descriptor_table,
			      sizeof(struct spdk_nvme_cpl) * nr_descriptors);
	hs->consumer_idx = ptr_to_shmptr(
		&netcfg.tx_region, &k->q_ptrs->storage_tail, sizeof(uint32_t));
	hs->descriptor_log_size = q->hq.descriptor_log_size;
	hs->nr_descriptors = q->hq.nr_descriptors;
	hs->parity_byte_offset = q->hq.parity_byte_offset;
	hs->parity_bit_mask = q->hq.parity_bit_mask;
	hs->hwq_type = HWQ_SPDK_NVME;

	tcache_init_perthread(storage_buf_tcache,
			      &perthread_get(storage_buf_pt));

	return 0;
}

/**
 * storage_proc_completions - process `budget` number of completions
 */
int storage_proc_completions(struct storage_q *q, unsigned int budget,
			     struct thread **wakeable_threads)
{
	assert_preempt_disabled();

	if (!cfg_storage_enabled)
		return 0;

	cb_ths = wakeable_threads;
	nrcb_ths = 0;

	if (!spin_try_lock(&q->lock))
		return 0;

	spdk_nvme_qpair_process_completions(q->spdk_qp_handle, budget);
	q->outstanding_reqs -= nrcb_ths;
	spin_unlock(&q->lock);

	return nrcb_ths;
}

/*
 * storage_write - write a payload to the nvme device
 *                 expects lba_count*storage_block_size() bytes to be allocated in the buffer
 *
 * returns -ENOMEM if no available memory, and -EIO if the write operation failed
 */
int storage_write(const void *payload, uint64_t lba, uint32_t lba_count)
{
	int rc;
	struct kthread *k;
	struct storage_q *q;
	void *spdk_payload;

	if (!cfg_storage_enabled)
		return -ENODEV;

	size_t req_size = lba_count * block_size;
	bool use_thread_cache = req_size <= REQUEST_BUF_SZ;

	k = getk();
	q = &k->storage_q;

	if (likely(use_thread_cache)) {
		spdk_payload = tcache_alloc(&perthread_get(storage_buf_pt));
	} else {
		spdk_payload =
			spdk_zmalloc(req_size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY,
				     SPDK_MALLOC_DMA);
	}

	if (unlikely(spdk_payload == NULL)) {
		putk();
		return -ENOMEM;
	}

	memcpy(spdk_payload, payload, req_size);

	spin_lock(&q->lock);
	rc = spdk_nvme_ns_cmd_write(spdk_namespace, q->spdk_qp_handle,
				    spdk_payload, lba, lba_count, seq_complete,
				    thread_self(), 0);

	if (unlikely(rc != 0)) {
		spin_unlock(&q->lock);
		rc = -EIO;
		goto done_np;
	}

	q->outstanding_reqs++;
	thread_park_and_unlock_np(&q->lock);

	preempt_disable();

done_np:
	if (likely(use_thread_cache))
		tcache_free(&perthread_get(storage_buf_pt), spdk_payload);
	else
		spdk_free(spdk_payload);

	preempt_enable();

	return rc;
}

/*
 * storage_read - read a payload from the nvme device
 *                expects lba_count*storage_block_size() bytes to be allocated in the buffer
 *
 * returns -ENOMEM if no available memory, and -EIO if the write operation failed
 */
int storage_read(void *dest, uint64_t lba, uint32_t lba_count)
{
	int rc;
	struct kthread *k;
	struct storage_q *q;
	void *spdk_payload;

	if (!cfg_storage_enabled)
		return -ENODEV;

	size_t req_size = lba_count * block_size;
	bool use_thread_cache = req_size <= REQUEST_BUF_SZ;

	k = getk();
	q = &k->storage_q;

	if (likely(use_thread_cache)) {
		spdk_payload = tcache_alloc(&perthread_get(storage_buf_pt));
	} else {
		spdk_payload =
			spdk_zmalloc(req_size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY,
				     SPDK_MALLOC_DMA);
	}

	if (unlikely(spdk_payload == NULL)) {
		putk();
		return -ENOMEM;
	}

	spin_lock(&q->lock);
	rc = spdk_nvme_ns_cmd_read(spdk_namespace, q->spdk_qp_handle,
				   spdk_payload, lba, lba_count, seq_complete,
				   thread_self(), 0);

	if (unlikely(rc != 0)) {
		spin_unlock(&q->lock);
		rc = -EIO;
		goto done_np;
	}

	q->outstanding_reqs++;
	thread_park_and_unlock_np(&q->lock);
	memcpy(dest, spdk_payload, req_size);
	preempt_disable();

done_np:
	if (likely(use_thread_cache))
		tcache_free(&perthread_get(storage_buf_pt), spdk_payload);
	else
		spdk_free(spdk_payload);
	preempt_enable();

	return rc;
}

#else
int storage_write(const void *payload, uint64_t lba, uint32_t lba_count)
{
	return -ENODEV;
}

int storage_read(void *dest, uint64_t lba, uint32_t lba_count)
{
	return -ENODEV;
}

int storage_init(void)
{
	return 0;
}

int storage_init_thread(void)
{
	return 0;
}

#endif
