/*
 * sched.c - low-level scheduler routines (e.g. adding and preempting cores)
 */

#include <stdio.h>

#include <base/stddef.h>
#include <base/assert.h>
#include <base/time.h>
#include <base/bitmap.h>
#include <base/log.h>
#include <base/cpu.h>

#include <iokernel/directpath.h>
#include <iokernel/queue.h>

#include "defs.h"
#include "sched.h"
#include "ksched.h"
#include "hw_timestamp.h"

#define PROC_TIMER_WHEEL_THRESH_US 100

/* a bitmap of cores available to be allocated by the scheduler */
DEFINE_BITMAP(sched_allowed_cores, NCPU);

/* maps each cpu number to the cpu number of its hyperthread buddy */
unsigned int sched_siblings[NCPU];

/* core assignments */
unsigned int sched_dp_core;	/* used for the iokernel's dataplane */
unsigned int sched_ctrl_core;	/* used for the iokernel's controlplane */

/* keeps track of which cores are in each NUMA socket */
struct socket socket_state[NNUMA];
int managed_numa_node;

/* arrays of core numbers for fast polling */
unsigned int sched_cores_tbl[NCPU];
int sched_cores_nr;

static int nr_guaranteed;

LIST_HEAD(poll_list);

struct core_state {
	struct thread	*last_th;     /* recently run thread, waiting for preemption to complete */
	struct thread	*pending_th;  /* a thread waiting run */
	struct thread	*cur_th;      /* the currently running thread */
	unsigned int	idle:1;	      /* is the core idle? */
	unsigned int	pending:1;    /* the next run is waiting */
	unsigned int	wait:1;       /* waiting for run to finish */
};

/* a per-CPU state table to manage scheduling operations */
static struct core_state state[NCPU];
/* policy-specific operations (TODO: should be made configurable) */
const struct sched_ops *sched_ops;

/* current hardware timestamp */
static uint64_t cur_tsc;

static void proc_disable_sched_poll(struct proc *p)
{
	// proc already disabled
	if (p->next_poll_tsc == UINT64_MAX)
		return;

	// otherwise delete it from the poll list or timer wheel
	list_del(&p->link);
	p->timer_pos_us = 0;
	p->next_poll_tsc = UINT64_MAX;
}

static void proc_set_next_poll(struct proc *p, uint64_t tsc)
{
	assert(proc_is_sched_polled(p));

	tsc = MAX(tsc, cur_tsc);
	p->next_poll_tsc = tsc;

	if (tsc - cur_tsc > PROC_TIMER_WHEEL_THRESH_US * cycles_per_us) {
		list_del_from(&poll_list, &p->link);
		if (tsc != UINT64_MAX)
			proc_timer_add(p, tsc);
	}
}

static void proc_enable_sched_poll(struct proc *p)
{
	if (proc_on_timer_wheel(p)) {
		p->timer_pos_us = 0;
		list_del(&p->link);
	} else if (p->next_poll_tsc != UINT64_MAX) {
		// already polled
		return;
	}

	proc_enable_sched_poll_nocheck(p);
}



/**
 * sched_steer_flows - redirects flows to active kthreads
 * @p: the proc for which to reallocate flows
 *
 * This main goal of this function is to ensure a subset of the flow space is
 * consistently allocated to the same kthreads, so that port hacking can be used
 * to maintain egress cache affinity.
 *
 * For ingress affinity, the benefits of smarter algorithms appear to be
 * insignificant because, in general, cores tend to be reallocated slowly enough
 * (~ every 100us is acceptable) for the overhead of changes in flow mappings to
 * be amortized. Therefore, our goal for the rest of the flow space is merely to
 * ensure it is distributed evenly.
 */
static void sched_steer_flows(struct proc *p)
{
	struct thread *th;
	int i, j = 0;

	/* don't do anything if zero threads are active */
	if (p->active_thread_count == 0)
		return;

	/* clear the flow table */
	memset(p->flow_tbl, 0xFF, sizeof(*p->flow_tbl) * p->thread_count);

	/* first assign the identity rxq to each active thread */
	for (i = 0; i < p->active_thread_count; i++) {
		ptrdiff_t idx = p->active_threads[i] - p->threads;
		p->flow_tbl[idx] = idx;
	}

	/* then assign the rest round-robin */
	for (i = 0; i < p->thread_count; i++) {
		if (p->flow_tbl[i] != UINT16_MAX)
			continue;
		th = p->active_threads[j++ % p->active_thread_count];
		p->flow_tbl[i] = th - p->threads;
	}
}

