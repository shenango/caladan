/*
 * defs.h - shared definitions local to the iokernel
 */

#pragma once

#include <base/stddef.h>
#include <base/bitmap.h>
#include <base/gen.h>
#include <base/lrpc.h>
#include <base/mem.h>
#include <base/pci.h>
#undef LIST_HEAD /* hack to deal with DPDK being annoying */
#include <base/list.h>
#include <iokernel/control.h>
#include <net/ethernet.h>

#include "ref.h"

#define STATS 1

/*
 * configuration parameters
 */

struct iokernel_cfg {
	bool	noht; /* disable hyperthreads */
	bool	nobw; /* disable bandwidth controller */
	bool	noidlefastwake; /* disable fast wakeups for idle processes */
	bool	ias_prefer_selfpair; /* prefer self-pairings */
	float	ias_bw_limit; /* IAS bw limit, (MB/s) */
	bool	no_hw_qdel; /* Disable use of hardware timestamps for qdelay */
	bool	vfio_directpath; /* enable new directpath using vfio */
	bool	no_directpath_active_rss; /* vfio directpath: keep all qs active */
};

extern struct iokernel_cfg cfg;
extern uint32_t nr_vfio_prealloc;


/*
 * Constant limits
 */

#define IOKERNEL_MAX_PROC		2048
#define IOKERNEL_NUM_MBUFS		(8192 * 16)
#define IOKERNEL_NUM_COMPLETIONS	32767
#define IOKERNEL_OVERFLOW_BATCH_DRAIN	64
#define IOKERNEL_TX_BURST_SIZE		64
#define IOKERNEL_CMD_BURST_SIZE		64
#define IOKERNEL_RX_BURST_SIZE		64
#define IOKERNEL_CONTROL_BURST_SIZE	4
#define IOKERNEL_POLL_INTERVAL		10


/*
 * Process Support
 */

struct proc;

struct hwq {
	bool			enabled;
	void			*descriptor_table;
	uint32_t		*consumer_idx;
	uint32_t		descriptor_log_size;
	uint32_t		nr_descriptors;
	uint32_t		parity_byte_offset;
	uint32_t		parity_bit_mask;
	uint32_t		hwq_type;
	uint32_t		last_tail;
	uint32_t		last_head;
	uint64_t		busy_since;
};


struct thread_metrics {
	uint32_t		uthread_elapsed_us;
	uint32_t		rcu_gen;
	bool			work_pending;
};

struct thread {
	bool			active;
	pid_t			tid;
	uint64_t		next_poll_tsc;
	struct proc		*p;
	struct lrpc_chan_out	rxq;
	struct lrpc_chan_in	txpktq;
	struct lrpc_chan_in	txcmdq;
	struct q_ptrs		*q_ptrs;
	uint32_t		last_rq_head;
	uint32_t		last_rq_tail;
	uint32_t		last_rxq_head;
	uint32_t		last_rxq_tail;
	uint64_t		rxq_busy_since;
	unsigned int		core;
	unsigned int		at_idx;
	unsigned int		ts_idx;
	uint32_t		last_yield_rcu_gen;
	uint64_t		wake_gen;
	uint64_t		change_tsc;

	struct hwq		directpath_hwq;
	struct hwq		storage_hwq;
	struct list_node	idle_link;

	/* useful metrics for scheduling policies */
	struct thread_metrics	metrics;
};

static inline void thread_enable_sched_poll(struct thread *th)
{
	th->next_poll_tsc = 0;
}

static inline void thread_set_next_poll(struct thread *th, uint64_t tsc)
{
	th->next_poll_tsc = tsc;
}

static inline void thread_disable_sched_poll(struct thread *th)
{
	th->next_poll_tsc = UINT64_MAX;
}

static inline bool thread_sched_should_poll(struct thread *th, uint64_t now)
{
	return th->next_poll_tsc <= now;
}

static inline bool hwq_busy(struct hwq *h, uint32_t cq_idx)
{
	uint32_t idx, parity, hd_parity;
	unsigned char *addr;

	idx = cq_idx & (h->nr_descriptors - 1);
	parity = !!(cq_idx & h->nr_descriptors);
	addr = h->descriptor_table + (idx << h->descriptor_log_size) + h->parity_byte_offset;
	hd_parity = !!(ACCESS_ONCE(*addr) & h->parity_bit_mask);

	return parity == hd_parity;
}

struct proc {
	pid_t			pid;
	struct shm_region	region;

	struct ref		ref;

