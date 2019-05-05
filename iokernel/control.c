/*
 * control.c - the control-plane for the I/O kernel
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <base/stddef.h>
#include <base/mem.h>
#include <base/log.h>
#include <base/thread.h>
#include <iokernel/control.h>

#include "defs.h"

#include <sys/mman.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

static int controlfd;
static int clientfds[IOKERNEL_MAX_PROC];
static struct proc *clients[IOKERNEL_MAX_PROC];
static int nr_clients;
struct lrpc_params lrpc_control_to_data_params;
struct lrpc_params lrpc_data_to_control_params;
static struct lrpc_chan_out lrpc_control_to_data;
static struct lrpc_chan_in lrpc_data_to_control;
static int nr_guaranteed;

#if __has_include("spdk/nvme.h")
static uint64_t spdk_map_idx;
static int map_spdk_hugepages(struct shm_region* reg, unsigned int shm_id) {
	int page_no, fd;
	uint64_t offset;
	char buffer [50];

	reg->base = (void *)SPDK_BASE_ADDR +
		SPDK_BASE_ADDR_OFFSET * spdk_map_idx;
	reg->len = 0;

	for (page_no = 0; page_no < SPDK_NUM_PAGES_MAPPED; page_no++) {
		offset = 0x200000 * (page_no + 1);
		sprintf(buffer, "/dev/hugepages/spdk%umap_%d", shm_id, page_no);
		fd = open(buffer, O_RDONLY);
		if (fd == -1) {
			if (errno == 2) {
				close(fd);
				break;
			} else {
				log_err("error opening spdk page, errno=%d", errno);
				close(fd);
				goto spdk_unmap;
			}
		}
		if ((void *)-1 == mmap(reg->base + offset, PGSIZE_2MB, PROT_READ,
					MAP_SHARED|MAP_FIXED|MAP_POPULATE, fd, 0)) {
			log_err("error mapping spdk page, errno=%d", errno);
			close(fd);
			goto spdk_unmap;
		}
		reg->len += PGSIZE_2MB;
		close(fd);
	}
	spdk_map_idx++;
	return 0;

spdk_unmap:
	if (-1 == munmap(reg->base, reg->len)) {
		log_err("error unmapping spdk pages, errno=%d", errno);
	}
	return -1;
}
#endif


static struct proc *control_create_proc(mem_key_t key, size_t len, pid_t pid,
		int *fds, int n_fds)
{
	struct control_hdr hdr;
	struct shm_region reg;
	size_t nr_pages;
	struct proc *p;
	struct thread_spec *threads;
	void *shbuf;
	int i, ret;
#if __has_include("spdk/nvme.h")
	struct shm_region spdk_reg;
#endif

	/* attach the shared memory region */
	if (len < sizeof(hdr))
		goto fail;
	shbuf = mem_map_shm(key, NULL, len, PGSIZE_2MB, false);
	if (shbuf == MAP_FAILED)
		goto fail;

	/* parse the control header */
	memcpy(&hdr, (struct control_hdr *)shbuf, sizeof(hdr)); /* TOCTOU */
	if (hdr.magic != CONTROL_HDR_MAGIC)
		goto fail_unmap;
	if (hdr.thread_count > NCPU || hdr.thread_count == 0 ||
			hdr.thread_count != n_fds)
		goto fail_unmap;

	if (hdr.sched_cfg.guaranteed_cores + nr_guaranteed > get_total_cores()) {
		log_err("guaranteed cores exceeds total core count");
		goto fail_unmap;
	}

	nr_guaranteed += hdr.sched_cfg.guaranteed_cores;

	/* create the process */
	nr_pages = div_up(len, PGSIZE_2MB);
	p = malloc(sizeof(*p) + nr_pages * sizeof(physaddr_t));
	if (!p)
		goto fail_unmap;

	threads = malloc(sizeof(*threads) * hdr.thread_count);
	if (!threads)
		goto fail_free_just_proc;
	memcpy(threads, ((struct control_hdr *)shbuf)->threads,
		sizeof(*threads) * hdr.thread_count);

	p->pid = pid;
	ref_init(&p->ref);
	reg.base = shbuf;
	reg.len = len;
	p->region = reg;
	p->removed = false;
	p->sched_cfg = hdr.sched_cfg;
	p->thread_count = hdr.thread_count;
	if (eth_addr_is_multicast(&hdr.mac) || eth_addr_is_zero(&hdr.mac))
		goto fail_free_proc;
	p->mac = hdr.mac;
	p->uniqid = rdtsc();
	p->congestion_signal =
		(int *)shmptr_to_ptr(&reg, hdr.congestion_signal, sizeof(int));
	if (!p->congestion_signal)
		goto fail_free_proc;
	*p->congestion_signal = false;

