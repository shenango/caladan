/*
 * defs.h - internal runtime definitions
 */

#pragma once

#include <base/stddef.h>
#include <base/list.h>
#include <base/mem.h>
#include <base/tcache.h>
#include <base/gen.h>
#include <base/lrpc.h>
#include <base/thread.h>
#include <base/time.h>
#include <net/ethernet.h>
#include <net/ip.h>
#include <iokernel/control.h>
#include <net/mbufq.h>
#include <runtime/gc.h>
#include <runtime/net.h>
#include <runtime/runtime.h>
#include <runtime/thread.h>
#include <runtime/rcu.h>
#include <runtime/preempt.h>


/*
 * constant limits
 * TODO: make these configurable?
 */

#define RUNTIME_MAX_THREADS		100000
#define RUNTIME_STACK_SIZE		256 * KB
#define RUNTIME_GUARD_SIZE		256 * KB
#define RUNTIME_RQ_SIZE			32
#define RUNTIME_MAX_TIMERS		4096
#define RUNTIME_SCHED_POLL_ITERS	0
#define RUNTIME_SCHED_MIN_POLL_US	2
#define RUNTIME_WATCHDOG_US		50
#define RUNTIME_RX_BATCH_SIZE		32


/*
 * Trap frame support
 */

/*
 * See the "System V Application Binary Interface" for a full explation of
 * calling and argument passing conventions.
 */

struct thread_tf {
	/* argument registers, can be clobbered by callee */
	uint64_t rdi; /* first argument */
	uint64_t rsi;
	uint64_t rdx;
	uint64_t rcx;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;

	/* callee-saved registers */
	uint64_t rbx;
	uint64_t rbp;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;

	/* special-purpose registers */
	uint64_t rax;	/* holds return value */
	uint64_t rip;	/* instruction pointer */
	uint64_t rsp;	/* stack pointer */
};

#define ARG0(tf)        ((tf)->rdi)
#define ARG1(tf)        ((tf)->rsi)
#define ARG2(tf)        ((tf)->rdx)
#define ARG3(tf)        ((tf)->rcx)
#define ARG4(tf)        ((tf)->r8)
#define ARG5(tf)        ((tf)->r9)

/* format of the trap frame set up by uintr_asm_entry */
struct uintr_frame {
	struct thread_tf general_regs;
	unsigned long pad;
	unsigned long uirrv;
	unsigned long rip;
	unsigned long rflags;
	unsigned long rsp;
};

/*
 * Thread support
 */

struct stack;

struct thread {
	struct thread_tf	tf;
	struct list_node	link;
	struct stack		*stack;
	unsigned int		main_thread:1;
	unsigned int		thread_ready;
	unsigned int		thread_running;
	unsigned int		last_cpu;
	uint64_t		run_start_tsc;
	uint64_t		ready_tsc;
	uint64_t		tlsvar;
#ifdef GC
	struct list_node	gc_link;
	unsigned int		onk;
#endif
};

typedef void (*runtime_fn_t)(void);

/* assembly helper routines from switch.S */
extern void __jmp_thread(struct thread_tf *tf) __noreturn;
extern void __jmp_thread_direct(struct thread_tf *oldtf,
				struct thread_tf *newtf,
				unsigned int *thread_running);
extern void __jmp_runtime(struct thread_tf *tf, runtime_fn_t fn,
			  void *stack);
extern void __jmp_runtime_nosave(runtime_fn_t fn, void *stack) __noreturn;

/* uintr assembly routine */
extern void uintr_asm_entry();

/*
 * Stack support
 */

#define STACK_PTR_SIZE	(RUNTIME_STACK_SIZE / sizeof(uintptr_t))
#define GUARD_PTR_SIZE	(RUNTIME_GUARD_SIZE / sizeof(uintptr_t))

struct stack {
	uintptr_t	guard[GUARD_PTR_SIZE]; /* unreadable and unwritable */
	uintptr_t       usable[STACK_PTR_SIZE];
};

DECLARE_PERTHREAD(struct tcache_perthread, stack_pt);

/**
 * stack_alloc - allocates a stack
 *
 * Stack allocation is extremely cheap, think less than taking a lock.
 *
 * Returns an unitialized stack.
 */
static inline struct stack *stack_alloc(void)
{
	void *s = tcache_alloc(perthread_ptr(stack_pt));
	if (unlikely(!s))
		return NULL;
	return container_of(s, struct stack, usable);
}

