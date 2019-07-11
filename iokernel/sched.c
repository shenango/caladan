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

#include "defs.h"
#include "sched.h"
#include "ksched.h"

/* a bitmap of cores available to be allocated by the scheduler */
DEFINE_BITMAP(sched_allowed_cores, NCPU);

/* maps each cpu number to the cpu number of its hyperthread buddy */
unsigned int sched_siblings[NCPU];

/* core assignments */
unsigned int sched_dp_core;	/* used for the iokernel's dataplane */
unsigned int sched_ctrl_core;	/* used for the iokernel's controlplane */
unsigned int sched_linux_core;	/* used by normal linux scheduler */

/* an array of core numbers that need to be polled */
static unsigned int poll_cores[NCPU];
static int poll_cores_nr;

struct core_state {
	struct thread	*pending_th;  /* a thread waiting run */
	struct thread	*cur_th;      /* the currently running thread */
	unsigned int	idle:1;	      /* is the core idle? */
	unsigned int	pending:1;    /* the next run is waiting */
	unsigned int	wait:1;       /* waiting for run to finish */
};

/* a per-CPU state table to manage scheduling operations */
static struct core_state state[NCPU];
/* policy-specific operations (TODO: should be made configurable) */
extern struct sched_ops simple_ops;
static const struct sched_ops *ops = &simple_ops;

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
	memset(p->flow_tbl, 0xFF, sizeof(p->flow_tbl));

	/* first assign the identity rxq to each active thread */
	for (i = 0; i < p->active_thread_count; i++) {
		ptrdiff_t idx = p->active_threads[i] - p->threads;
		p->flow_tbl[idx] = idx;
	}

	/* then assign the rest round-robin */
	for (i = 0; i < p->thread_count; i++) {
		if (p->flow_tbl[i] != UINT_MAX)
			continue;
		th = p->active_threads[j++ % p->active_thread_count];
		p->flow_tbl[i] = th - p->threads;
	}
}

static void sched_enable_kthread(struct thread *th, unsigned int core)
{
	struct proc *p = th->p;

	th->active = true;
	th->core = core;
	list_del_from(&p->idle_threads, &th->idle_link);
	th->at_idx = p->active_thread_count;
	p->active_threads[p->active_thread_count++] = th;
	sched_steer_flows(p);
	poll_thread(th);
}

static void sched_disable_kthread(struct thread *th)
{
	struct proc *p = th->p;

	th->active = false;
	p->active_threads[th->at_idx] =
		p->active_threads[--p->active_thread_count];
        p->active_threads[th->at_idx]->at_idx = th->at_idx;
	list_add(&p->idle_threads, &th->idle_link);
	sched_steer_flows(p);
	if (lrpc_empty(&th->txpktq))
		unpoll_thread(th);
}

static struct thread *sched_pick_kthread(struct proc *p, unsigned int core)
{
	struct thread *th;

	/* TODO: investigate whether O(n) time is an issue here */

	/* first try to find a thread that last ran on this core */
	list_for_each(&p->idle_threads, th, idle_link) {
		if (th->core == core)
			return th;
	}

	/* then try to find a thread that last ran on this core's sibling */
	list_for_each(&p->idle_threads, th, idle_link) {
		if (th->core == sched_siblings[core])
			return th;
	}

	/* finally pick the least recently used thread (to avoid thrashing) */
	return list_tail(&p->idle_threads, struct thread, idle_link);
}

/**
 * sched_run_on_core - allocates a core to a process
 * @p: the process to run on the core (can be NULL to make the core idle)
 * @core: the core number to run the process on
 *
 * If another process is currently running on @core, it will be preempted.
 *
 * Returns 0 if successful, otherwise fail.
 */