#if __has_include("spdk/nvme.h")
	if (map_spdk_hugepages(&spdk_reg, hdr.spdk_shm_id))
		goto fail_free_proc;
#endif

	/* initialize the threads */
	for (i = 0; i < hdr.thread_count; i++) {
		struct thread *th = &p->threads[i];
		struct thread_spec *s = &threads[i];

		/* attach the RX queue */
		ret = shm_init_lrpc_out(&reg, &s->rxq, &th->rxq);
		if (ret)
			goto fail_free_proc;

		/* attach the TX packet queue */
		ret = shm_init_lrpc_in(&reg, &s->txpktq, &th->txpktq);
		if (ret)
			goto fail_free_proc;

		/* attach the TX command queue */
		ret = shm_init_lrpc_in(&reg, &s->txcmdq, &th->txcmdq);
		if (ret)
			goto fail_free_proc;

#if __has_include("spdk/nvme.h")
		/* set SPDK pointers */
		p->nvmeq[i].cpl_ref = (struct spdk_nvme_cpl *)shmptr_to_ptr(&spdk_reg,
				(shmptr_t)s->nvme_qpair_cpl,
				sizeof(p->nvmeq[i].cpl_ref));
		if (!p->nvmeq[i].cpl_ref)
			goto fail_free_proc;
		p->nvmeq[i].nvme_io_cq_head = (uint16_t *)shmptr_to_ptr(&spdk_reg,
				(shmptr_t)s->nvme_qpair_cq_head,
				sizeof(p->nvmeq[i].nvme_io_cq_head));
		if (!p->nvmeq[i].nvme_io_cq_head)
			goto fail_free_proc;
		p->nvmeq[i].nvme_io_phase = (uint8_t *)shmptr_to_ptr(&spdk_reg,
				(shmptr_t)s->nvme_qpair_phase,
				sizeof(p->nvmeq[i].nvme_io_phase));
		if (!p->nvmeq[i].nvme_io_phase)
			goto fail_free_proc;
#endif

		th->tid = s->tid;
		th->park_efd = fds[i];
		th->p = p;
		th->state = THREAD_STATE_IDLE;
		th->reaffinitize = true;
		th->at_idx = -1;
		th->ts_idx = -1;

		/* initialize pointer to queue pointers in shared memory */
		th->q_ptrs = (struct q_ptrs *) shmptr_to_ptr(&reg, s->q_ptrs,
				sizeof(struct q_ptrs));
		if (!th->q_ptrs)
			goto fail_free_proc;
	}

	free(threads);

	/* initialize the table of physical page addresses */
	ret = mem_lookup_page_phys_addrs(p->region.base, p->region.len, PGSIZE_2MB,
			p->page_paddrs);
	if (ret)
		goto fail_free_just_proc;

	p->max_overflows = hdr.egress_buf_count;
	p->nr_overflows = 0;
	p->overflow_queue = malloc(sizeof(unsigned long) * p->max_overflows);
	if (p->overflow_queue == NULL)
		goto fail_free_just_proc;

	return p;

fail_free_proc:
	free(threads);
fail_free_just_proc:
	free(p);
fail_unmap:
	mem_unmap_shm(shbuf);
fail:
	log_err("control: couldn't attach pid %d", pid);
	return NULL;
}

static void control_destroy_proc(struct proc *p)
{
	int i;

	/* close eventfds */
	for (i = 0; i < p->thread_count; i++)
		close(p->threads[i].park_efd);

	nr_guaranteed -= p->sched_cfg.guaranteed_cores;
	mem_unmap_shm(p->region.base);
	free(p->overflow_queue);
	free(p);
}

/*
 * Receive up to n file descriptors on the unix control socket fd, write them
 * to the array fds. Returns the number of fds on success, < 0 on error.
 */