static void sched_enable_kthread(struct proc *p, struct thread *th, unsigned int core)
{
	ACCESS_ONCE(th->q_ptrs->curr_grant_gen) = ++th->wake_gen;
	thread_enable_sched_poll(th);
	proc_enable_sched_poll(p);
	th->change_tsc = cur_tsc;
	th->active = true;
	th->core = core;
	p->last_core[th - p->threads] = NCPU;
	list_del_from(&p->idle_threads, &th->idle_link);
	if (p->has_vfio_directpath) {
		p->active_thread_count++;
		directpath_notify_waking(p, th);
	} else {
		th->at_idx = p->active_thread_count;
		p->active_threads[p->active_thread_count++] = th;
		poll_thread(th);
		if (!p->has_directpath)
			sched_steer_flows(p);
	}

	if (unlikely(!p->started))
		p->started = true;
}

static void sched_disable_kthread(struct thread *th, unsigned int last_core)
{
	struct proc *p = th->p;

	th->active = false;
	th->change_tsc = cur_tsc;
	p->last_core[th - p->threads] = last_core;
	list_add(&p->idle_threads, &th->idle_link);
	if (!p->has_directpath)
		sched_steer_flows(p);
	if (!p->has_vfio_directpath) {
		p->active_threads[th->at_idx] = p->active_threads[--p->active_thread_count];
		p->active_threads[th->at_idx]->at_idx = th->at_idx;
		if (lrpc_empty(&th->txpktq))
			unpoll_thread(th);
	} else {
		p->active_thread_count--;
	}
}

static struct thread *sched_pick_kthread(struct proc *p, uint16_t core)
{
	uint16_t i;

	for (i = 0; i < p->thread_count; i++) {
		if (p->last_core[i] == core || p->last_core[i] == sched_siblings[core])
			return &p->threads[i];
	}

	/* finally pick the least recently used thread (to avoid thrashing) */
	return list_tail(&p->idle_threads, struct thread, idle_link);
}

static int
__sched_run(struct core_state *s, struct thread *th, unsigned int core)
{
	/* if we're still busy with the last run request than stop here */
	if (s->wait) {
		if (s->pending_th) {
			sched_disable_kthread(s->pending_th, UINT16_MAX);
			proc_put(s->pending_th->p);
		}
		s->pending_th = th;
		s->pending = true;
		return 0;
	}

	/* check if we need to interrupt the current core */
	if (!s->idle && s->cur_th != NULL) {
		ACCESS_ONCE(s->cur_th->q_ptrs->cede_gen) = s->cur_th->wake_gen;
		ksched_enqueue_intr(core, KSCHED_INTR_CEDE);
		STAT_INC(PREEMPT, 1);
	}

	/* finally request that the new kthread run on this core */
	ksched_run(core, th ? th->tid : 0);

	s->last_th = s->cur_th;
	s->cur_th = th;
	s->wait = true;
	s->idle = false;
	return 0;
}

/**
 * sched_run_on_core - allocates a core to a process
 * @p: the process to run on the core
 * @core: the core number to run the process on
 *
 * If another process is currently running on @core, it will be preempted.
 *
 * Returns 0 if successful, otherwise fail.
 */
int sched_run_on_core(struct proc *p, unsigned int core)
{
	struct core_state *s = &state[core];
	struct thread *th;

	/* validate inputs --- mostly to catch bugs */
	if (unlikely(list_empty(&p->idle_threads) || core >= NCPU ||
		     !bitmap_test(sched_allowed_cores, core))) {
		WARN();
		return -EINVAL;
	}

	/* select the best kthread to run on this core (if requested) */
	th = sched_pick_kthread(p, core);
	if (unlikely(!th))
		return -ENOENT;
	proc_get(p);
	sched_enable_kthread(p, th, core);

	/* issue the command to run the thread */
	return __sched_run(s, th, core);
}

