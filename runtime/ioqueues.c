/*
 * ioqueues.c
 */

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <base/hash.h>
#include <base/log.h>
#include <base/lrpc.h>
#include <base/mem.h>
#include <base/thread.h>

#include <iokernel/directpath.h>
#include <iokernel/shm.h>
#include <runtime/thread.h>

#include <net/ethernet.h>
#include <net/mbuf.h>

#include "defs.h"
#include "net/defs.h"

#define PACKET_QUEUE_MCOUNT	4096
#define LRPC_QUEUE_SIZE_DIRECTPATH 16

static size_t lrpc_q_size(void)
{
	if (cfg_directpath_external())
		return LRPC_QUEUE_SIZE_DIRECTPATH;

	return PACKET_QUEUE_MCOUNT;
}

/* the egress buffer pool must be large enough to fill all the TXQs entirely */
static size_t calculate_egress_pool_size(void)
{
	if (cfg_directpath_external())
		return 0;

	size_t buflen = MBUF_DEFAULT_LEN;
	return align_up(PACKET_QUEUE_MCOUNT *
			buflen * MAX(1, guaranteedks) * 8UL,
			PGSIZE_2MB);
}

struct iokernel_control iok;
bool cfg_prio_is_lc;
unsigned int cfg_request_hardware_queues = DIRECTPATH_REQUEST_NONE;
uint64_t cfg_ht_punish_us;
uint64_t cfg_qdelay_us = 10;
uint64_t cfg_quantum_us = 100;

static inline size_t shm_page_size(void)
{
	if (storage_enabled() || !cfg_directpath_external())
		return PGSIZE_2MB;

	return PGSIZE_4KB;
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
	ret += align_up(sizeof(struct runtime_info), CACHE_LINE_SIZE);

	// Shared queue pointers for the iokernel to use to determine busyness
	q = align_up(sizeof(struct q_ptrs), CACHE_LINE_SIZE);
	ret += q * maxks;

	// RX queues (wb is not included)
	q = sizeof(struct lrpc_msg) * lrpc_q_size();
	q = align_up(q, CACHE_LINE_SIZE);
	ret += q * maxks;

	// TX packet queues
	q = sizeof(struct lrpc_msg) * lrpc_q_size();
	q = align_up(q, CACHE_LINE_SIZE);
	q += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	ret += q * maxks;

	// TX command queues
	q = sizeof(struct lrpc_msg) * lrpc_q_size();
	q = align_up(q, CACHE_LINE_SIZE);
	q += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	ret += q * maxks;

	if (!cfg_directpath_external()) {
		ret = align_up(ret, PGSIZE_2MB);

		// Egress buffers
		BUILD_ASSERT(PGSIZE_2MB % MBUF_DEFAULT_LEN == 0);
		ret += calculate_egress_pool_size();
		ret = align_up(ret, PGSIZE_2MB);
	}

	// mlx5 directpath
	if (cfg_directpath_enabled() && !cfg_directpath_external()) {
		ret += PGSIZE_2MB * 4;
		if (is_directpath_strided())
			ret += PGSIZE_2MB * 28;
		ret += align_up(directpath_rx_buf_pool_sz(maxks), PGSIZE_2MB);
	}

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
		r->len = align_up(estimate_shm_space(), shm_page_size());
		r->base = mem_map_shm(iok.key, NULL, r->len, shm_page_size(), true);
		if (r->base == MAP_FAILED)
			panic("failed to map shared memory (requested %lu bytes)", r->len);
		log_info("shm: using %lu bytes", r->len);
	}

	if (alignment)
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

int ioqueues_init_early(void)
{
	void *shbuf;

	shbuf = mem_map_shm_rdonly(IOKERNEL_INFO_KEY, NULL, IOKERNEL_INFO_SIZE,
	                           PGSIZE_4KB);
	if (unlikely(shbuf == MAP_FAILED)) {
		log_err("control_setup: failed to map iokernel info region");
		log_err("Please make sure IOKernel is running");
		return -1;
	}

	iok.iok_info = (struct iokernel_info *)shbuf;
	memcpy(&netcfg.mac, &iok.iok_info->host_mac, sizeof(netcfg.mac));

#ifdef DIRECTPATH
	if (iok.iok_info->external_directpath_enabled) {
		cfg_directpath_strided = iok.iok_info->external_directpath_rmp;
		cfg_directpath_mode = DIRECTPATH_MODE_EXTERNAL;
	}
#endif

	if (iok.iok_info->transparent_hugepages)
		cfg_transparent_hugepages_enabled = true;

	return 0;
}

/*
 * General initialization for runtime <-> iokernel communication. Must be
 * called before per-thread ioqueues initialization.
 */
