/*
 * ioqueues.c
 */

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <base/hash.h>
#include <base/log.h>
#include <base/lrpc.h>
#include <base/mem.h>
#include <base/thread.h>

#include <iokernel/shm.h>
#include <runtime/thread.h>

#include <net/ethernet.h>
#include <net/mbuf.h>

#include "defs.h"
#include "net/defs.h"

#define PACKET_QUEUE_MCOUNT	4096
#define COMMAND_QUEUE_MCOUNT	4096

/* the egress buffer pool must be large enough to fill all the TXQs entirely */
static size_t calculate_egress_pool_size(void)
{
	size_t buflen = MBUF_DEFAULT_LEN;
	return align_up(PACKET_QUEUE_MCOUNT *
			buflen * MAX(1, guaranteedks) * 8UL,
			PGSIZE_2MB);
}

struct iokernel_control iok;
bool cfg_prio_is_lc;
uint64_t cfg_ht_punish_us;
uint64_t cfg_qdelay_us = 10;
uint64_t cfg_quantum_us = 100;

static int generate_random_mac(struct eth_addr *mac)
{
	int fd, ret;
	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;

	ret = read(fd, mac, sizeof(*mac));
	close(fd);
	if (ret != sizeof(*mac))
		return -1;

	mac->addr[0] &= ~ETH_ADDR_GROUP;
	mac->addr[0] |= ETH_ADDR_LOCAL_ADMIN;

	return 0;
}

// Could be a macro really, this is totally static :/
static size_t estimate_shm_space(void)
{
	size_t ret = 0, q;

	// Header + queue_spec information
	ret += sizeof(struct control_hdr);
	ret += sizeof(struct thread_spec) * maxks;
	ret = align_up(ret, CACHE_LINE_SIZE);

	// Compute congestion signal line
	ret += CACHE_LINE_SIZE;

	// RX queues (wb is not included)
	q = sizeof(struct lrpc_msg) * PACKET_QUEUE_MCOUNT;
	q = align_up(q, CACHE_LINE_SIZE);
	ret += q * maxks;

	// TX packet queues
	q = sizeof(struct lrpc_msg) * PACKET_QUEUE_MCOUNT;
	q = align_up(q, CACHE_LINE_SIZE);
	q += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	ret += q * maxks;

	// TX command queues
	q = sizeof(struct lrpc_msg) * COMMAND_QUEUE_MCOUNT;
	q = align_up(q, CACHE_LINE_SIZE);
	q += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	ret += q * maxks;

	// Shared queue pointers for the iokernel to use to determine busyness
	q = align_up(sizeof(struct q_ptrs), CACHE_LINE_SIZE);
	ret += q * maxks;

	ret = align_up(ret, PGSIZE_2MB);

	// Egress buffers
	BUILD_ASSERT(PGSIZE_2MB % MBUF_DEFAULT_LEN == 0);
	ret += calculate_egress_pool_size();
	ret = align_up(ret, PGSIZE_2MB);

#ifdef DIRECTPATH
	// mlx5 directpath
	if (cfg_directpath_enabled)
		ret += PGSIZE_2MB * 4;
#endif

#ifdef DIRECT_STORAGE
	// SPDK completion queue memory
	if (cfg_storage_enabled) {
		/* sizeof(spdk_nvme_cpl) * default queue len * threads */
		ret += 16 * 4096 * maxks;
	}
#endif
	return ret;
}

/*
 * iok_shm_alloc - allocator for iokernel shared memory region
 * this is intended only for use during initialization.
 * panics if memory can't be allocated
 *
 */
void *iok_shm_alloc(size_t size, size_t alignment, shmptr_t *shm_out)
{
	static DEFINE_SPINLOCK(shmlock);
	static size_t allocated;
	struct shm_region *r = &netcfg.tx_region;
	void *p;

	spin_lock(&shmlock);
	if (!r->base) {
		r->len = estimate_shm_space();
		r->base = mem_map_shm(iok.key, NULL, r->len, PGSIZE_2MB, true);
		if (r->base == MAP_FAILED)
			panic("failed to map shared memory (requested %lu bytes)", r->len);
	}

	if (alignment < CACHE_LINE_SIZE)
		alignment = CACHE_LINE_SIZE;

	allocated = align_up(allocated, alignment);

	p = shmptr_to_ptr(r, allocated, size);
	BUG_ON(!p);

	if (shm_out)
		*shm_out = allocated;

	allocated += size;

	spin_unlock(&shmlock);

	return p;
}

static void ioqueue_alloc(struct queue_spec *q, size_t msg_count,
			  bool alloc_wb)
{
	iok_shm_alloc(sizeof(struct lrpc_msg) * msg_count, CACHE_LINE_SIZE, &q->msg_buf);

	if (alloc_wb)
		iok_shm_alloc(CACHE_LINE_SIZE, CACHE_LINE_SIZE, &q->wb);

	q->msg_count = msg_count;
}

/*
 * General initialization for runtime <-> iokernel communication. Must be
 * called before per-thread ioqueues initialization.
 */