static int control_recv_fds(int fd, int *fds, int n)
{
	struct msghdr msg;
	char buf[CMSG_SPACE(sizeof(int) * n)];
	struct iovec iov[1];
	char iobuf[1];
	ssize_t ret;
	struct cmsghdr *cmptr;
	int n_fds;

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
	} else if (cmptr->cmsg_len > CMSG_LEN(sizeof(int) * n)) {
		log_debug("control: cmsg is too long %ld", cmptr->cmsg_len);
		return -1;
	} else if (cmptr->cmsg_level != SOL_SOCKET) {
		log_debug("control: unrecognized cmsg level %d", cmptr->cmsg_level);
		return -1;
	} else if (cmptr->cmsg_type != SCM_RIGHTS) {
		log_debug("control: unrecognized cmsg type %d", cmptr->cmsg_type);
		return -1;
	}

	/* determine how many descriptors we received, copy to fds */
	n_fds = 0;
	while (CMSG_LEN(sizeof(int) * n_fds) < cmptr->cmsg_len)
		n_fds++;
	memcpy(fds, (int *) CMSG_DATA(cmptr), n_fds * sizeof(int));

	return n_fds;
}

static void control_add_client(void)
{
	struct proc *p;
	struct ucred ucred;
	socklen_t len;
	mem_key_t shm_key;
	size_t shm_len;
	ssize_t ret;
	int fd, n_fds, i;
	int fds[NCPU];

	fd = accept(controlfd, NULL, NULL);
	if (fd == -1) {
		log_err("control: accept() failed [%s]", strerror(errno));
		return;
	}

	if (nr_clients >= IOKERNEL_MAX_PROC) {
		log_err("control: hit client process limit");
		goto fail;
	}

	len = sizeof(struct ucred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1) {
		log_err("control: getsockopt() failed [%s]", strerror(errno));
		goto fail;
	}

	ret = read(fd, &shm_key, sizeof(shm_key));
	if (ret != sizeof(shm_key)) {
		log_err("control: read() failed, len=%ld [%s]",
			ret, strerror(errno));
		goto fail;
	}

	ret = read(fd, &shm_len, sizeof(shm_len));
	if (ret != sizeof(shm_len)) {
		log_err("control: read() failed, len=%ld [%s]",
			ret, strerror(errno));
		goto fail;
	}

	n_fds = control_recv_fds(fd, &fds[0], NCPU);
	if (n_fds <= 0) {
		log_err("control: control_recv_fds() failed with ret %d", n_fds);
		goto fail;
	}

	p = control_create_proc(shm_key, shm_len, ucred.pid, &fds[0], n_fds);
	if (!p) {
		log_err("control: failed to create process '%d'", ucred.pid);
		goto fail_close_fds;
	}

	if (!lrpc_send(&lrpc_control_to_data, DATAPLANE_ADD_CLIENT,
			(unsigned long) p)) {
		log_err("control: failed to inform dataplane of new client '%d'",
				ucred.pid);
		goto fail_destroy_proc;
	}

	clients[nr_clients] = p;
	clientfds[nr_clients++] = fd;
	return;

fail_destroy_proc:
	control_destroy_proc(p);
fail_close_fds:
	for (i = 0; i < n_fds; i++)
		close(fds[i]);
fail:
	close(fd);
}

static void control_instruct_dataplane_to_remove_client(int fd)
{
	int i;

	for (i = 0; i < nr_clients; i++) {
		if (clientfds[i] == fd)
			break;
	}

	if (i == nr_clients) {
		WARN();
		return;
	}

	clients[i]->removed = true;
	if (!lrpc_send(&lrpc_control_to_data, DATAPLANE_REMOVE_CLIENT,
			(unsigned long) clients[i])) {
		log_err("control: failed to inform dataplane of removed client");
	}
}

static void control_remove_client(struct proc *p)
{
	int i;

	for (i = 0; i < nr_clients; i++) {
		if (clients[i] == p)
			break;
	}

	if (i == nr_clients) {
		WARN();
		return;
	}

	control_destroy_proc(p);
	clients[i] = clients[nr_clients - 1];

	close(clientfds[i]);
	clientfds[i] = clientfds[nr_clients - 1];
	nr_clients--;
}