/**
 * sched_idle_on_core - makes a core go idle
 * @mwait_hint: the model-specific MWAIT idle state hint
 * @core: the core number to run the process on
 *
 * If another process is currently running on @core, it will be preempted.
 *
 * Returns 0 if successful, otherwise fail.
 */
int sched_idle_on_core(uint32_t mwait_hint, unsigned int core)
{
	struct core_state *s = &state[core];

	/* validate inputs --- mostly to catch bugs */
	if (unlikely(core >= NCPU || !bitmap_test(sched_allowed_cores, core))) {
		WARN();
		return -EINVAL;
	}

	/* setup the requested idle state */
	ksched_idle_hint(core, mwait_hint);

	/* issue the command to idle the core */
	return __sched_run(s, NULL, core);
}

/*
 * sched_request_cooperative_cede - request a runtime core cede its allocation
 * sets a flag that a kthread will eventually see to notify. use sched_run() to
 * deliver a preemption signal.
 *
 * @core: the core number to preempt
 *
 * Returns 0 if successful, otherwise fail.
 */
int sched_request_cooperative_cede(unsigned int core)
{
	struct thread *th;

	th = sched_get_thread_on_core(core);
	if (!th)
		return -ENOENT;

	ACCESS_ONCE(th->q_ptrs->park_gen) = th->wake_gen;
	return 0;
}

int sched_cancel_cooperative_cede(struct proc *p, unsigned int core)
{
	struct thread *th;

	th = sched_get_thread_on_core(core);
	if (!th || th->p != p) {
		WARN();
		return -ENOENT;
	}

	ACCESS_ONCE(th->q_ptrs->park_gen) = th->wake_gen - 1;
	return 0;
}

/**
 * sched_yield_on_core - yields the running uthread on the core
 * @core: the core number to preempt
 *
 * Returns 0 if successful, otherwise fail.
 */
int sched_yield_on_core(unsigned int core)
{
	struct thread *th;

	th = sched_get_thread_on_core(core);
	if (!th)
		return -ENOENT;

	/* check to make sure the last yield request finished */
	if (th->last_yield_rcu_gen == th->metrics.rcu_gen)
		return 0;

	/* send the yield signal */
	th->last_yield_rcu_gen = th->metrics.rcu_gen;
	ACCESS_ONCE(th->q_ptrs->yield_rcu_gen) = th->metrics.rcu_gen;
	ksched_enqueue_intr(core, KSCHED_INTR_YIELD);
	return 0;
}

/**
 * sched_get_thread_on_core - retrieves the thread currently on a core
 * @core: the core number to get the thread for
 *
 * Returns a pointer to a thread, or NULL if none is present.
 */
struct thread *sched_get_thread_on_core(unsigned int core)
{
	struct core_state *s = &state[core];
	return s->pending ? s->pending_th : s->cur_th;
}

static uint32_t hwq_find_head(struct hwq *h, uint32_t cur_tail, uint32_t last_head)
{
	uint32_t i = 0;
	uint32_t start_idx = wraps_lt(cur_tail, last_head) ? last_head : cur_tail;
	uint32_t nr_desc = h->nr_descriptors - (start_idx - cur_tail);

	while (i < nr_desc) {
		if (!hwq_busy(h, start_idx + i))
			break;
		i++;
	}

	return i + start_idx;
}

static uint64_t sched_measure_mlx5_delay(struct hwq *h)
{
	uint32_t cur_tail;
	struct mlx5_cqe64 *cqe;

	assert(h->enabled);
	cur_tail = ACCESS_ONCE(*h->consumer_idx);
	if (!hwq_busy(h, cur_tail))
		return 0;

	cur_tail &= h->nr_descriptors - 1;
	cqe = h->descriptor_table + (cur_tail << h->descriptor_log_size);
	return hw_timestamp_delay_us(cqe) * cycles_per_us;
}

static bool sched_queue_has_hw_timestamp(struct hwq *h)
{
	if (!is_hw_timestamp_enabled())
		return false;

	return h->hwq_type == HWQ_MLX5 || h->hwq_type == HWQ_MLX5_QSTEER;
}