/**
 * stack_free - frees a stack
 * @s: the stack to free
 */
static inline void stack_free(struct stack *s)
{
	tcache_free(perthread_ptr(stack_pt), (void *)s->usable);
}

#define RSP_ALIGNMENT	16

static inline void assert_rsp_aligned(uint64_t rsp)
{
	/*
	 * The stack must be 16-byte aligned at process entry according to
	 * the System V Application Binary Interface (section 3.4.1).
	 *
	 * The callee assumes a return address has been pushed on the aligned
	 * stack by CALL, so we look for an 8 byte offset.
	 */
	assert(rsp % RSP_ALIGNMENT == sizeof(void *));
}

/**
 * stack_init_to_rsp - sets up an exit handler and returns the top of the stack
 * @s: the stack to initialize
 * @exit_fn: exit handler that is called when the top of the call stack returns
 *
 * Returns the top of the stack as a stack pointer.
 */
static inline uint64_t stack_init_to_rsp(struct stack *s, void (*exit_fn)(void))
{
	uint64_t rsp;

	s->usable[STACK_PTR_SIZE - 1] = (uintptr_t)exit_fn;
	rsp = (uint64_t)&s->usable[STACK_PTR_SIZE - 1];
	assert_rsp_aligned(rsp);
	return rsp;
}

/**
 * stack_init_to_rsp_with_buf - sets up an exit handler and returns the top of
 * the stack, reserving space for a buffer above
 * @s: the stack to initialize
 * @buf: a pointer to store the buffer pointer
 * @buf_len: the length of the buffer to reserve
 * @exit_fn: exit handler that is called when the top of the call stack returns
 *
 * Returns the top of the stack as a stack pointer.
 */
static inline uint64_t
stack_init_to_rsp_with_buf(struct stack *s, void **buf, size_t buf_len,
			   void (*exit_fn)(void))
{
	uint64_t rsp, pos = STACK_PTR_SIZE;

	/* reserve the buffer */
	pos -= div_up(buf_len, sizeof(uint64_t));
	pos = align_down(pos, RSP_ALIGNMENT / sizeof(uint64_t));
	*buf = (void *)&s->usable[pos];

	/* setup for usage as stack */
	s->usable[--pos] = (uintptr_t)exit_fn;
	rsp = (uint64_t)&s->usable[pos];
	assert_rsp_aligned(rsp);
	return rsp;
}


/*
 * ioqueues
 */

struct iokernel_control {
	int fd;
	mem_key_t key;
	struct control_hdr *hdr;
	struct thread_spec *threads;
	const struct iokernel_info *iok_info;
	void *tx_buf;
	size_t tx_len;

	void *rx_buf;
	size_t rx_len;
};

extern struct iokernel_control iok;
extern void *iok_shm_alloc(size_t size, size_t alignment, shmptr_t *shm_out);
extern struct runtime_info *runtime_info;

/*
 * Direct hardware queue support
 */

struct hardware_q {
	void		*descriptor_table;
	uint32_t	*consumer_idx;
	uint32_t	*shadow_tail;
	uint32_t	descriptor_log_size;
	uint32_t	nr_descriptors;
	uint32_t	parity_byte_offset;
	uint32_t	parity_bit_mask;
};

static inline bool hardware_q_pending(struct hardware_q *q)
{
	uint32_t tail, idx, parity, hd_parity;
	unsigned char *addr;

	tail = ACCESS_ONCE(*q->consumer_idx);
	idx = tail & (q->nr_descriptors - 1);
	parity = !!(tail & q->nr_descriptors);
	addr = q->descriptor_table +
		     (idx << q->descriptor_log_size) + q->parity_byte_offset;
	hd_parity = !!(ACCESS_ONCE(*addr) & q->parity_bit_mask);

	return parity == hd_parity;
}


/*
 * Storage support
 */

#ifdef DIRECT_STORAGE

extern bool cfg_storage_enabled;
extern unsigned long storage_device_latency_us;

static inline bool storage_enabled(void)
{
	return cfg_storage_enabled;
}

struct storage_q {

	spinlock_t lock;

	unsigned int outstanding_reqs;
	void *spdk_qp_handle;

	struct hardware_q hq;

	unsigned long pad[1];
};

#else

static inline bool storage_enabled(void) {
	return false;
}

#endif

#ifdef GC
extern bool cfg_gc_enabled;
#endif

