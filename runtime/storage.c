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
struct iovec;
#include <spdk/nvme.h>
#include <spdk/env.h>

#include "defs.h"

unsigned long storage_device_latency_us = 100;
bool cfg_storage_enabled;

static struct spdk_nvme_ctrlr *controller;
static struct spdk_nvme_ns *spdk_namespace;

/* 4KB storage request buffers */
#define REQUEST_BUF_POOL_SZ (PGSIZE_2MB * 20)
#define REQUEST_BUF_SZ (16 * KB)
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

static void seq_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct thread *th = arg;
	thread_ready(th);
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
			log_info("storage: recognized device %s", known_devices[i].name);
			storage_device_latency_us = known_devices[i].latency_us;
			break;
		}
	}


}

/**
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
		spdk_payload = tcache_alloc(perthread_ptr(storage_buf_pt));
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
		tcache_free(perthread_ptr(storage_buf_pt), spdk_payload);
	else
		spdk_free(spdk_payload);

	preempt_enable();

	return rc;
}

/**
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
		spdk_payload = tcache_alloc(perthread_ptr(storage_buf_pt));
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
		tcache_free(perthread_ptr(storage_buf_pt), spdk_payload);
	else
		spdk_free(spdk_payload);
	preempt_enable();

	return rc;
}

static int storage_softirq_one(struct storage_q *q)
{
	int ret;

	assert_spin_lock_held(&q->lock);

	ret = spdk_nvme_qpair_process_completions(q->spdk_qp_handle, RUNTIME_RX_BATCH_SIZE);
	q->outstanding_reqs -= ret;
	return ret;
}

void storage_softirq(void *arg)
{
	struct kthread *k = arg;
	struct storage_q *q = &k->storage_q;
	int ret;

	if (!cfg_storage_enabled)
		return;

	while (true) {
		preempt_disable();
		do {
			spin_lock(&q->lock);
			ret = storage_softirq_one(q);
			spin_unlock(&q->lock);
		} while (!preempt_needed() && ret > 0);
		k->storage_busy = false;
		thread_park_and_preempt_enable();
	}
}

/**
 * storage_init_thread - initializes storage (per-thread)
 */
int storage_init_thread(void)
{
	struct kthread *k = myk();
	struct hardware_queue_spec *hs = &iok.threads[k->kthread_idx].storage_hwq;
	struct storage_q *q = &k->storage_q;
	thread_t *th;

	uint32_t max_xfer_size, entries, depth, *consumer_idx;
	shmptr_t cq_shm;
	struct spdk_nvme_cpl *cpl;
	struct spdk_nvme_io_qpair_opts opts;
	void *qp_handle;

	if (!cfg_storage_enabled)
		return 0;

	th = thread_create(storage_softirq, k);
	if (!th)
		return -ENOMEM;

	k->storage_softirq = th;
	spdk_nvme_ctrlr_get_default_io_qpair_opts(controller, &opts, sizeof(opts));
	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(spdk_namespace);
	entries = (4096 - 1) / max_xfer_size + 2;
	depth = 64;
	if (depth * entries > opts.io_queue_size) {
		log_info("controller IO queue size %u less than required",
			 opts.io_queue_size);
		log_info(
			"Consider using lower queue depth or small IO size because "
			"IO requests may be queued at the NVMe driver.");
	}
	entries += 1;
	if (depth * entries > opts.io_queue_requests)
		opts.io_queue_requests = depth * entries;


	/* Allocate CQ of size io_queue_size * sizeof(struct spdk_nvme_cpl) */
	opts.cq.buffer_size = opts.io_queue_size * sizeof(*cpl);
	cpl = iok_shm_alloc(opts.cq.buffer_size, PGSIZE_4KB, &cq_shm);
	if (!cpl) {
		log_err("could not allocate storage CQ buf in shared mem");
		return -ENOMEM;
	}
	opts.cq.vaddr = cpl;
	qp_handle =
		spdk_nvme_ctrlr_alloc_io_qpair(controller, &opts, sizeof(opts));
	if (qp_handle == NULL) {
		log_err("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed");
		return -1;
	}

	nvme_setup_shenango(qp_handle, &consumer_idx,  &k->q_ptrs->storage_tail);

	/* intialize struct storage_q */
	spin_lock_init(&q->lock);
	q->outstanding_reqs = 0;
	q->spdk_qp_handle = qp_handle;
	q->hq.descriptor_table = cpl;
	q->hq.consumer_idx = consumer_idx;
	q->hq.shadow_tail = &k->q_ptrs->storage_tail;
	q->hq.descriptor_log_size = __builtin_ctz(sizeof(*cpl));
	BUILD_ASSERT(is_power_of_two(sizeof(*cpl)));
	q->hq.nr_descriptors = opts.io_queue_size;
	q->hq.parity_byte_offset = offsetof(struct spdk_nvme_cpl, status);
	q->hq.parity_bit_mask = 0x1;

	/* inform iokernel of queue info */
	hs->descriptor_table = cq_shm;
	hs->consumer_idx = ptr_to_shmptr(
		&netcfg.tx_region, &k->q_ptrs->storage_tail, sizeof(uint32_t));
	hs->descriptor_log_size = q->hq.descriptor_log_size;
	hs->nr_descriptors = q->hq.nr_descriptors;
	hs->parity_byte_offset = q->hq.parity_byte_offset;
	hs->parity_bit_mask = q->hq.parity_bit_mask;
	hs->hwq_type = HWQ_SPDK_NVME;

	tcache_init_perthread(storage_buf_tcache,
			      perthread_ptr(storage_buf_pt));

	return 0;
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

	if (spdk_env_init(&opts) < 0) {
		log_err("Unable to initialize SPDK env");
		return 1;
	}

	rc = spdk_mem_register(netcfg.tx_region.base, netcfg.tx_region.len);
	if (rc != 0) {
		log_err("storage: failed to register SHM area");
		return 1;
	}

	rc = spdk_nvme_probe(NULL, NULL, NULL, attach_cb, NULL);
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
