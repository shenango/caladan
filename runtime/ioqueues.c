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

#define PACKET_QUEUE_MCOUNT	4096
#define COMMAND_QUEUE_MCOUNT	4096
/* the egress buffer pool must be large enough to fill all the TXQs entirely */
#define EGRESS_POOL_SIZE(nks) \
	(PACKET_QUEUE_MCOUNT * MBUF_DEFAULT_LEN * MAX(1, guaranteedks) * 16UL)

struct iokernel_control iok;

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
	BUILD_ASSERT(ETH_MAX_LEN + sizeof(struct tx_net_hdr) <=
			MBUF_DEFAULT_LEN);
	BUILD_ASSERT(PGSIZE_2MB % MBUF_DEFAULT_LEN == 0);
	ret += EGRESS_POOL_SIZE(maxks);
	ret = align_up(ret, PGSIZE_2MB);

#ifdef DIRECTPATH
	// mlx5 directpath
	if (cfg_directpath_enabled)
		ret += PGSIZE_2MB;
#endif

#ifdef DIRECT_STORAGE
	// SPDK Memory - TODO: size this correctly
	if (cfg_storage_enabled) {
		ret += 5 * PGSIZE_2MB;
		ret += 2 * maxks * PGSIZE_2MB;
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
	int i, ret;
	struct thread_spec *ts;

	if (!netcfg.mac.addr[0]) {
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
		return -1;
	}
	netcfg.rx_region.len = INGRESS_MBUF_SHM_SIZE;
	iok.iok_info = (struct iokernel_info *)netcfg.rx_region.base;

	/* set up queues in shared memory */
	iok.hdr = iok_shm_alloc(sizeof(*iok.hdr), 0, NULL);
	iok.threads = iok_shm_alloc(sizeof(*ts) * maxks, 0, NULL);
	runtime_congestion = iok_shm_alloc(sizeof(struct congestion_info),
					   0, NULL);

	for (i = 0; i < maxks; i++) {
		ts = &iok.threads[i];
		ioqueue_alloc(&ts->rxq, PACKET_QUEUE_MCOUNT, false);
		ioqueue_alloc(&ts->txpktq, PACKET_QUEUE_MCOUNT, true);
		ioqueue_alloc(&ts->txcmdq, COMMAND_QUEUE_MCOUNT, true);

		iok_shm_alloc(sizeof(struct q_ptrs), CACHE_LINE_SIZE, &ts->q_ptrs);
		ts->rxq.wb = ts->q_ptrs;
	}

	iok.tx_len = EGRESS_POOL_SIZE(maxks);
	iok.tx_buf = iok_shm_alloc(iok.tx_len, PGSIZE_2MB, NULL);

	return 0;
}

static void ioqueues_shm_cleanup(void)
{
	mem_unmap_shm(netcfg.tx_region.base);
	mem_unmap_shm(netcfg.rx_region.base);
}

/*
 * Send an array of n file descriptors fds on the unix control socket fd.
 * Returns the number of bytes sent on success or -1 on error.
 */
static ssize_t ioqueues_send_fds(int fd, int *fds, int n)
{
	struct msghdr msg;
	char buf[CMSG_SPACE(sizeof(int) * n)];
	struct iovec iov[1];
	char iobuf[1];
	struct cmsghdr *cmptr;

	/* init message header, iovec is necessary even though it's unused */
	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);
	iov[0].iov_base = iobuf;
	iov[0].iov_len = sizeof(iobuf);
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	/* init control message */
	cmptr = CMSG_FIRSTHDR(&msg);
	cmptr->cmsg_len = CMSG_LEN(sizeof(int) * n);
	cmptr->cmsg_level = SOL_SOCKET;
	cmptr->cmsg_type = SCM_RIGHTS;
	memcpy((int *) CMSG_DATA(cmptr), fds, n * sizeof(int));

	return sendmsg(fd, &msg, 0);
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
	int ret, i;
	int kthread_fds[NCPU];

	/* initialize control header */
	hdr = iok.hdr;
	BUG_ON((uintptr_t)iok.hdr != (uintptr_t)r->base);
	hdr->magic = CONTROL_HDR_MAGIC;
	hdr->version_no = CONTROL_HDR_VERSION;
	hdr->egress_buf_count = div_up(iok.tx_len, MBUF_DEFAULT_LEN);
	hdr->thread_count = maxks;
	hdr->mac = netcfg.mac;
	hdr->congestion_info = ptr_to_shmptr(r, runtime_congestion,
					     sizeof(struct congestion_info));

	hdr->sched_cfg.priority = SCHED_PRIORITY_NORMAL;
	hdr->sched_cfg.max_cores = maxks;
	hdr->sched_cfg.guaranteed_cores = guaranteedks;
	hdr->sched_cfg.congestion_latency_us = 0;
	hdr->sched_cfg.scaleout_latency_us = 0;
	hdr->sched_cfg.preferred_socket = preferred_socket;

	hdr->thread_specs = ptr_to_shmptr(r, iok.threads, sizeof(*iok.threads) * maxks);

	/* register with iokernel */
	BUILD_ASSERT(strlen(CONTROL_SOCK_PATH) <= sizeof(addr.sun_path) - 1);
	memset(&addr, 0x0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

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

	/* send efds to iokernel */
	for (i = 0; i < maxks; i++)
		kthread_fds[i] = iok.threads[i].park_efd;
	ret = ioqueues_send_fds(iok.fd, &kthread_fds[0], maxks);
	if (ret < 0) {
		log_err("register_iokernel: ioqueues_send_fds() failed with ret %d",
				ret);
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
	pid_t tid = gettid();
	struct shm_region *r = &netcfg.tx_region;

	struct thread_spec *ts = &iok.threads[myk()->kthread_idx];
	ts->tid = tid;
	ts->park_efd = myk()->park_efd;

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