	unsigned int		has_directpath:1;
	unsigned int		has_vfio_directpath:1;
	unsigned int		vfio_directpath_rmp:1;
	unsigned int		kill:1;       /* the proc is being torn down */
	unsigned int		attach_fail:1;
	unsigned int		removed:1;
	unsigned int		started:1;
	struct runtime_info	*runtime_info;
	unsigned long		policy_data;
	unsigned long		directpath_data;
	float			load;
	uint64_t		next_poll_tsc;

	/* scheduler data */
	struct sched_spec	sched_cfg;

	/* the flow steering table */
	unsigned int		flow_tbl[NCPU];

	/* runtime threads */
	unsigned int		thread_count;
	unsigned int		active_thread_count;
	struct thread		threads[NCPU];
	struct thread		*active_threads[NCPU];
	struct list_head	idle_threads;
	unsigned int		next_thread_rr; // for spraying join requests/overflow completions

	/* network data */
	uint32_t		ip_addr;

	/* Overfloq queue for completion data */
	size_t max_overflows;
	size_t nr_overflows;
	unsigned long *overflow_queue;
	struct list_node overflow_link;

	int				control_fd;

	/* table of physical addresses for shared memory */
	physaddr_t		page_paddrs[];
};

static inline void proc_enable_sched_poll(struct proc *p)
{
	p->next_poll_tsc = 0;
}

static inline void proc_set_next_poll(struct proc *p, uint64_t tsc)
{
	p->next_poll_tsc = tsc;
}

static inline void proc_disable_sched_poll(struct proc *p)
{
	p->next_poll_tsc = UINT64_MAX;
}


static inline bool proc_sched_should_poll(struct proc *p, uint64_t now)
{
	return p->next_poll_tsc <= now;
}

extern void proc_release(struct ref *r);

/**
 * proc_get - increments the proc reference count
 * @p: the proc to reference count
 *
 * Returns @p.
 */
static inline struct proc *proc_get(struct proc *p)
{
	ref_get(&p->ref);
	return p;
}

/**
 * proc_put - decrements the proc reference count, freeing if zero
 * @p: the proc to unreference count
 */
static inline void proc_put(struct proc *p)
{
	ref_put(&p->ref, proc_release);
}

/* the number of active threads to be polled (across all procs) */
extern unsigned int nrts;
/* an array of active threads to be polled (across all procs) */
extern struct thread *ts[NCPU];

/**
 * poll_thread - adds a thread to the queue polling array
 * @th: the thread to poll
 *
 * Can be called more than once.
 */
static inline void poll_thread(struct thread *th)
{
	if (th->ts_idx != UINT_MAX)
		return;
	proc_get(th->p);
	ts[nrts] = th;
	th->ts_idx = nrts++;
}

/**
 * unpoll_thread - removes a thread from the queue polling array
 * @th: the thread to no longer poll
 */
static inline void unpoll_thread(struct thread *th)
{
	if (th->ts_idx == UINT_MAX)
		return;
	ts[th->ts_idx] = ts[--nrts];
	ts[th->ts_idx]->ts_idx = th->ts_idx;
	th->ts_idx = UINT_MAX;
	proc_put(th->p);
}

/*
 * Communication between control plane and data-plane in the I/O kernel
 */
#define CONTROL_DATAPLANE_QUEUE_SIZE	IOKERNEL_MAX_PROC
struct lrpc_params {
	struct lrpc_msg *buffer;
	uint32_t *wb;
};
extern struct lrpc_params lrpc_control_to_data_params;
extern struct lrpc_params lrpc_data_to_control_params;
extern int data_to_control_efd;

/*
 * Commands from control plane to dataplane.
 */
enum {
	DATAPLANE_ADD_CLIENT,		/* points to a struct proc */
	DATAPLANE_REMOVE_CLIENT,	/* points to a struct proc */
	DATAPLANE_NR,			/* number of commands */
};

/*
 * Commands from dataplane to control plane.
 */
enum {
	CONTROL_PLANE_REMOVE_CLIENT,	/* points to a struct proc */
	CONTROL_PLANE_NR,		/* number of commands */
};

/*
 * Dataplane state
 */
struct dataplane {
	uint8_t			port;
	struct rte_mempool	*rx_mbuf_pool;
	struct shm_region	ingress_mbuf_region;

	struct proc		*clients[IOKERNEL_MAX_PROC];
	int			nr_clients;
	struct rte_hash		*ip_to_proc;
	struct rte_device	*device;
};