int sched_run_on_core(struct proc *p, unsigned int core)
{
	struct core_state *s = &state[core];
	struct thread *th = NULL;

	/* validate inputs --- mostly to catch bugs */
	if (unlikely((p && list_empty(&p->idle_threads)) || core >= NCPU ||
		     !bitmap_test(sched_allowed_cores, core))) {
		WARN();
		return -EINVAL;
	}

	/* select the best kthread to run on this core (if requested) */
	if (p) {
		th = sched_pick_kthread(p, core);
		if (unlikely(!th))
			return -ENOENT;

		proc_get(th->p);
		sched_enable_kthread(th, core);
	}

	/* if we're still busy with the last run request than stop here */
	if (s->wait) {
		if (s->pending_th) {
			sched_disable_kthread(s->pending_th);
			proc_put(s->pending_th->p);
		}
		s->pending_th = th;
		s->pending = true;
		return 0;
	}

	/* check if we need to interrupt the current core */
	if (!s->idle)
		ksched_enqueue_intr(core, KSCHED_INTR_CEDE);

	/* finally request that the new kthread run on this core */
	ksched_run(core, th ? th->tid : 0);
	if (s->cur_th) {
		sched_disable_kthread(s->cur_th);
		proc_put(s->cur_th->p);
	}
	s->cur_th = th;
	s->wait = true;
	s->idle = false;
	return 0;
}

static void sched_detect_congestion(struct proc *p)
{
	DEFINE_BITMAP(threads, NCPU);
	DEFINE_BITMAP(ios, NCPU);
	struct thread *th;
	uint32_t cur_tail, cur_head, last_head;
	int i;

	bitmap_init(threads, NCPU, false);
	bitmap_init(ios, NCPU, false);

	/* detect uthread runqueue congestion */
	for (i = 0; i < p->thread_count; i++) {
		th = &p->threads[i];
		cur_tail = load_acquire(&th->q_ptrs->rq_tail);
		last_head = th->last_rq_head;
		cur_head = ACCESS_ONCE(th->q_ptrs->rq_head);
		th->last_rq_head = cur_head;
		if (th->active ? wraps_lt(cur_tail, last_head) :
				 cur_head != cur_tail) {
			bitmap_set(threads, i);
		}
	}

	/* detect RX queue congestion */
	for (i = 0; i < p->thread_count; i++) {
		th = &p->threads[i];
		cur_tail = lrpc_poll_send_tail(&th->rxq);
		last_head = th->last_rxq_head;
		cur_head = ACCESS_ONCE(th->rxq.send_head);
		th->last_rxq_head = cur_head;
		if (th->active ? wraps_lt(cur_tail, last_head) :
				 cur_head != cur_tail) {
			bitmap_set(ios, i);
		}
	}

	/* notify the scheduler policy of the current congestion */
	ops->notify_congested(p, threads, ios);
}

static int sched_try_fast_rewake(struct thread *th)
{
	/*
	 * If the kthread has yielded voluntarily but still has pending I/O
	 * requests in flight, we can just wake it back up directly without
	 * wasting any extra time in the scheduler.
	 */
	if (ACCESS_ONCE(th->rxq.send_head) == lrpc_poll_send_tail(&th->rxq))
		return -EINVAL;

	ksched_run(th->core, th->tid);
	state[th->core].wait = true;
	return 0;
}

/**
 * sched_poll - advance the scheduler during each poll loop iteration
 */
