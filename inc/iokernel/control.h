/*
 * control.h - the control interface for the I/O kernel
 */

#pragma once

#include <sys/types.h>

#include <base/limits.h>
#include <iokernel/shm.h>
#include <net/ethernet.h>

/* The abstract namespace path for the control socket. */
#define CONTROL_SOCK_PATH	"\0/control/iokernel.sock"

/* describes a queue */
struct q_ptrs {
	uint32_t		rxq_wb; /* must be first */
	uint32_t		rq_head;
	uint32_t		rq_tail;
};

/* describes a runtime kernel thread */
struct thread_spec {
	struct queue_spec	rxq;
	struct queue_spec	txpktq;
	struct queue_spec	txcmdq;
	shmptr_t		q_ptrs;
	pid_t			tid;
	int32_t			park_efd;
	shmptr_t		nvme_qpair_cpl;
	shmptr_t		nvme_qpair_cq_head;
	shmptr_t		nvme_qpair_phase;
};

enum {
	SCHED_PRIORITY_SYSTEM = 0, /* high priority, system-level services */
	SCHED_PRIORITY_NORMAL,     /* normal priority, typical tasks */
	SCHED_PRIORITY_BATCH,      /* low priority, batch processing */
};

/* describes scheduler options */
struct sched_spec {
	unsigned int		priority;
	unsigned int		max_cores;
	unsigned int		guaranteed_cores;
	unsigned int		congestion_latency_us;
	unsigned int		scaleout_latency_us;
};

#define CONTROL_HDR_MAGIC	0x696f6b3a /* "iok:" */

/* the main control header */
struct control_hdr {
	unsigned int		magic;
	unsigned int		thread_count;
	unsigned long		egress_buf_count;
	shmptr_t		congestion_signal;
	int			spdk_shm_id;
	struct eth_addr		mac;
	struct sched_spec	sched_cfg;
	struct thread_spec	threads[];
};