static void
sched_measure_hardware_delay(struct thread *th, struct hwq *h,
			     bool update_pointers, bool *has_work,
			     bool *standing_queue, uint64_t *delay_cycles)
{
	uint32_t cur_tail, cur_head, last_head, last_tail;
	uint64_t delay;

	if (!h->enabled)
		return;

	/* fast way to measuring queueing in MLX hardware queues */
	if (sched_queue_has_hw_timestamp(h)) {
		delay = sched_measure_mlx5_delay(h);

		if (!delay)
			return;

		*has_work = true;
		/* quickly approximate standing queue signal */
		*standing_queue |= delay >= IOKERNEL_POLL_INTERVAL * cycles_per_us;
		*delay_cycles = delay;
		return;
	}

	/*
	 * slow path - will use hwq_find_head() to scan the queue
	 * to find the newest element
	 */
	last_head = h->last_head;
	last_tail = h->last_tail;

	cur_tail = ACCESS_ONCE(*h->consumer_idx);
	cur_head = hwq_find_head(h, cur_tail, last_head);

	if (update_pointers) {
		h->last_tail = cur_tail;
		h->last_head = cur_head;
	}

	/* check whether hwq is empty */
	if (cur_head == cur_tail) {
		h->busy_since = UINT64_MAX;
		return;
	}

	/* check whether there was any progress on draining hwq or new packet
	 * has arrived */
	if (cur_tail != last_tail || h->busy_since == UINT64_MAX)
		h->busy_since = cur_tsc;

	*has_work = true;
	*standing_queue |= wraps_lt(cur_tail, last_head);
	*delay_cycles = cur_tsc - h->busy_since;
}

static uint64_t calc_delay_tsc(uint64_t tsc)
{
	return cur_tsc - MIN(tsc, cur_tsc);
}

static void
sched_update_kthread_metrics(struct thread *th, bool work_pending)
{
	uint32_t rcu_gen, uthread_elapsed_tsc;

	rcu_gen = ACCESS_ONCE(th->q_ptrs->rcu_gen);
	if ((rcu_gen & 0x1) == 0) {
		/* in scheduler context, no thread running */
		uthread_elapsed_tsc = 0;
	} else {
		uint64_t tsc = ACCESS_ONCE(th->q_ptrs->run_start_tsc);
		uthread_elapsed_tsc = cur_tsc - MIN(tsc, cur_tsc);
	}

	th->metrics.uthread_elapsed_us = uthread_elapsed_tsc / cycles_per_us;
	th->metrics.rcu_gen = rcu_gen;
	th->metrics.work_pending = work_pending;
	th->metrics.kthread_elapsed_us = (cur_tsc - th->change_tsc) / cycles_per_us;
}