extern struct dataplane dp;
extern struct iokernel_info *iok_info;


/*
 * Logical cores assigned to linux and the control and dataplane threads
 */
struct core_assignments {
	uint8_t linux_core;
	uint8_t ctrl_core;
	uint8_t dp_core;
};

extern struct core_assignments core_assign;


/*
 * Stats collected in the iokernel
 */
enum {
	RX_UNREGISTERED_MAC = 0,
	RX_UNICAST_FAIL,
	RX_BROADCAST_FAIL,
	RX_UNHANDLED,

	PARKED_THREAD_BUSY_WAKE,
	PARK_FAST_REWAKE,

	TX_COMPLETION_OVERFLOW,
	TX_COMPLETION_FAIL,

	RX_PULLED,
	COMMANDS_PULLED,
	COMPLETION_DRAINED,
	COMPLETION_ENQUEUED,
	LOOPS,
	TX_PULLED,
	TX_BACKPRESSURE,

	SCHED_RUN,
	PREEMPT,

	RX_REFILL,

	DIRECTPATH_EVENTS,

	NR_STATS,

};

extern uint64_t stats[NR_STATS];

#ifdef STATS
#define STAT_INC(stat_name, amt) do { stats[stat_name] += amt; } while (0);
#else
#define STAT_INC(stat_name, amt) ;
#endif

/*
 * RXQ command steering
 */

extern bool rx_send_to_runtime(struct proc *p, uint32_t hash, uint64_t cmd,
			       unsigned long payload);

/*
 * Initialization
 */

extern int ksched_init(void);
extern int sched_init(void);
extern int simple_init(void);
extern int numa_init(void);
extern int ias_init(void);
extern int control_init(void);
extern int dpdk_init(void);
extern int rx_init(void);
extern int tx_init(void);
extern int dp_clients_init(void);
extern int dpdk_late_init(void);
extern int hw_timestamp_init(void);
extern int stats_init(void);

extern char *nic_pci_addr_str;
extern struct pci_addr nic_pci_addr;
extern bool allowed_cores_supplied;
extern DEFINE_BITMAP(input_allowed_cores, NCPU);
extern char **dpdk_argv;
extern int dpdk_argc;
extern int managed_numa_node;
extern pthread_barrier_t init_barrier;

/*
 * dataplane RX/TX functions
 */
extern bool rx_burst(void);
extern bool tx_burst(void);
extern bool tx_send_completion(void *obj);
extern bool tx_drain_completions(void);

/*
 * other dataplane functions
 */
extern void dp_clients_rx_control_lrpcs(void);
extern bool commands_rx(void);
extern void dpdk_print_eth_stats(void);

/*
 * vfio directpath functions
 */
extern int directpath_init(void);
#ifdef DIRECTPATH
struct directpath_spec;

extern int directpath_get_clock(unsigned int *frequency_khz, void **core_clock);

/* may not be called from the dataplane thread (potentially blocking) */
extern int alloc_directpath_ctx(struct proc *p, bool use_rmp,
                                struct directpath_spec *spec_out,
                                int *memfd_out, int *barfd_out);
extern void release_directpath_ctx(struct proc *p);
extern void directpath_preallocate(bool use_rmp, unsigned int nrqs, unsigned int cnt);

/* must be called from the dataplane thread */
extern bool directpath_poll(void);
extern bool directpath_poll_proc(struct proc *p, uint64_t *delay_cycles, uint64_t cur_tsc);
extern void directpath_notify_waking(struct proc *p, struct thread *th);
extern void directpath_dataplane_notify_kill(struct proc *p);
extern void directpath_dataplane_attach(struct proc *p);
#else

static inline int alloc_directpath_ctx(struct proc *p, ...)
{
	return -1;
}

static inline void release_directpath_ctx(struct proc *p) {}
static inline void directpath_preallocate(bool use_rmp, unsigned int nrqs, unsigned int cnt) {}
static inline void directpath_dataplane_notify_kill(struct proc *p) {}
static inline void directpath_dataplane_attach(struct proc *p) {}

static inline bool directpath_poll(void)
{
	return false;
}

static inline int directpath_get_clock(unsigned int *f, ...)
{
	return -1;
}

static inline void directpath_poll_thread_delay(struct proc *p, ...) {}
static inline bool directpath_poll_proc(struct proc *p, uint64_t *delay_cycles, uint64_t cur_tsc) { return true; }
static inline void directpath_notify_waking(struct proc *p, struct thread *th) {}

#endif