/*
 * Per-kernel-thread State
 */

/*
 * These are per-kthread stat counters. It's recommended that most counters be
 * monotonically increasing, as that decouples the counters from any particular
 * collection time period. However, it may not be possible to represent all
 * counters this way.
 *
 * Don't use these enums directly. Instead, use the STAT() macro.
 */
enum {
	/* scheduler counters */
	STAT_RESCHEDULES = 0,
	STAT_SCHED_CYCLES,
	STAT_PROGRAM_CYCLES,
	STAT_THREADS_STOLEN,
	STAT_SOFTIRQS_STOLEN,
	STAT_SOFTIRQS_LOCAL,
	STAT_PARKS,
	STAT_PREEMPTIONS,
	STAT_CORE_MIGRATIONS,
	STAT_LOCAL_RUNS,
	STAT_REMOTE_RUNS,
	STAT_LOCAL_WAKES,
	STAT_REMOTE_WAKES,
	STAT_RQ_OVERFLOW,

	/* network stack counters */
	STAT_RX_BYTES,
	STAT_RX_PACKETS,
	STAT_TX_BYTES,
	STAT_TX_PACKETS,
	STAT_DROPS,
	STAT_RX_TCP_IN_ORDER,
	STAT_RX_TCP_OUT_OF_ORDER,
	STAT_RX_TCP_TEXT_CYCLES,
	STAT_TXQ_OVERFLOW,

	/* directpath stats */
	STAT_FLOW_STEERING_CYCLES,
	STAT_RX_HW_DROP,

	/* total number of counters */
	STAT_NR,
};

struct timer_idx {
	uint64_t		deadline_us;
	struct timer_entry	*e;
};

struct kthread {
	/* 1st cache-line */
	spinlock_t		lock;
	uint32_t		kthread_idx;
	uint32_t		rq_head;
	uint32_t		rq_tail;
	struct list_head	rq_overflow;
	struct lrpc_chan_in	rxq;
	pid_t			tid;
	bool			parked;

	/* 2nd cache-line */
	struct q_ptrs		*q_ptrs;
	struct mbufq		txpktq_overflow;
	struct mbufq		txcmdq_overflow;
	unsigned int		rcu_gen;
	unsigned int		curr_cpu;
#ifdef GC
	uint64_t		local_gc_gen;
	unsigned long		pad1[1];
#else
	unsigned long		pad1[2];
#endif

	/* 3rd cache-line */
	struct lrpc_chan_out	txpktq;
	struct lrpc_chan_out	txcmdq;

	/* 4th-7th cache-line */
	thread_t		*rq[RUNTIME_RQ_SIZE];

	/* 8th cache-line */
	spinlock_t		timer_lock;
	unsigned int		timern;
	struct timer_idx	*timers;
	thread_t		*iokernel_softirq;
	thread_t		*timer_softirq;
	thread_t		*storage_softirq;
	bool			iokernel_busy;
	bool			timer_busy;
	bool			storage_busy;
	bool			pad2[5 + 8];
	uint64_t		last_softirq_tsc;

#ifdef DIRECT_STORAGE
	/* 9th cache-line, storage nvme queues */
	struct storage_q	storage_q;
#endif

	/* 10th cache-line, statistics counters */
	uint64_t		stats[STAT_NR];
};