static void control_loop(void)
{
	fd_set readset;
	int maxfd, i, nrdy;
	uint64_t cmd;
	unsigned long payload;
	struct proc *p;

	while (1) {
		maxfd = controlfd;
		FD_ZERO(&readset);
		FD_SET(controlfd, &readset);

		for (i = 0; i < nr_clients; i++) {
			if (clients[i]->removed)
				continue;

			FD_SET(clientfds[i], &readset);
			maxfd = (clientfds[i] > maxfd) ? clientfds[i] : maxfd;
		}

		nrdy = select(maxfd + 1, &readset, NULL, NULL, NULL);
		if (nrdy == -1) {
			log_err("control: select() failed [%s]",
				strerror(errno));
			BUG();
		}

		for (i = 0; i <= maxfd && nrdy > 0; i++) {
			if (!FD_ISSET(i, &readset))
				continue;

			if (i == controlfd) {
				/* accept a new connection */
				control_add_client();
			} else {
				/* close an existing connection */
				control_instruct_dataplane_to_remove_client(i);
			}

			nrdy--;
		}

		while (lrpc_recv(&lrpc_data_to_control, &cmd, &payload)) {
			p = (struct proc *) payload;
			assert(cmd == CONTROL_PLANE_REMOVE_CLIENT);

			/* it is now safe to remove data structures for this client */
			control_remove_client(p);
		}
	}
}

static void *control_thread(void *data)
{
	int ret;

	/* pin to our assigned core */
	ret = cores_pin_thread(gettid(), core_assign.ctrl_core);
	if (ret < 0) {
		log_err("control: failed to pin control thread to core %d",
				core_assign.ctrl_core);
		/* continue running but performance is unpredictable */
	}

	control_loop();
	return NULL;
}

/*
 * Initialize channels for communicating with the I/O kernel dataplane.
 */
static int control_init_dataplane_comm(void)
{
	int ret;
	struct lrpc_msg *buffer_out, *buffer_in;
	uint32_t *wb_out, *wb_in;

	buffer_out = malloc(sizeof(struct lrpc_msg) *
			CONTROL_DATAPLANE_QUEUE_SIZE);
	if (!buffer_out)
		goto fail;
	wb_out = malloc(CACHE_LINE_SIZE);
	if (!wb_out)
		goto fail_free_buffer_out;

	lrpc_control_to_data_params.buffer = buffer_out;
	lrpc_control_to_data_params.wb = wb_out;

	ret = lrpc_init_out(&lrpc_control_to_data,
			lrpc_control_to_data_params.buffer, CONTROL_DATAPLANE_QUEUE_SIZE,
			lrpc_control_to_data_params.wb);
	if (ret < 0) {
		log_err("control: initializing LRPC to dataplane failed");
		goto fail_free_wb_out;
	}

	buffer_in = malloc(sizeof(struct lrpc_msg) * CONTROL_DATAPLANE_QUEUE_SIZE);
	if (!buffer_in)
		goto fail_free_wb_out;
	wb_in = malloc(CACHE_LINE_SIZE);
	if (!wb_in)
		goto fail_free_buffer_in;

	lrpc_data_to_control_params.buffer = buffer_in;
	lrpc_data_to_control_params.wb = wb_in;

	ret = lrpc_init_in(&lrpc_data_to_control,
			lrpc_data_to_control_params.buffer, CONTROL_DATAPLANE_QUEUE_SIZE,
			lrpc_data_to_control_params.wb);
	if (ret < 0) {
		log_err("control: initializing LRPC from dataplane failed");
		goto fail_free_wb_in;
	}

	return 0;

fail_free_wb_in:
	free(wb_in);
fail_free_buffer_in:
	free(buffer_in);
fail_free_wb_out:
	free(wb_out);
fail_free_buffer_out:
	free(buffer_out);
fail:
	return -1;
}

int control_init(void)
{
	struct sockaddr_un addr;
	pthread_t tid;
	int sfd, ret;

	BUILD_ASSERT(strlen(CONTROL_SOCK_PATH) <= sizeof(addr.sun_path) - 1);

	memset(&addr, 0x0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

	sfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sfd == -1) {
		log_err("control: socket() failed [%s]", strerror(errno));
		return -errno;
	}

	if (bind(sfd, (struct sockaddr *)&addr,
		 sizeof(struct sockaddr_un)) == -1) {
		log_err("control: bind() failed [%s]", strerror(errno));
		close(sfd);
		return -errno;
	}

	if (listen(sfd, 100) == -1) {
		log_err("control: listen() failed[%s]", strerror(errno));
		close(sfd);
		return -errno;
	}

	ret = control_init_dataplane_comm();
	if (ret < 0) {
		log_err("control: cannot initialize communication with dataplane");
		return ret;
	}

	log_info("control: spawning control thread");
	controlfd = sfd;
	if (pthread_create(&tid, NULL, control_thread, NULL) == -1) {
		log_err("control: pthread_create() failed [%s]",
			strerror(errno));
		close(sfd);
		return -errno;
	}

	return 0;
}
