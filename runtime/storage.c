/*
 * storage.c
 */
#if __has_include("spdk/nvme.h")
#include <stdio.h>
#include <base/hash.h>
#include <base/log.h>
#include <runtime/storage.h>
#include <runtime/sync.h>

#include "spdk/nvme.h"
#include "spdk/env.h"

#include "defs.h"

static struct spdk_nvme_ctrlr *controller;
static struct spdk_nvme_ns *namespace;
static int block_size;
static int num_blocks;

static __thread struct thread **cb_ths;
static __thread int nrcb_ths;

/**
 * seq_complete - callback run after spdk nvme operation is complete
 *
 */
static void
seq_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct thread *th = arg;
	cb_ths[nrcb_ths++] = th;
}

/**
 * probe_cb - callback run after nvme devices have been probed
 *
 */
static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	struct spdk_nvme_ctrlr_opts *opts)
{
	opts->io_queue_size = UINT16_MAX;
	return true;
}

/**
 * attach_cb - callback run after nvme device has been attached
 *
 */
static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int num_ns;

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
	namespace = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
	block_size = (int)spdk_nvme_ns_get_sector_size(namespace);
	num_blocks = (int)spdk_nvme_ns_get_num_sectors(namespace);
}

/**
 * storage_init - initializes storage
 *
 */
int storage_init(void)
{
	int shm_id, rc;
	struct spdk_env_opts opts;

	spdk_env_opts_init(&opts);
	opts.name = "shenango runtime";
	shm_id = rand_crc32c((uintptr_t)myk());
	if (shm_id < 0) shm_id = -shm_id;
	opts.shm_id = shm_id;

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

	iok.spdk_shm_id = shm_id;
	return 0;
}

/**
 * storage_init_thread - initializes storage (per-thread)
 */
int storage_init_thread(void)
{

	struct spdk_nvme_io_qpair_opts opts;
	struct kthread *k;
	struct shm_region r;
	struct nvme_pcie_qpair {
		uint8_t pad0[40];
		struct spdk_nvme_cpl *cpl;
		uint8_t pad1[46];
		uint16_t cq_head;
		uint8_t pad2[2];
		uint8_t phase;
		bool  is_enabled;
		void *qpair;
	}* qpair;
	void *qpair_addr;
	uint32_t max_xfer_size, entries, depth;
	struct thread_spec* spec;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(controller,
			&opts, sizeof(opts));
	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(namespace);
	entries = (4096 - 1) / max_xfer_size + 2;
	depth = 64;
	if ((depth * entries) > opts.io_queue_size) {
		log_info("controller IO queue size %u less than required",
			opts.io_queue_size);
		log_info("Consider using lower queue depth or small IO size because "
			"IO requests may be queued at the NVMe driver.");
	}
	entries += 1;


	if (depth * entries > opts.io_queue_requests)
		opts.io_queue_requests = depth * entries;

	k = getk();
	qpair_addr = spdk_nvme_ctrlr_alloc_io_qpair(controller, &opts, sizeof(opts));
	k->nvme_io_pair = qpair_addr;
	if (k->nvme_io_pair == NULL) {
		putk();
		log_err("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed");
		return 1;
	}
	putk();


	qpair = container_of((void **)qpair_addr, typeof(*qpair), qpair);
	r.base = (void *)SPDK_BASE_ADDR;
	r.len = SPDK_BASE_ADDR_OFFSET;
	spec = &iok.threads[myk()->kthread_idx];
	spec->nvme_qpair_cpl = ptr_to_shmptr(&r,
			qpair->cpl, sizeof(qpair->cpl));
	spec->nvme_qpair_cq_head = ptr_to_shmptr(&r,
			&qpair->cq_head, sizeof(qpair->cq_head));
	spec->nvme_qpair_phase = ptr_to_shmptr(&r,
			&qpair->phase, sizeof(qpair->phase));
	return 0;
}

/**
 * storage_proc_completions - process `budget` number of completions
 */
int storage_proc_completions(struct kthread *k,
	unsigned int budget, struct thread **wakeable_threads)
{
	assert_preempt_disabled();
	cb_ths = wakeable_threads;
	nrcb_ths = 0;
	if (!spin_try_lock(&k->io_pair_lock)) {
		return 0;
	}
	spdk_nvme_qpair_process_completions(k->nvme_io_pair, budget);
	k->outstanding_reqs -= nrcb_ths;
	spin_unlock(&k->io_pair_lock);
	return nrcb_ths;
}

/*
 * storage_available_completions - get number of available completions
 *
 * Preemption must be disabled!
 */
int storage_available_completions(struct kthread *k)
{
	return nvme_pcie_qpair_available_completions(k->nvme_io_pair);
}

/*
 * storage_write - write a payload to the nvme device
 *                 expects lba_count*storage_block_size() bytes to be allocated in the buffer
 *
 * returns -ENOMEM if no available memory, and -EIO if the write operation failed
 */
int storage_write(const void* payload, int lba, int lba_count)
{
	int rc;
	struct kthread *k;
	void *spdk_payload;

	k = getk();
	spdk_payload = spdk_zmalloc(lba_count * block_size, 0, NULL,
			SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (unlikely(spdk_payload == NULL)) {
		putk();
		return -ENOMEM;
	}
	memcpy(spdk_payload, payload, lba_count * block_size);
	spin_lock(&k->io_pair_lock);
	rc = spdk_nvme_ns_cmd_write(namespace, k->nvme_io_pair, spdk_payload,
			lba, /* LBA start */
			lba_count, /* number of LBAs */
			seq_complete, thread_self(), 0);
	if (unlikely(rc != 0)) {
		log_err("starting write I/O failed");
		spin_unlock(&k->io_pair_lock);
		spdk_free(spdk_payload);
		putk();
		return -EIO;
	}
	k->outstanding_reqs++;
	thread_park_and_unlock_np(&k->io_pair_lock);
	preempt_disable();
	spdk_free(spdk_payload);
	preempt_enable();
	return 0;
}

/*
 * storage_read - read a payload from the nvme device
 *                expects lba_count*storage_block_size() bytes to be allocated in the buffer
 *
 * returns -ENOMEM if no available memory, and -EIO if the write operation failed
 */
int storage_read(void* dest, int lba, int lba_count)
{
	int rc;
	struct kthread *k;
	void *spdk_dest;

	k = getk();
	spdk_dest = spdk_zmalloc(lba_count * block_size, 0, NULL,
			SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (unlikely(spdk_dest == NULL)) {
		putk();
		return -ENOMEM;
	}
	spin_lock(&k->io_pair_lock);
	rc = spdk_nvme_ns_cmd_read(namespace, k->nvme_io_pair, spdk_dest,
			lba, /* LBA start */
			lba_count, /* number of LBAs */
			seq_complete, thread_self(), 0);
	if (unlikely(rc != 0)) {
		log_err("starting read I/O failed\n");
		spin_unlock(&k->io_pair_lock);
		spdk_free(spdk_dest);
		putk();
		return -EIO;
	}
	k->outstanding_reqs++;
	thread_park_and_unlock_np(&k->io_pair_lock);
	memcpy(dest, spdk_dest, lba_count * block_size);
	preempt_disable();
	spdk_free(spdk_dest);
	preempt_enable();
	return 0;
}

/*
 * storage_block_size - get the size of a block from the nvme device
 */
int storage_block_size()
{
	return block_size;
}

/*
 * storage_num_blocks - gets the number of blocks from the nvme device
 */
int storage_num_blocks()
{
	return num_blocks;
}

#endif
