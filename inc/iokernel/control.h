/*
 * control.h - the control interface for the I/O kernel
 */

#pragma once

#include <sys/types.h>

#include <base/atomic.h>
#include <base/bitmap.h>
#include <base/limits.h>
#include <base/pci.h>
#include <iokernel/shm.h>
#include <net/ethernet.h>

/*
 * WARNING: If you make any changes that impact the layout of
 * struct control_hdr, please increment the version number!
 */

#define CONTROL_HDR_VERSION 15

/* The abstract namespace path for the control socket. */
#define CONTROL_SOCK_PATH	"/run/iokernel.sock"

/* max device completion queues the iokernel monitors per kthread */
#define NR_CQS 8

/* describes a queue */
struct q_ptrs {
	/* first cache line: work indicators the iokernel reads on every
	 * poll of this kthread */
	uint32_t		rxq_wb; /* must be first */
	uint32_t		rq_head;
	uint32_t		rq_tail;
	uint32_t		directpath_rx_tail;
	uint64_t		next_timer_tsc;
	/* cq_outstanding[i] is 1 while completion queue i (see
	 * thread_spec::cqs) has requests in flight, and 0 otherwise. One
	 * byte per queue so writers need no atomics; the union allows one
	 * combined load. Strictly 0 or 1, never a count: cq_due_mask()
	 * gathers bit 0 of each byte, so any other value reads as not-due
	 * for that queue. */
	union {
		uint8_t		cq_outstanding[NR_CQS];
		uint64_t	cq_outstanding_word;
	};
	uint64_t		oldest_tsc;
	uint64_t		rcu_gen;
	uint64_t		run_start_tsc;
	uint64_t		directpath_strides_consumed;

	/* second cache line: consumer tails for the monitored completion
	 * queues (indexed by thread_spec::cqs slot), kept off the
	 * scheduler-written line below to avoid bouncing it; the union lets
	 * the iokernel snapshot all tails with a few wide loads */
	union {
		uint32_t	cq_tails[NR_CQS];
		uint64_t	cq_tails_words[NR_CQS / 2];
	};
	uint8_t			pad2[64 - sizeof(uint32_t) * NR_CQS];

	/* third cache line contains information written by the scheduler */
	uint64_t		curr_grant_gen;
	uint64_t		cede_gen;
	uint64_t		yield_rcu_gen;
	uint64_t		park_gen;
	uint64_t		pad3[4];
};

BUILD_ASSERT(sizeof(struct q_ptrs) == 3 * CACHE_LINE_SIZE);
BUILD_ASSERT(offsetof(struct q_ptrs, cq_tails) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(offsetof(struct q_ptrs, curr_grant_gen) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(offsetof(struct q_ptrs, cq_outstanding) % sizeof(uint64_t) == 0);

struct congestion_info {
	float			load;
	uint64_t		delay_us;
};

struct runtime_info {
	struct congestion_info congestion;
	uint64_t directpath_strides_posted;
	atomic64_t directpath_strides_consumed;
};

enum {
	HWQ_INVALID = 0,
	HWQ_MLX5,
	HWQ_MLX5_QSTEER,
	HWQ_SPDK_NVME,
	HWQ_GENERIC_CQ,	/* request/response ring with no special handling */
	NR_HWQ,
};

/* how the iokernel decides whether the completion-ring slot at the consumer
 * tail has been written by the device but not yet consumed by the runtime */
enum {
	CQ_DONE_INVALID = 0,	/* spec slot unused */
	CQ_DONE_PARITY,		/* done bit's phase flips on each ring wrap */
	CQ_DONE_NONZERO,	/* done bits nonzero until cleared for reuse */
};

/* describes a device completion queue monitored by the iokernel; its tail
 * and in-flight byte are published in q_ptrs at the index of this spec in
 * thread_spec::cqs */
struct cq_spec {
	shmptr_t		descriptor_table;
	uint32_t		descriptor_log_size;
	uint32_t		nr_descriptors;
	uint32_t		done_byte_offset; /* byte in slot w/ done bits */
	uint8_t			done_bit_mask;
	uint8_t			done_mode;	  /* CQ_DONE_* */
	uint8_t			hwq_type;	  /* HWQ_* */
};

/* describes a runtime kernel thread */
struct thread_spec {
	struct queue_spec	rxq;
	struct queue_spec	txpktq;
	struct queue_spec	txcmdq;
	shmptr_t		q_ptrs;
	pid_t			tid;
	int32_t			park_efd;

	struct cq_spec		cqs[NR_CQS];
};

enum {
	SCHED_PRIO_LC = 0, /* high priority, latency-critical task */
	SCHED_PRIO_BE,     /* low priority, best-effort task */
};

/* describes scheduler options */
struct sched_spec {
	unsigned int		priority;
	unsigned int		max_cores;
	unsigned int		guaranteed_cores;
	unsigned int		preferred_socket;
	uint64_t		qdelay_us;
	uint64_t		ht_punish_us;
	uint64_t		quantum_us;
};

#define CONTROL_HDR_MAGIC	0x696f6b3a /* "iok:" */

enum {
	DIRECTPATH_REQUEST_NONE = 0,
	DIRECTPATH_REQUEST_REGULAR = 1,
	DIRECTPATH_REQUEST_STRIDED_RMP = 2,
};

/* the main control header */
struct control_hdr {
	unsigned int		version_no;
	unsigned int		magic;
	unsigned int		thread_count;
	unsigned int		request_directpath_queues;
	unsigned long		egress_buf_count;
	shmptr_t		runtime_info;
	uint32_t		ip_addr;
	struct sched_spec	sched_cfg;
	shmptr_t		thread_specs;
	size_t			shared_reg_page_size;
};

/* information shared from iokernel to all runtimes */
struct iokernel_info {
	DEFINE_BITMAP(managed_cores, NCPU);
	unsigned char		rss_key[52];
	size_t			rss_key_len;
	struct pci_addr		directpath_pci;
	int			cycles_per_us;
	int			managed_numa_node;
	struct eth_addr		host_mac;
	bool			external_directpath_enabled;
	bool			external_directpath_rmp;
	bool			transparent_hugepages;
	bool			no_tx_offloads;
	uint8_t			min_pkt_size;
};

BUILD_ASSERT(sizeof(struct iokernel_info) <= IOKERNEL_INFO_SIZE);