/* compile-time verification of cache-line alignment */
BUILD_ASSERT(offsetof(struct kthread, lock) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(offsetof(struct kthread, q_ptrs) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(offsetof(struct kthread, txpktq) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(offsetof(struct kthread, rq) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(offsetof(struct kthread, timer_lock) % CACHE_LINE_SIZE == 0);
#ifdef DIRECT_STORAGE
BUILD_ASSERT(offsetof(struct kthread, storage_q) % CACHE_LINE_SIZE == 0);
#endif
BUILD_ASSERT(offsetof(struct kthread, stats) % CACHE_LINE_SIZE == 0);

DECLARE_PERTHREAD(struct kthread *, mykthread);

/**
 * myk - returns the per-kernel-thread data
 */
static inline struct kthread *myk(void)
{
	return perthread_read(mykthread);
}

/**
 * getk - returns the per-kernel-thread data and disables preemption
 *
 * WARNING: If you're using myk() instead of getk(), that's a bug if preemption
 * is enabled. The local kthread can change at anytime.
 */
static __always_inline __nofp struct kthread *getk(void)
{
	preempt_disable();
	return perthread_read(mykthread);
}

/**
 * putk - reenables preemption after calling getk()
 */
static __always_inline __nofp void putk(void)
{
	preempt_enable();
}

/* preempt_cede_needed - check if kthread should cede */
static __always_inline __nofp bool preempt_cede_needed(struct kthread *k)
{
	return k->q_ptrs->curr_grant_gen ==
	       ACCESS_ONCE(k->q_ptrs->cede_gen);
}

/* preempt_yield_needed - check if current uthread should yield */
static __always_inline __nofp bool preempt_yield_needed(struct kthread *k)
{
        return ACCESS_ONCE(k->q_ptrs->yield_rcu_gen) == k->rcu_gen;
}

/* preempt_park_needed - check if kthread should park itself */
static __always_inline __nofp bool preempt_park_needed(struct kthread *k)
{
	return k->q_ptrs->curr_grant_gen ==
	       ACCESS_ONCE(k->q_ptrs->park_gen);
}

#ifdef DIRECT_STORAGE
static inline bool storage_available_completions(struct kthread *k)
{
	return cfg_storage_enabled && hardware_q_pending(&k->storage_q.hq);
}

static inline bool storage_pending_completions(struct kthread *k)
{
	return cfg_storage_enabled && k->storage_q.outstanding_reqs > 0 &&
	       storage_device_latency_us <= 10;
}

#else

static inline bool storage_available_completions(struct kthread *k)
{
	return false;
}

static inline bool storage_pending_completions(struct kthread *k)
{
	return false;
}

#endif



DECLARE_SPINLOCK(klock);
extern unsigned int spinks;
extern unsigned int maxks;
extern unsigned int guaranteedks;
extern struct kthread *ks[NCPU];
extern bool cfg_prio_is_lc;
extern unsigned int cfg_request_hardware_queues;
extern uint64_t cfg_ht_punish_us;
extern uint64_t cfg_qdelay_us;
extern uint64_t cfg_quantum_us;

extern void kthread_park(void);
extern void kthread_park_now(void);
extern void kthread_wait_to_attach(void);

extern int ksched_fd;

struct cpu_record {
	struct kthread *recent_kthread;
	unsigned long sibling_core;
	unsigned long pad[6];
};

BUILD_ASSERT(sizeof(struct cpu_record) == CACHE_LINE_SIZE);

extern struct cpu_record cpu_map[NCPU];
extern int preferred_socket;

/**
 * STAT - gets a stat counter
 *
 * e.g. STAT(DROPS)++;
 *
 * Deliberately could race with preemption.
 */
#define STAT(counter)  \
	((perthread_read_stable(mykthread))->stats[STAT_ ## counter])


/*
 * Softirq support
 */

extern bool disable_watchdog;
extern bool softirq_pending(struct kthread *k, uint64_t now_tsc);
extern bool softirq_run_locked(struct kthread *k);
extern bool softirq_run(void);


/*
 * Network stack
 */

struct net_cfg {
	struct shm_region	tx_region;
	struct shm_region	rx_region;
	uint32_t		addr;
	uint32_t		netmask;
	uint32_t		gateway;
	struct eth_addr		mac;
	uint8_t			pad[14];
};

BUILD_ASSERT(sizeof(struct net_cfg) == CACHE_LINE_SIZE);

extern struct net_cfg netcfg;

struct cfg_arp_static_entry {
	uint32_t ip;
	struct eth_addr addr;
};
extern size_t arp_static_count;
extern struct cfg_arp_static_entry *static_entries;

extern void net_rx_softirq(struct rx_net_hdr **hdrs, unsigned int nr);
extern void net_rx_softirq_direct(struct mbuf **ms, unsigned int nr);

extern int __noinline net_tx_drain_overflow(void);

struct trans_entry;
struct net_driver_ops {
	bool (*rx_poll)(unsigned int q_index);
	bool (*rx_poll_locked)(unsigned int q_index);
	int (*tx_single)(struct mbuf *m);
	int (*steer_flows)(unsigned int *new_fg_assignment);
	int (*register_flow)(unsigned int affininty, struct trans_entry *e, void **handle_out);
	int (*deregister_flow)(struct trans_entry *e, void *handle);
	uint32_t (*get_flow_affinity)(uint8_t ipproto, uint16_t local_port, struct netaddr remote);
	void (*trigger_rx_refill)(void);
};

extern struct net_driver_ops net_ops;

#ifdef DIRECTPATH

extern int directpath_parse_arg(const char *name, const char *val);
extern int cfg_directpath_mode;
extern bool cfg_directpath_strided;

enum {
	DIRECTPATH_MODE_DISABLED = 0,
	DIRECTPATH_MODE_ALLOW_ANY,
	DIRECTPATH_MODE_FLOW_STEERING,
	DIRECTPATH_MODE_QUEUE_STEERING,
	DIRECTPATH_MODE_EXTERNAL,
};

extern bool cfg_transparent_hugepages_enabled;

static inline bool is_directpath_strided(void)
{
	return cfg_directpath_strided;
}

static inline bool cfg_directpath_enabled(void)
{
	return cfg_directpath_mode != DIRECTPATH_MODE_DISABLED;
}

static inline bool cfg_directpath_external(void)
{
	return cfg_directpath_mode == DIRECTPATH_MODE_EXTERNAL;
}

static inline bool rx_poll(struct kthread *k)
{
	// Note: the caller must not hold the kthread-local lock
	return net_ops.rx_poll && net_ops.rx_poll(k->kthread_idx);
}

static inline bool rx_poll_locked(struct kthread *k)
{
	assert(spin_lock_held(&myk()->lock));
	return net_ops.rx_poll_locked && net_ops.rx_poll_locked(k->kthread_idx);
}

extern size_t directpath_rx_buf_pool_sz(unsigned int nrqs);

#else

static inline bool is_directpath_strided(void)
{
	return false;
}

static inline bool cfg_directpath_enabled(void)
{
	return false;
}

static inline bool cfg_directpath_external(void)
{
	return false;
}

static inline bool rx_poll(struct kthread *k)
{
	return false;
}

static inline bool rx_poll_locked(struct kthread *k)
{
	return false;
}

static inline size_t directpath_rx_buf_pool_sz(unsigned int nrqs)
{
	return 0;
}

#endif

extern unsigned int eth_mtu;

/**
 * net_get_mtu - gets the ethernet MTU (maximum transmission unit)
 */
static inline unsigned int net_get_mtu(void)
{
	return eth_mtu;
}


/*
 * Runtime configuration infrastructure
 */

typedef int (*cfg_fn_t)(const char *name, const char *val);
struct cfg_handler {
	const char			*name;
	cfg_fn_t			fn;
	bool				required;
	struct list_node		link;
};

#define REGISTER_CFG(c)					\
 __attribute__((constructor))					\
void register_cfg_init_##c (void)				\
{								\
	extern void cfg_register(struct cfg_handler *h);	\
	cfg_register(&c);					\
}


/*
 * Init
 */

/* per-thread initialization */
extern int kthread_init_thread(void);
extern int ioqueues_init_thread(void);
extern int stack_init_thread(void);
extern int timer_init_thread(void);
extern int sched_init_thread(void);
extern int stat_init_thread(void);
extern int net_init_thread(void);
extern int smalloc_init_thread(void);
extern int storage_init_thread(void);
extern int directpath_init_thread(void);
extern int preempt_init_thread(void);

/* global initialization */
extern int kthread_init(void);
extern int ioqueues_init(void);
extern int stack_init(void);
extern int sched_init(void);
extern int preempt_init(void);
extern int net_init(void);
extern int udp_init(void);
extern int arp_init(void);
extern int trans_init(void);
extern int smalloc_init(void);
extern int storage_init(void);
extern int directpath_init(void);
#ifdef GC
extern int gc_init(void);
#endif

/* late initialization */
extern int ioqueues_register_iokernel(void);
extern int arp_init_late(void);
extern int stat_init_late(void);
extern int tcp_init_late(void);
extern int rcu_init_late(void);
extern int directpath_init_late(void);

extern int ioqueues_init_early(void);
extern int net_init_mempool(void);
extern int net_init_mempool_threads(void);

struct directpath_spec;
extern int mlx5_init_ext_late(struct directpath_spec *spec, int bar_fd, int mem_fd);

/* configuration loading */
extern int cfg_load(const char *path);

/* internal runtime scheduling functions */
extern void sched_start(void) __noreturn;
extern int thread_spawn_main(thread_fn_t fn, void *arg);
extern void thread_cede(void);
extern void thread_ready_locked(thread_t *th);
extern void thread_ready_head_locked(thread_t *th);
extern void join_kthread(struct kthread *k);