static void
sched_measure_kthread_delay(struct proc *p, struct thread *th, uint64_t *thread_delay,
                            uint64_t *rxq_delay, bool *has_work,
                            bool *standing_queue, uint64_t *next_timer)
{
	uint32_t cur_tail, cur_head, last_head, last_tail;
	uint64_t tmp;

	/* UTHREAD: measure delay */
	last_tail = th->last_rq_tail;
	cur_tail = load_acquire(&th->q_ptrs->rq_tail);
	last_head = th->last_rq_head;
	cur_head = ACCESS_ONCE(th->q_ptrs->rq_head);
	th->last_rq_head = cur_head;
	th->last_rq_tail = cur_tail;

	/* UTHREAD: update old standing queue signal */
	*has_work |= cur_head != cur_tail;
	*standing_queue |= wraps_lt(cur_tail, last_head);

	/* UTHREAD: update new queueing delay signal */
	if (cur_head != cur_tail) {
		tmp = ACCESS_ONCE(th->q_ptrs->oldest_tsc);
		*thread_delay += calc_delay_tsc(tmp);
	}

	/* RXQ: measure delay */
	last_tail = th->last_rxq_tail;
	cur_tail = ACCESS_ONCE(th->q_ptrs->rxq_wb);
	last_head = th->last_rxq_head;
	cur_head = ACCESS_ONCE(th->rxq.send_head);
	th->last_rxq_head = cur_head;
	th->last_rxq_tail = cur_tail;

	/* RXQ: update old standing queue signal */
	*has_work |= cur_head != cur_tail;
	*standing_queue |= wraps_lt(cur_tail, last_head);

	/* RXQ: update new queueing delay signal */
	/* FIXME: this only approximates queueing delay */
	if (cur_head == cur_tail)
		th->rxq_busy_since = UINT64_MAX;
	else if (cur_tail != last_tail || th->rxq_busy_since == UINT64_MAX)
		th->rxq_busy_since = cur_tsc;
	 *thread_delay += calc_delay_tsc(th->rxq_busy_since);

	/* TIMER: measure delay and update signals */
	tmp = *next_timer = ACCESS_ONCE(th->q_ptrs->next_timer_tsc);
	if (tmp <= cur_tsc) {
		*has_work = true;
		*standing_queue |= tmp + IOKERNEL_POLL_INTERVAL * cycles_per_us < cur_tsc;
	}
	*thread_delay += calc_delay_tsc(tmp);

	/*
	 * DIRECTPATH: measure delay and update signals.
	 * ignore the busy signal here.
	 */
	if (p->has_directpath && !p->has_vfio_directpath) {
		if (th->directpath_hwq.hwq_type == HWQ_MLX5_QSTEER) {
			bool a, b;
			sched_measure_hardware_delay(th, &th->directpath_hwq, true, &a, &b, rxq_delay);
		} else {
			tmp = 0;
			sched_measure_hardware_delay(th, &th->directpath_hwq, true, has_work, standing_queue, &tmp);
			*thread_delay += tmp;
		}
	}

	if (p->has_storage) {
		/* STORAGE: measure delay and update signals */
		tmp = 0;
		sched_measure_hardware_delay(th, &th->storage_hwq, true, has_work,
			                         standing_queue, &tmp);
		*thread_delay += tmp;
	}
}

#define EWMA_WEIGHT     0.1f

static void sched_report_metrics(struct proc *p, uint64_t delay)
{
	struct congestion_info *info = &p->runtime_info->congestion;
	float instant_load;

	instant_load = (float)p->active_thread_count;
	p->load = p->load * (1 - EWMA_WEIGHT) + instant_load * EWMA_WEIGHT;
	ACCESS_ONCE(info->load) = p->load;
	ACCESS_ONCE(info->delay_us) = delay;
}

static bool sched_proc_can_unpoll(struct proc *p)
{
	if (unlikely(!p->started))
		return false;

	return !p->has_directpath || p->has_vfio_directpath;
}