int ioqueues_init(void)
{
	bool has_mac = false;
	int i, ret;
	struct thread_spec *ts;

	for (i = 0; i < ARRAY_SIZE(netcfg.mac.addr); i++)
		has_mac |= netcfg.mac.addr[i] != 0;

	if (!has_mac) {
		ret = generate_random_mac(&netcfg.mac);
		if (ret < 0)
			return ret;
	}

	BUILD_ASSERT(sizeof(netcfg.mac) >= sizeof(mem_key_t));
	iok.key = *(mem_key_t*)(&netcfg.mac);
	iok.key = rand_crc32c(iok.key);

	/* map ingress memory */
	netcfg.rx_region.base =
	    mem_map_shm_rdonly(INGRESS_MBUF_SHM_KEY, NULL, INGRESS_MBUF_SHM_SIZE,
			PGSIZE_2MB);
	if (netcfg.rx_region.base == MAP_FAILED) {
		log_err("control_setup: failed to map ingress region");
		log_err("Please make sure IOKernel is running");
		return -1;
	}
	netcfg.rx_region.len = INGRESS_MBUF_SHM_SIZE;
#if 0
	iok.iok_info = (struct iokernel_info *)netcfg.rx_region.base;
#endif

	/* set up queues in shared memory */
	iok.hdr = iok_shm_alloc(sizeof(*iok.hdr), 0, NULL);
	iok.threads = iok_shm_alloc(sizeof(*ts) * maxks, 0, NULL);
	runtime_congestion = iok_shm_alloc(sizeof(struct congestion_info),
					   0, &iok.hdr->congestion_info);

	for (i = 0; i < maxks; i++) {
		ts = &iok.threads[i];
		ioqueue_alloc(&ts->rxq, PACKET_QUEUE_MCOUNT, false);
		ioqueue_alloc(&ts->txpktq, PACKET_QUEUE_MCOUNT, true);
		ioqueue_alloc(&ts->txcmdq, COMMAND_QUEUE_MCOUNT, true);

		iok_shm_alloc(sizeof(struct q_ptrs), CACHE_LINE_SIZE, &ts->q_ptrs);
		ts->rxq.wb = ts->q_ptrs;
	}

	iok.tx_len = calculate_egress_pool_size();
	iok.tx_buf = iok_shm_alloc(iok.tx_len, PGSIZE_2MB, NULL);

	return 0;
}

static void ioqueues_shm_cleanup(void)
{
	mem_unmap_shm(netcfg.tx_region.base);
	mem_unmap_shm(netcfg.rx_region.base);
}

/*
 * Register this runtime with the IOKernel. All threads must complete their
 * per-thread ioqueues initialization before this function is called.
 */
int ioqueues_register_iokernel(void)
{
	struct control_hdr *hdr;
	struct shm_region *r = &netcfg.tx_region;
	struct sockaddr_un addr;
	int ret;

	/* initialize control header */
	hdr = iok.hdr;
	BUG_ON((uintptr_t)iok.hdr != (uintptr_t)r->base);
	hdr->magic = CONTROL_HDR_MAGIC;
	hdr->version_no = CONTROL_HDR_VERSION;
	/* TODO: overestimating is okay, but fix this later */
	hdr->egress_buf_count = div_up(iok.tx_len, net_get_mtu() + MBUF_HEAD_LEN);
	hdr->thread_count = maxks;
	hdr->mac = netcfg.mac;

	hdr->sched_cfg.priority = cfg_prio_is_lc ?
				  SCHED_PRIO_LC : SCHED_PRIO_BE;
	hdr->sched_cfg.ht_punish_us = cfg_ht_punish_us;
	hdr->sched_cfg.qdelay_us = cfg_qdelay_us;
	hdr->sched_cfg.max_cores = maxks;
	hdr->sched_cfg.guaranteed_cores = guaranteedks;
	hdr->sched_cfg.preferred_socket = preferred_socket;
	hdr->sched_cfg.quantum_us = cfg_quantum_us;
	hdr->thread_specs = ptr_to_shmptr(r, iok.threads, sizeof(*iok.threads) * maxks);

	// Make sure it's an abstract namespace path.
	assert(CONTROL_SOCK_PATH[0] == '\0');

	/* register with iokernel */
	BUILD_ASSERT(strlen(CONTROL_SOCK_PATH + 1) <= sizeof(addr.sun_path) - 2);
	memset(&addr, 0x0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path + 1, CONTROL_SOCK_PATH + 1, sizeof(addr.sun_path) - 2);

	iok.fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (iok.fd == -1) {
		log_err("register_iokernel: socket() failed [%s]", strerror(errno));
		goto fail;
	}

	if (connect(iok.fd, (struct sockaddr *)&addr,
		 sizeof(struct sockaddr_un)) == -1) {
		log_err("register_iokernel: connect() failed [%s]", strerror(errno));
		goto fail_close_fd;
	}

	ret = write(iok.fd, &iok.key, sizeof(iok.key));
	if (ret != sizeof(iok.key)) {
		log_err("register_iokernel: write() failed [%s]", strerror(errno));
		goto fail_close_fd;
	}

	ret = write(iok.fd, &netcfg.tx_region.len, sizeof(netcfg.tx_region.len));
	if (ret != sizeof(netcfg.tx_region.len)) {
		log_err("register_iokernel: write() failed [%s]", strerror(errno));
		goto fail_close_fd;
	}

	return 0;

fail_close_fd:
	close(iok.fd);
fail:
	ioqueues_shm_cleanup();
	return -errno;
}

int ioqueues_init_thread(void)
{
	int ret;
	pid_t tid = myk()->tid = thread_gettid();
	struct shm_region *r = &netcfg.tx_region;

	struct thread_spec *ts = &iok.threads[myk()->kthread_idx];
	ts->tid = tid;

	ret = shm_init_lrpc_in(r, &ts->rxq, &myk()->rxq);
	BUG_ON(ret);

	ret = shm_init_lrpc_out(r, &ts->txpktq, &myk()->txpktq);
	BUG_ON(ret);

	ret = shm_init_lrpc_out(r, &ts->txcmdq, &myk()->txcmdq);
	BUG_ON(ret);

	myk()->q_ptrs = (struct q_ptrs *) shmptr_to_ptr(r, ts->q_ptrs,
			sizeof(uint32_t));
	BUG_ON(!myk()->q_ptrs);

	return 0;
}
