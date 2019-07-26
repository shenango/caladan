/*
 * control.h - the control interface for the I/O kernel
 */

#pragma once

#include <sys/types.h>

#include <base/limits.h>
#include <iokernel/shm.h>
#include <net/ethernet.h>

/*
 * WARNING: If you make any changes that impact the layout of
 * struct control_hdr, please increment the version number!
 */

#define CONTROL_HDR_VERSION 4

/* The abstract namespace path for the control socket. */
#define CONTROL_SOCK_PATH	"\0/control/iokernel.sock"

/* describes a queue */
struct q_ptrs {
	uint32_t		rxq_wb; /* must be first */
	uint32_t		rq_head;
	uint32_t		rq_tail;
	uint32_t		directpath_rx_tail;
	uint64_t		next_timer_tsc;
	uint32_t		storage_tail;
};

struct congestion_info {
	float			load;
	uint64_t		standing_queue_us;
};

enum {
	HWQ_INVALID = 0,
	HWQ_MLX5,
	HWQ_SPDK_NVME,
	NR_HWQ,
};

struct hardware_queue_spec {
	shmptr_t		descriptor_table;
	shmptr_t		consumer_idx;
	uint32_t		descriptor_log_size;
	uint32_t		nr_descriptors;
	uint32_t		parity_byte_offset;
	uint32_t		parity_bit_mask;
	uint32_t		hwq_type;
};

struct timer_spec {
	shmptr_t		next_tsc;
	unsigned long		timer_resolution;
};


/* describes a runtime kernel thread */
struct thread_spec {
	struct queue_spec	rxq;
	struct queue_spec	txpktq;
	struct queue_spec	txcmdq;
	shmptr_t		q_ptrs;
	pid_t			tid;
	int32_t			park_efd;

	struct hardware_queue_spec		direct_rxq;
	struct hardware_queue_spec		storage_hwq;
	struct timer_spec		timer_heap;
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
	unsigned int		preferred_socket;
};

#define CONTROL_HDR_MAGIC	0x696f6b3a /* "iok:" */

/* the main control header */
struct control_hdr {
	unsigned int		version_no;
	unsigned int		magic;
	unsigned int		thread_count;
	unsigned long		egress_buf_count;
	shmptr_t		congestion_info;
	struct eth_addr		mac;
	struct sched_spec	sched_cfg;
	shmptr_t		thread_specs;
};