static void sched_measure_delay(struct proc *p)
{
	struct delay_info dl;
	struct thread *th;
	uint64_t rxq_delay = 0, consumed_strides = 0, posted_strides, next_poll_tsc;
	unsigned int i;

	if (!proc_sched_should_poll(p, cur_tsc))
		return;

	dl.has_work = false;
	dl.standing_queue = false;
	dl.parked_thread_busy = false;
	dl.max_delay_us = 0;
	dl.min_delay_us = UINT64_MAX;
	dl.avg_delay_us = 0;

	directpath_poll_proc_prefetch(p);

	prefetch(&p->runtime_info->directpath_strides_consumed);

	next_poll_tsc = UINT64_MAX;

	/* detect per-kthread delay */
	for (i = 0; i < p->thread_count; i++) {
		bool busy = false;
		uint64_t delay = 0, next_timer_tsc;
		th = &p->threads[i];


		if (!thread_sched_should_poll(th, cur_tsc)) {
			next_poll_tsc = MIN(next_poll_tsc, th->next_poll_tsc);
			continue;
		}

		void *prefetch1 = directpath_poll_proc_prefetch_th0(p, i);

		sched_measure_kthread_delay(p, th, &delay, &rxq_delay, &busy,
			                        &dl.standing_queue, &next_timer_tsc);

		consumed_strides += ACCESS_ONCE(th->q_ptrs->directpath_strides_consumed);

		directpath_poll_proc_prefetch_th1(prefetch1, ACCESS_ONCE(th->q_ptrs->directpath_rx_tail));

		dl.has_work |= busy;
		dl.parked_thread_busy |= busy && !th->active;
		dl.max_delay_us = MAX(delay, dl.max_delay_us);
		dl.avg_delay_us += delay;

		if (th->active) {
			sched_update_kthread_metrics(th, busy);
			if (delay < dl.min_delay_us) {
				dl.min_delay_us = delay;
				dl.min_delay_core = th->core;
			}
		} else if (!busy && sched_proc_can_unpoll(p)) {
			thread_set_next_poll(th, next_timer_tsc);
			next_poll_tsc = MIN(next_poll_tsc, next_timer_tsc);
		}
	}


	bool directpath_armed = true;
	if (p->has_vfio_directpath) {
		directpath_armed = directpath_poll_proc(p, &rxq_delay, cur_tsc);

		consumed_strides += atomic64_read(&p->runtime_info->directpath_strides_consumed);
		posted_strides = ACCESS_ONCE(p->runtime_info->directpath_strides_posted);
		posted_strides <<= DIRECTPATH_STRIDE_SHIFT;

		if (posted_strides && posted_strides >= consumed_strides &&
		    posted_strides - consumed_strides < DIRECTPATH_STRIDE_REFILL_THRESH_HI) {
			rx_send_to_runtime(p, 0, RX_REFILL_BUFS, 0);
			STAT_INC(RX_REFILL, 1);
			dl.has_work = true;
			dl.parked_thread_busy |= sched_threads_active(p) == 0;
		}
	}

	if (rxq_delay) {
		dl.max_delay_us += rxq_delay;
		dl.avg_delay_us += rxq_delay * sched_threads_active(p);
		dl.min_delay_us += rxq_delay;

		dl.has_work = true;
		dl.standing_queue |= rxq_delay >= IOKERNEL_POLL_INTERVAL * cycles_per_us;
		dl.parked_thread_busy |= sched_threads_active(p) == 0;
	}

	/* don't report parked busy if no threads are active */
	if (cfg.noidlefastwake && sched_threads_active(p) == 0)
		dl.parked_thread_busy = false;

	/* convert the delays to us */
	dl.max_delay_us /= (double)cycles_per_us;
	dl.min_delay_us /= (double)cycles_per_us;
	dl.avg_delay_us /= (double)(cycles_per_us * sched_threads_active(p));

	/* report delay back to runtime */
	sched_report_metrics(p, dl.max_delay_us);

	/* notify the scheduler policy of the current delay */
	if (sched_ops->notify_congested(p, &dl))
		proc_disable_sched_poll(p);
	else if (sched_threads_active(p) == 0 && !dl.has_work &&
	    directpath_armed && sched_proc_can_unpoll(p))
	    proc_set_next_poll(p, next_poll_tsc);
}

/*
 * Checks if there are any queued I/Os for a proc p which has no active
 * kthreads. Attempts to add a core if so.
 */
static void sched_detect_io_for_idle_runtime(struct proc *p)
{
	struct thread *th;
	bool busy = false, standing_queue;
	int i;
	uint64_t delay;

	for (i = 0; i < p->thread_count; i++) {
		th = &p->threads[i];

		sched_measure_hardware_delay(th, &th->directpath_hwq, false, &busy,
			                         &standing_queue, &delay);
		if (busy) {
			sched_add_core(p);
			return;
		}

		/* no need to check storage queues because kthreads with pending
		   storage I/Os don't park */
	}
}

static int sched_try_fast_rewake(struct thread *th)
{
	struct hwq *h;

	if (unlikely(th->p->kill))
		return -EINVAL;

	/* don't rewake if we requeested a cede */
	if (th->q_ptrs->park_gen == th->wake_gen)
		return -EINVAL;

	/*
	 * If the kthread has yielded voluntarily but still has pending I/O
	 * requests in flight, we can just wake it back up directly without
	 * wasting any extra time in the scheduler.
	 */
	if (ACCESS_ONCE(th->rxq.send_head) != lrpc_poll_send_tail(&th->rxq))
		goto rewake;

	h = &th->directpath_hwq;
	if (h->enabled && hwq_busy(h, ACCESS_ONCE(*h->consumer_idx)))
		goto rewake;

	return -EINVAL;

rewake:
	STAT_INC(PARK_FAST_REWAKE, 1);
	ksched_run(th->core, th->tid);
	state[th->core].wait = true;
	return 0;
}