void sched_poll(void)
{
	static uint64_t last_time = 0;
	DEFINE_BITMAP(idle, NCPU);
	struct core_state *s;
	uint64_t now;
	int i, core;
	bool idled = false;

	/*
	 * fast pass --- runs every poll loop
	 */

	bitmap_init(idle, NCPU, false);
	for (i = 0; i < poll_cores_nr; i++) {
		core = poll_cores[i];
		s = &state[core];

		/* check if a pending context switch finished */
		if (s->wait && ksched_poll_run_done(core)) {
			if (s->pending) {
				struct thread *th = s->pending_th;

				s->pending_th = NULL;
				s->pending = false;
				ksched_enqueue_intr(core, KSCHED_INTR_CEDE);
				ksched_run(core, th ? th->tid : 0);
				if (s->cur_th) {
					sched_disable_kthread(s->cur_th);
					proc_put(s->cur_th->p);
				}
				s->cur_th = th;
			} else {
				s->wait = false;
			}
		}

		/* check if a core went idle */
		if (!s->wait && !s->idle && ksched_poll_idle(core)) {
			if (s->cur_th) {
				if (sched_try_fast_rewake(s->cur_th) == 0)
					continue;
				sched_disable_kthread(s->cur_th);
				proc_put(s->cur_th->p);
				s->cur_th = NULL;
			}
			s->idle = true;
			bitmap_set(idle, core);
			idled = true;
		}
	}

	/*
	 * slow pass --- runs every IOKERNEL_POLL_INTERVAL
	 */

	now = microtime();
	if (now - last_time > IOKERNEL_POLL_INTERVAL) {
		int i;

		last_time = now;
		for (i = 0; i < dp.nr_clients; i++)
			sched_detect_congestion(dp.clients[i]);
	}

	/*
	 * final pass --- let the scheduler policy decide how to respond
	 */

	if (idled)
		ops->sched_poll(idle);
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
	return ops->notify_core_needed(p);
}

/**
 * sched_attach_proc - attach a process to the scheduler
 * @p: the process to attach
 *
 * Returns 0 if successful, otherwise fail.
 */
int sched_attach_proc(struct proc *p)
{
	int i;

	p->active_thread_count = 0;
	list_head_init(&p->idle_threads);
	for (i = 0; i < p->thread_count; i++) {
		p->threads[i].core = UINT_MAX;
		p->threads[i].active = false;
		list_add_tail(&p->idle_threads, &p->threads[i].idle_link);
	}

	return ops->proc_attach(p, &p->sched_cfg);
}

/**
 * sched_detach_proc - detach a process from the scheduler
 * @p: the process to detach
 */
void sched_detach_proc(struct proc *p)
{
	ops->proc_detach(p);
}

static int sched_scan_node(int node)
{
	struct cpu_info *info;
	int i, sib, ret = 0;

	for (i = 0; i < cpu_count; i++) {
		info = &cpu_info_tbl[i];
		if (info->package != node)
			continue;

		/* TODO: can only support hyperthread pairs */
		if (bitmap_popcount(info->thread_siblings_mask, NCPU) != 2)
			ret = -EINVAL;

		sib = bitmap_find_next_set(info->thread_siblings_mask,
					   NCPU, 0);
		if (sib == i)
			sib = bitmap_find_next_set(info->thread_siblings_mask,
						   NCPU, sib + 1);

		sched_siblings[i] = sib;
		if (i < sib)
			printf("[%d,%d]", i, sib);
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
	int i, sib;
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
		/* TODO: can only support one package */
		if (cpu_info_tbl[i].package != 0)
			continue;

		bitmap_set(sched_allowed_cores, i);
	}
	/* check for minimum number of cores required */
	i = bitmap_popcount(sched_allowed_cores, NCPU);
	if (i < 4) {
		log_err("sched: %d is not enough cores\n", i);
		return -EINVAL;
	}

	/*
	 * third pass: reserve cores for iokernel and system
	 */

	i = bitmap_find_next_set(sched_allowed_cores, NCPU, 0);
	sib = sched_siblings[i];
	bitmap_clear(sched_allowed_cores, i);
	bitmap_clear(sched_allowed_cores, sib);
	sched_dp_core = i;
	sched_ctrl_core = sib;
	sched_linux_core = sib;
	log_info("sched: dataplane on %d, control on %d, linux on %d",
		 sched_dp_core, sched_ctrl_core, sched_linux_core);

	/* check if configuration disables hyperthreads */
	if (cfg.noht) {
		for (i = 0; i < NCPU; i++) {
			if (!bitmap_test(sched_allowed_cores, i))
				continue;

			bitmap_clear(sched_allowed_cores, sched_siblings[i]);
		}
	}

	bitmap_for_each_set(sched_allowed_cores, NCPU, i)
		poll_cores[poll_cores_nr++] = i;

	return 0;
}