int ioqueues_init(void)
{
	int i;
	struct thread_spec *ts;

	iok.key = netcfg.addr;

	if (!cfg_directpath_external()) {
		/* map ingress memory */
		netcfg.rx_region.base =
		    mem_map_shm_rdonly(INGRESS_MBUF_SHM_KEY, NULL, INGRESS_MBUF_SHM_SIZE,
				PGSIZE_4KB);
		if (netcfg.rx_region.base == MAP_FAILED) {
			log_err("control_setup: failed to map ingress region");
			log_err("Please make sure IOKernel is running");
			return -1;
		}
		netcfg.rx_region.len = INGRESS_MBUF_SHM_SIZE;
	}

	/* set up queues in shared memory */
	iok.hdr = iok_shm_alloc(sizeof(*iok.hdr), 0, NULL);
	iok.threads = iok_shm_alloc(sizeof(*ts) * maxks, 0, NULL);
	runtime_info = iok_shm_alloc(sizeof(struct runtime_info),
	                             0, &iok.hdr->runtime_info);

	/* first allocate q_ptrs in a contiguous array */
	for (i = 0; i < maxks; i++) {
		ts = &iok.threads[i];
		iok_shm_alloc(sizeof(struct q_ptrs), CACHE_LINE_SIZE, &ts->q_ptrs);
	}

	/* then allocate lrpc rings */
	for (i = 0; i < maxks; i++) {
		ts = &iok.threads[i];
		ioqueue_alloc(&ts->rxq, lrpc_q_size(), false);
		ioqueue_alloc(&ts->txpktq, lrpc_q_size(), true);
		ioqueue_alloc(&ts->txcmdq, lrpc_q_size(), true);

		ts->rxq.wb = ts->q_ptrs;
	}

	/* don't allocate buffer space, iokernel will provide */
	if (cfg_directpath_external())
		return 0;

	iok.tx_len = calculate_egress_pool_size();
	iok.tx_buf = iok_shm_alloc(iok.tx_len, PGSIZE_2MB, NULL);

	if (cfg_directpath_enabled()) {
		iok.rx_len = directpath_rx_buf_pool_sz(maxks);
		iok.rx_buf = iok_shm_alloc(iok.rx_len, PGSIZE_2MB, NULL);
	}

	return 0;
}

static void ioqueues_shm_cleanup(void)
{
	mem_unmap_shm(netcfg.tx_region.base);
	mem_unmap_shm(netcfg.rx_region.base);
}

#ifdef DIRECTPATH
static int recv_fd(int fd, int *fd_out)
{
	struct msghdr msg;
	char buf[CMSG_SPACE(sizeof(int))];
	struct iovec iov[1];
	char iobuf[1];
	ssize_t ret;
	struct cmsghdr *cmptr;

	/* init message header and buffs for control message and iovec */
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);
	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov[0].iov_base = iobuf;
	iov[0].iov_len = sizeof(iobuf);
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	ret = recvmsg(fd, &msg, 0);
	if (ret < 0) {
		log_debug("control: error with recvmsg %ld", ret);
		return ret;
	}

	/* check validity of control message */
	cmptr = CMSG_FIRSTHDR(&msg);
	if (cmptr == NULL) {
		log_debug("control: no cmsg %p", cmptr);
		return -1;
	} else if (cmptr->cmsg_len != CMSG_LEN(sizeof(int))) {
		log_debug("control: cmsg is too long %ld", cmptr->cmsg_len);
		return -1;
	} else if (cmptr->cmsg_level != SOL_SOCKET) {
		log_debug("control: unrecognized cmsg level %d", cmptr->cmsg_level);
		return -1;
	} else if (cmptr->cmsg_type != SCM_RIGHTS) {
		log_debug("control: unrecognized cmsg type %d", cmptr->cmsg_type);
		return -1;
	}

	*fd_out = *(int *)CMSG_DATA(cmptr);
	return 0;
}

static int setup_external_directpath(int controlfd)
{
	int memfd = -1, ret, barfd = -1;
	size_t specsz;
	ssize_t rret;
	struct directpath_spec *spec;
	struct directpath_queue_spec *qspec;

	specsz = sizeof(*spec) + sizeof(*qspec) * maxks;
	spec = malloc(specsz);
	if (unlikely(!spec))
		return -ENOMEM;

	ret = recv_fd(controlfd, &memfd);
	if (unlikely(ret < 0)) {
		log_err("bad recv");
		goto done;
	}

	ret = recv_fd(controlfd, &barfd);
	if (unlikely(ret < 0)) {
		log_err("bad recv");
		goto done;
	}

	rret = read(controlfd, spec, specsz);
	if (unlikely(rret != specsz)) {
		log_err("bad read");
		ret = -1;
		goto done;
	}

	ret = mlx5_init_ext_late(spec, barfd, memfd);

done:
	free(spec);
	return ret;
}
#endif

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
	hdr->ip_addr = netcfg.addr;

	hdr->request_directpath_queues = cfg_request_hardware_queues;

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
	BUILD_ASSERT(sizeof(CONTROL_SOCK_PATH) <= sizeof(addr.sun_path));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, CONTROL_SOCK_PATH, sizeof(CONTROL_SOCK_PATH));

	iok.fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (iok.fd == -1) {
		log_err("register_iokernel: socket() failed [%s]", strerror(errno));
		goto fail;
	}

	if (connect(iok.fd, (struct sockaddr *)&addr,
		 sizeof(addr.sun_family) + sizeof(CONTROL_SOCK_PATH)) == -1) {
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

#ifdef DIRECTPATH
	if (cfg_request_hardware_queues) {
		ret = setup_external_directpath(iok.fd);
		if (ret) {
			log_err("dp setup error");
			goto fail_close_fd;
		}
	}
#endif

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
			sizeof(struct q_ptrs));
	BUG_ON(!myk()->q_ptrs);

	return 0;
}
