/*
 * defs.h - shared definitions local to the iokernel
 */

#include <base/stddef.h>
#include <base/bitmap.h>
#include <base/gen.h>
#include <base/lrpc.h>
#include <base/mem.h>
#undef LIST_HEAD /* hack to deal with DPDK being annoying */
#include <base/list.h>
#include <iokernel/control.h>
#include <net/ethernet.h>

#include "mlx.h"
#include "ref.h"
#include "spdk.h"

/* #define STATS 1 */

/*
 * Constant limits
 */
#define IOKERNEL_MAX_PROC		1024
#define IOKERNEL_NUM_MBUFS		(8192 * 16)
#define IOKERNEL_NUM_COMPLETIONS	32767
#define IOKERNEL_OVERFLOW_BATCH_DRAIN	64
#define IOKERNEL_TX_BURST_SIZE		64
#define IOKERNEL_CMD_BURST_SIZE		64
#define IOKERNEL_RX_BURST_SIZE		64
#define IOKERNEL_CONTROL_BURST_SIZE	4


/*
 * Process Support
 */

struct proc;

enum {
	/* the thread is actively running on a core */
	THREAD_STATE_RUNNING,
	/* the thread is parked (not running on a core) */
	THREAD_STATE_PARKED,
	/* the thread is parked and all of its work has been stolen */
	THREAD_STATE_IDLE,
};

struct thread {
	struct proc		*p;
	unsigned int		state;
	unsigned int		reaffinitize:1;
	struct lrpc_chan_out	rxq;
	struct lrpc_chan_in	txpktq;
	struct lrpc_chan_in	txcmdq;
	pid_t			tid;
	int32_t			park_efd;
	struct q_ptrs		*q_ptrs;
	uint32_t		last_rq_head;
	uint32_t		last_rxq_head;
	/* current or most recent core this thread ran on, depending on whether
	 * this thread is parked or not */
	unsigned int		core;
	/* the @ts index (if active) */
	unsigned int		ts_idx;
	/* the proc->active_threads index (if active) */
	unsigned int		at_idx;
	/* list link for when idle */
	struct list_node	idle_link;
};

struct proc {
	pid_t			pid;
	struct shm_region	region;
	bool			removed;
	struct ref		ref;
	unsigned int		kill:1;       /* the proc is being torn down */
	unsigned int		overloaded:1; /* the proc needs more cores */
	unsigned int		bursting:1;   /* the proc is using past resv. */
	unsigned int		launched:1;   /* executing the first time */
	int			*congestion_signal;

	/* intrusive list links */
	struct list_node	overloaded_link;
	struct list_node	bursting_link;

	/* scheduler data */
	struct sched_spec	sched_cfg;

	/* the flow steering table */
	unsigned int		flow_tbl[NCPU];

	/* runtime threads */
	unsigned int		thread_count;
	unsigned int		active_thread_count;
	struct thread		threads[NCPU];
	struct thread		*active_threads[NCPU];
	DEFINE_BITMAP(available_threads, NCPU);
	struct list_head	idle_threads;
	unsigned int		inflight_preempts;
	unsigned int		next_thread_rr; // for spraying join requests/overflow completions

	/* network data */
	struct eth_addr		mac;

	/* Unique identifier -- never recycled across runtimes*/
	uintptr_t		uniqid;
#ifdef MLX
	uint32_t lkey;
	void *mr;
#endif

	/* Overfloq queue for completion data */
	size_t max_overflows;
	size_t nr_overflows;
	unsigned long *overflow_queue;

	unsigned int nvmeq_cnt;
	struct {
		void* last_cpl;
		bool last_pending;

		struct spdk_nvme_cpl	*cpl_ref;
		uint16_t		*nvme_io_cq_head;
		uint8_t			*nvme_io_phase;
	} nvmeq[NCPU];

	/* table of physical addresses for shared memory */
	physaddr_t		page_paddrs[];
};

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
	if (th->ts_idx != -1)
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
	if (th->ts_idx == -1)
		return;
	ts[th->ts_idx] = ts[--nrts];
	ts[th->ts_idx]->ts_idx = th->ts_idx;
	th->ts_idx = -1;
	proc_put(th->p);
}

/*
 * Communication between control plane and data-plane in the I/O kernel
 */
#define CONTROL_DATAPLANE_QUEUE_SIZE	128
struct lrpc_params {
	struct lrpc_msg *buffer;
	uint32_t *wb;
};
extern struct lrpc_params lrpc_control_to_data_params;
extern struct lrpc_params lrpc_data_to_control_params;

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

	struct proc		*clients[IOKERNEL_MAX_PROC];
	int			nr_clients;
	struct rte_hash		*mac_to_proc;
};

extern struct dataplane dp;

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
	RX_JOIN_FAIL,

	TX_COMPLETION_OVERFLOW,
	TX_COMPLETION_FAIL,

	RX_PULLED,
	COMMANDS_PULLED,
	COMPLETION_DRAINED,
	COMPLETION_ENQUEUED,
	BATCH_TOTAL,
	TX_PULLED,
	TX_BACKPRESSURE,

	RQ_GRANT,
	RX_GRANT,

	ADJUSTS,

	NR_STATS,

};

extern uint64_t stats[NR_STATS];
extern void print_stats(void);

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

extern int cores_init(void);
extern int control_init(void);
extern int dpdk_init();
extern int rx_init();
extern int tx_init();
extern int dp_clients_init();
extern int dpdk_late_init();

/*
 * dataplane RX/TX functions
 */
extern bool rx_burst();
extern bool tx_burst();
extern bool tx_send_completion(void *obj);
extern bool tx_drain_completions();

/*
 * other dataplane functions
 */
extern void dp_clients_rx_control_lrpcs();
extern bool commands_rx();
extern void dpdk_print_eth_stats();

/*
 * functions for manipulating core assignments
 */
extern void cores_init_proc(struct proc *p);
extern void cores_free_proc(struct proc *p);
extern int cores_pin_thread(pid_t tid, int core);
extern bool cores_park_kthread(struct thread *t, bool force);
extern struct thread *cores_add_core(struct proc *p);
extern void cores_adjust_assignments();
extern void proc_set_overloaded(struct proc *p);
extern unsigned int get_nr_avail_cores(void);
extern unsigned int get_total_cores(void);