/**
 * sched_poll - advance the scheduler during each poll loop iteration
 */
void sched_poll(void)
{
	static uint64_t last_time;
	DEFINE_BITMAP(idle, NCPU);
	struct core_state *s;
	uint64_t now;
	int i, core, idle_cnt = 0;
	struct proc *p, *p_next;

	/*
	 * slow pass --- runs every IOKERNEL_POLL_INTERVAL
	 */

	cur_tsc = rdtsc();
	now = (cur_tsc - start_tsc) / cycles_per_us;
	if (cur_tsc - last_time >= IOKERNEL_POLL_INTERVAL * cycles_per_us) {

		STAT_INC(SCHED_RUN, 1);

		/* retrieve current network device tick */
		hw_timestamp_update();

		proc_timer_run(now);

		last_time = cur_tsc;
		list_for_each_safe(&poll_list, p, p_next, link) {
			prefetch(p_next);
			sched_measure_delay(p);
		}
	} else if (!cfg.noidlefastwake && !cfg.vfio_directpath) {
		/* check if any idle directpath runtimes have received I/Os */
		for (i = 0; i < dp.nr_clients; i++) {
			p = dp.clients[i];
			if (p->has_vfio_directpath)
				continue;
			if (p->has_directpath && sched_threads_active(p) == 0)
				sched_detect_io_for_idle_runtime(p);
		}
	}

	/*
	 * fast pass --- runs every poll loop
	 */

	bitmap_init(idle, NCPU, false);
	sched_for_each_allowed_core(core, i) {
		s = &state[core];

		/* check if a pending context switch finished */
		if (s->wait && ksched_poll_run_done(core)) {
			if (s->last_th) {
				sched_disable_kthread(s->last_th, core);
				proc_put(s->last_th->p);
				s->last_th = NULL;
			}
			if (s->pending) {
				struct thread *th = s->pending_th;

				s->pending_th = NULL;
				s->pending = false;
				if (s->cur_th)
					ACCESS_ONCE(s->cur_th->q_ptrs->cede_gen) =
						s->cur_th->wake_gen;
				ksched_enqueue_intr(core, KSCHED_INTR_CEDE);
				STAT_INC(PREEMPT, 1);
				ksched_run(core, th ? th->tid : 0);
				s->last_th = s->cur_th;
				s->cur_th = th;
			} else {
				s->wait = false;
			}
		}

		/* check if a core went idle */
		if (!s->wait && !s->idle && ksched_poll_idle(core)) {
			if (s->cur_th) {
				if (!cfg.vfio_directpath && sched_try_fast_rewake(s->cur_th) == 0)
					continue;
				sched_disable_kthread(s->cur_th, core);
				proc_put(s->cur_th->p);
				s->cur_th = NULL;
			}
			s->idle = true;
			bitmap_set(idle, core);
			idle_cnt++;
		}
	}

	/*
	 * final pass --- let the scheduler policy decide how to respond
	 */

	sched_ops->sched_poll(now, idle_cnt, idle);
	ksched_send_intrs();
}

/**
 * sched_add_core - allocates an additional core to a process
 * @p: the process to add a core
 *
 * Returns 0 if successful, otherwise fail.
 */
int sched_add_core(struct proc *p)
{
	if (cfg.noidlefastwake) {
		proc_enable_sched_poll(p);
		return 0;
	}

	return sched_ops->notify_core_needed(p);
}

/**
 * sched_attach_proc - attach a process to the scheduler
 * @p: the process to attach
 *
 * Returns 0 if successful, otherwise fail.
 */
int sched_attach_proc(struct proc *p)
{
	int i, ret;

	if (p->sched_cfg.guaranteed_cores + nr_guaranteed > sched_cores_nr) {
		log_err("guaranteed cores exceeds total core count");
		return -1;
	}

	p->active_thread_count = 0;
	list_head_init(&p->idle_threads);
	for (i = 0; i < p->thread_count; i++) {
		p->last_core[i] = UINT16_MAX;
		p->threads[i].core = UINT16_MAX;
		p->threads[i].active = false;
		list_add(&p->idle_threads, &p->threads[i].idle_link);
	}

	ret = sched_ops->proc_attach(p, &p->sched_cfg);
	if (ret)
		return ret;

	nr_guaranteed += p->sched_cfg.guaranteed_cores;
	proc_enable_sched_poll_nocheck(p);

	return 0;
}

/**
 * sched_detach_proc - detach a process from the scheduler
 * @p: the process to detach
 */
void sched_detach_proc(struct proc *p)
{
	proc_disable_sched_poll(p);
	sched_ops->proc_detach(p);
	nr_guaranteed -= p->sched_cfg.guaranteed_cores;
}

static int sched_scan_node(int node)
{
	struct cpu_info *info;
	int i, sib, nr, ret = 0;

	for (i = 0; i < cpu_count; i++) {
		info = &cpu_info_tbl[i];
		if (info->package != node)
			continue;

		bitmap_set(socket_state[node].cores, i);

		/* TODO: can only support hyperthread pairs */
		nr = bitmap_popcount(info->thread_siblings_mask, NCPU);
		if (nr != 2) {
			if (nr > 2)
				ret = -EINVAL;
			if (nr == 1 && !cfg.noht)  {
				log_err("HT not detected. Please run again with noht option");
				ret = -EINVAL;
			}
		}

		sib = bitmap_find_next_set(info->thread_siblings_mask,
					   NCPU, 0);
		if (sib == i)
			sib = bitmap_find_next_set(info->thread_siblings_mask,
						   NCPU, sib + 1);

		sched_siblings[i] = sib;
		if (i < sib) {
			if (sib != NCPU)
				printf("[%d,%d]", i, sib);
			else
				printf("[%d]", i);
		}
	}

	return ret;
}

/**
 * sched_init - the global initializer for the scheduler
 *
 * Returns 0 if successful, otherwise fail.
 */
int sched_init(void)
{
	int i;
	bool valid = true;

	bitmap_init(sched_allowed_cores, cpu_count, false);

	/*
	 * first pass: scan and log CPUs
	 */

	log_info("sched: CPU configuration...");
	for (i = 0; i < numa_count; i++) {
		printf("\tnode %d: ", i);
		if (sched_scan_node(i) != 0)
			valid = false;
		printf("\n");
		fflush(stdout);
	}
	if (!valid)
		return -EINVAL;

	/*
	 * second pass: determine available CPUs
	 */

	for (i = 0; i < cpu_count; i++) {
		if (cpu_info_tbl[i].package != managed_numa_node && sched_ops != &numa_ops)
			continue;

		if (allowed_cores_supplied &&
		    !bitmap_test(input_allowed_cores, i))
			continue;

		bitmap_set(sched_allowed_cores, i);
	}
	/* check for minimum number of cores required */
	i = bitmap_popcount(sched_allowed_cores, NCPU);
	if (i < 3 + !cfg.noht) {
		log_err("sched: %d is not enough cores\n", i);
		return -EINVAL;
	}

	/*
	 * third pass: reserve cores for iokernel and system
	 */

	sched_ctrl_core = bitmap_find_next_set(sched_allowed_cores, NCPU, 0);
	if (cfg.noht)
		sched_dp_core = bitmap_find_next_set(sched_allowed_cores, NCPU, sched_ctrl_core + 1);
	else
		sched_dp_core = sched_siblings[sched_ctrl_core];
	bitmap_clear(sched_allowed_cores, sched_ctrl_core);
	bitmap_clear(sched_allowed_cores, sched_dp_core);
	log_info("sched: dataplane on %d, control on %d",
		 sched_dp_core, sched_ctrl_core);

	/* check if configuration disables hyperthreads */
	if (cfg.noht) {
		for (i = 0; i < NCPU; i++) {
			if (!bitmap_test(sched_allowed_cores, i))
				continue;

			if (sched_siblings[i] == NCPU)
				continue;

			bitmap_clear(sched_allowed_cores, sched_siblings[i]);
		}
	}

	/* generate polling arrays */
	bitmap_for_each_set(sched_allowed_cores, NCPU, i)
		sched_cores_tbl[sched_cores_nr++] = i;

	return 0;
}
