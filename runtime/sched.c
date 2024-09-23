/*
 * sched.c - a scheduler for user-level threads
 */

#include <sched.h>

#include <base/stddef.h>
#include <base/lock.h>
#include <base/list.h>
#include <base/hash.h>
#include <base/limits.h>
#include <base/tcache.h>
#include <base/slab.h>
#include <base/log.h>
#include <runtime/sync.h>
#include <runtime/thread.h>

#include "defs.h"

/* the current running thread, or NULL if there isn't one */
DEFINE_PERTHREAD(thread_t *, __self);
/* a pointer to the top of the per-kthread (TLS) runtime stack */
static DEFINE_PERTHREAD(void *, runtime_stack);

/* Flag to prevent watchdog from running */
bool disable_watchdog;

/* real-time compute congestion signals (shared with the iokernel) */
struct runtime_info *runtime_info;

/* fast allocation of struct thread */
static struct slab thread_slab;
static struct tcache *thread_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, thread_pt);

/* used to track cycle usage in scheduler */
static DEFINE_PERTHREAD(uint64_t, last_tsc);

/**
 * In inc/runtime/thread.h, this function is declared inline (rather than static
 * inline) so that it is accessible to the Rust bindings. As a result, it must
 * also appear in a source file to avoid linker errors.
 */
thread_t *thread_self(void);

uint64_t get_uthread_specific(void)
{
	BUG_ON(!perthread_read_stable(__self));
	return (perthread_read_stable(__self))->tlsvar;
}

void set_uthread_specific(uint64_t val)
{
	BUG_ON(!perthread_read_stable(__self));
	(perthread_read_stable(__self))->tlsvar = val;
}

/**
 * cores_have_affinity - returns true if two cores have cache affinity
 * @cpua: the first core
 * @cpub: the second core
 */
static inline bool cores_have_affinity(unsigned int cpua, unsigned int cpub)
{
	return cpua == cpub ||
	       cpu_map[cpua].sibling_core == cpub;
}

/**
 * jmp_thread - runs a thread, popping its trap frame
 * @th: the thread to run
 *
 * This function restores the state of the thread and switches from the runtime
 * stack to the thread's stack. Runtime state is not saved.
 */
static __noreturn void jmp_thread(thread_t *th)
{
	assert_preempt_disabled();
	assert(th->thread_ready);

	perthread_store(__self, th);
	th->thread_ready = false;
	if (unlikely(load_acquire(&th->thread_running))) {
		/* wait until the scheduler finishes switching stacks */
		while (load_acquire(&th->thread_running))
			cpu_relax();
	}
	th->thread_running = true;
	__jmp_thread(&th->tf);
}

/**
 * jmp_thread_direct - runs a thread, popping its trap frame
 * @oldth: the last thread to run
 * @newth: the next thread to run
 *
 * This function restores the state of the thread and switches from the runtime
 * stack to the thread's stack. Runtime state is not saved.
 */
static void jmp_thread_direct(thread_t *oldth, thread_t *newth)
{
	assert_preempt_disabled();
	assert(newth->thread_ready);

	perthread_store(__self, newth);
	newth->thread_ready = false;
	if (unlikely(load_acquire(&newth->thread_running))) {
		/* wait until the scheduler finishes switching stacks */
		while (load_acquire(&newth->thread_running))
			cpu_relax();
	}
	newth->thread_running = true;
	__jmp_thread_direct(&oldth->tf, &newth->tf, &oldth->thread_running);
}

/**
 * jmp_runtime - saves the current trap frame and jumps to a function in the
 *               runtime
 * @fn: the runtime function to call
 *
 * WARNING: Only threads can call this function.
 *
 * This function saves state of the running thread and switches to the runtime
 * stack, making it safe to run the thread elsewhere.
 */
static void jmp_runtime(runtime_fn_t fn)
{
	assert_preempt_disabled();
	assert(thread_self() != NULL);

	__jmp_runtime(&thread_self()->tf, fn, perthread_read(runtime_stack));
}

/**
 * jmp_runtime_nosave - jumps to a function in the runtime without saving the
 *			caller's state
 * @fn: the runtime function to call
 */
static __noreturn void jmp_runtime_nosave(runtime_fn_t fn)
{
	assert_preempt_disabled();

	__jmp_runtime_nosave(fn, perthread_read(runtime_stack));
}

static void drain_overflow(struct kthread *l)
{
	thread_t *th;

	assert_spin_lock_held(&l->lock);
	assert(myk() == l || l->parked);

	while (l->rq_head - l->rq_tail < RUNTIME_RQ_SIZE) {
		th = list_pop(&l->rq_overflow, thread_t, link);
		if (!th)
			break;
		l->rq[l->rq_head++ % RUNTIME_RQ_SIZE] = th;
	}
}

static bool work_available(struct kthread *k, uint64_t now_tsc)
{
#ifdef GC
	if (get_gc_gen() != ACCESS_ONCE(k->local_gc_gen) &&
	    ACCESS_ONCE(k->parked)) {
		return true;
	}
#endif

	return ACCESS_ONCE(k->rq_tail) != ACCESS_ONCE(k->rq_head) ||
	       softirq_pending(k, now_tsc);
}

static void update_oldest_tsc(struct kthread *k)
{
	thread_t *th;

	assert_spin_lock_held(&k->lock);

	/* find the oldest thread in the runqueue */
	if (load_acquire(&k->rq_head) != k->rq_tail) {
		th = k->rq[k->rq_tail % RUNTIME_RQ_SIZE];
		ACCESS_ONCE(k->q_ptrs->oldest_tsc) = th->ready_tsc;
	}
}

/* drain up to nr threads from k's runqueue into list l */
static uint32_t drain_threads(struct kthread *k, struct list_head *l, uint32_t nr, bool update_tail)
{
	uint32_t i, rq_head, rq_tail;
	thread_t *th;

	rq_head = load_acquire(&k->rq_head);
	rq_tail = k->rq_tail;

	for (i = 0; i < nr; i++) {
		if (likely(wraps_lt(rq_tail, rq_head))) {
			th = k->rq[rq_tail++ % RUNTIME_RQ_SIZE];
		} else {
			th = list_pop(&k->rq_overflow, thread_t, link);
			if (!th)
				break;
		}
		list_add_tail(l, &th->link);
	}

	if (update_tail) {
		k->rq_tail = rq_tail;
		ACCESS_ONCE(k->q_ptrs->rq_tail) += i;
	}

	return i;
}

static void merge_runqueues(struct kthread *l, uint32_t lsize, struct kthread *r, uint32_t rsize)
{
	struct list_head l_ths, r_ths;
	thread_t *th, *cur_l, *cur_r;
	uint32_t i;

	assert_preempt_disabled();
	assert_spin_lock_held(&l->lock);
	assert_spin_lock_held(&r->lock);
	assert(myk() == l);

	list_head_init(&r_ths);
	rsize = drain_threads(r, &r_ths, rsize, true /* update_tail */);
	update_oldest_tsc(r);
	spin_unlock(&r->lock);

	list_head_init(&l_ths);
	lsize = drain_threads(l, &l_ths, lsize, false /* update_tail */);

	/* reset rq_head/tail */
	l->rq_head = l->rq_tail = 0;

	cur_r = list_pop(&r_ths, thread_t, link);
	cur_l = list_pop(&l_ths, thread_t, link);

	/* merge together two queues of threads sorted by ready_tsc */
	for (i = 0; i < lsize + rsize; i++) {
		if (cur_r && (!cur_l || cur_r->ready_tsc < cur_l->ready_tsc)) {
			th = cur_r;
			cur_r = list_pop(&r_ths, thread_t, link);
		} else {
			th = cur_l;
			cur_l = list_pop(&l_ths, thread_t, link);
		}

		assert(th);
		if (unlikely(l->rq_head - l->rq_tail >= RUNTIME_RQ_SIZE))
			list_add_tail(&l->rq_overflow, &th->link);
		else
			l->rq[l->rq_head++ % RUNTIME_RQ_SIZE] = th;
	}

	ACCESS_ONCE(l->q_ptrs->rq_head) += rsize;
	update_oldest_tsc(l);
}

static bool steal_work(struct kthread *l, struct kthread *r)
{
	uint64_t now_tsc = rdtsc();
	uint32_t lsize, rsize, num_to_steal = 0;

	assert_spin_lock_held(&l->lock);
	assert(myk() == l);

	if (!work_available(r, now_tsc) || !spin_try_lock(&r->lock))
		return rx_poll_locked(r);

#ifdef GC
	if (unlikely(get_gc_gen() != r->local_gc_gen)) {
		if (!ACCESS_ONCE(r->parked)) {
			spin_unlock(&r->lock);
			return false;
		}
		gc_kthread_report(r);
		drain_overflow(r);
	}
#endif

	/* first try to steal directly from the runqueue */
	lsize = l->q_ptrs->rq_head - l->q_ptrs->rq_tail;
	rsize = ACCESS_ONCE(r->q_ptrs->rq_head) - r->q_ptrs->rq_tail;
	if (lsize < rsize)
		num_to_steal = MIN(div_up(rsize - lsize, 2), RUNTIME_RQ_SIZE);
	if (num_to_steal) {
		merge_runqueues(l, lsize, r, num_to_steal);
		return true;
	}

	/* otherwise try to steal softirqs */
	if (softirq_run_locked(r)) {
		STAT(SOFTIRQS_STOLEN)++;
		spin_unlock(&r->lock);
		return true;
	}

	spin_unlock(&r->lock);
	return false;
}

static __noinline bool do_watchdog(struct kthread *l)
{
	bool work;

	assert_spin_lock_held(&l->lock);

	work = softirq_run_locked(l);
	if (work)
		STAT(SOFTIRQS_LOCAL)++;
	return work;
}

/* the main scheduler routine, decides what to run next */
static __noreturn __noinline void schedule(void)
{
	struct kthread *r = NULL, *l = myk();
	uint64_t start_tsc;
	thread_t *th = NULL;
	unsigned int start_idx;
	unsigned int iters = 0;
	int i, sibling;

	assert_spin_lock_held(&l->lock);
	assert(l->parked == false);

	/* detect misuse of preempt disable */
	BUG_ON((perthread_read(preempt_cnt) & ~PREEMPT_NOT_PENDING) != 1);

	/* unmark busy for the stack of the last uthread */
	if (likely(perthread_get_stable(__self) != NULL)) {
		store_release(&perthread_get_stable(__self)->thread_running, false);
		perthread_get_stable(__self) = NULL;
	}

	/* update entry stat counters */
	STAT(RESCHEDULES)++;
	start_tsc = rdtsc();
	STAT(PROGRAM_CYCLES) += start_tsc - perthread_read_stable(last_tsc);

	/* increment the RCU generation number (even is in scheduler) */
	store_release(&l->rcu_gen, l->rcu_gen + 1);
	ACCESS_ONCE(l->q_ptrs->rcu_gen) = l->rcu_gen;
	assert((l->rcu_gen & 0x1) == 0x0);

	/* check for pending preemption */
	if (unlikely(preempt_cede_needed(l))) {
		l->parked = true;
		spin_unlock(&l->lock);
		kthread_park_now();
		start_tsc = rdtsc();
		iters = 0;
		spin_lock(&l->lock);
		l->parked = false;
	}

#ifdef GC
	if (unlikely(get_gc_gen() != l->local_gc_gen))
		gc_kthread_report(l);
#endif

	/* if it's been too long, run the softirq handler */
	if (!disable_watchdog &&
	    unlikely(start_tsc - l->last_softirq_tsc >=
	             cycles_per_us * RUNTIME_WATCHDOG_US)) {
		l->last_softirq_tsc = start_tsc;
		if (do_watchdog(l))
			goto done;
	}

	/* move overflow tasks into the runqueue */
	if (unlikely(!list_empty(&l->rq_overflow)))
		drain_overflow(l);

	/* first try the local runqueue */
	if (l->rq_head != l->rq_tail)
		goto done;

again:

	if (unlikely(!mbufq_empty(&l->txpktq_overflow)))
		net_tx_drain_overflow();

	/* then check for local softirqs */
	if (softirq_run_locked(l)) {
		STAT(SOFTIRQS_LOCAL)++;
		goto done;
	}

	/* then try to steal from a sibling kthread */
	sibling = cpu_map[l->curr_cpu].sibling_core;
	r = cpu_map[sibling].recent_kthread;
	if (r && r != l && steal_work(l, r))
		goto done;

	/* try to steal from every kthread */
	start_idx = rand_crc32c((uintptr_t)l);
	for (i = 0; i < maxks; i++) {
		int idx = (start_idx + i) % maxks;
		if (ks[idx] != l && steal_work(l, ks[idx]))
			goto done;
	}

	/* recheck for local softirqs one last time */
	if (softirq_run_locked(l)) {
		STAT(SOFTIRQS_LOCAL)++;
		goto done;
	}

#ifdef GC
	if (unlikely(get_gc_gen() != l->local_gc_gen))
		gc_kthread_report(l);
#endif

	/* keep trying to find work until the polling timeout expires */
	perthread_get_stable(last_tsc) = rdtsc();
	if (!preempt_cede_needed(l) &&
	    (++iters < RUNTIME_SCHED_POLL_ITERS ||
	     perthread_get_stable(last_tsc) - start_tsc < cycles_per_us * RUNTIME_SCHED_MIN_POLL_US ||
	     storage_pending_completions(l) ||
	     !mbufq_empty(&l->txpktq_overflow))) {
		goto again;
	}

	l->parked = true;

	/* did not find anything to run, park this kthread */
	STAT(SCHED_CYCLES) += perthread_get_stable(last_tsc) - start_tsc;
	/* we may have got a preempt signal before voluntarily yielding */
	kthread_park();
	start_tsc = rdtsc();
	iters = 0;

	l->parked = false;
	goto again;

done:
	/* pop off a thread and run it */
	assert(l->rq_head != l->rq_tail);
	th = l->rq[l->rq_tail++ % RUNTIME_RQ_SIZE];
	ACCESS_ONCE(l->q_ptrs->rq_tail)++;

	/* move overflow tasks into the runqueue */
	if (unlikely(!list_empty(&l->rq_overflow)))
		drain_overflow(l);

	update_oldest_tsc(l);
	spin_unlock(&l->lock);

	/* update exit stat counters */
	perthread_get_stable(last_tsc) = rdtsc();
	STAT(SCHED_CYCLES) += perthread_get_stable(last_tsc) - start_tsc;
	if (cores_have_affinity(th->last_cpu, l->curr_cpu))
		STAT(LOCAL_RUNS)++;
	else
		STAT(REMOTE_RUNS)++;

	/* update exported thread run start time */
	th->run_start_tsc = perthread_get_stable(last_tsc);
	ACCESS_ONCE(l->q_ptrs->run_start_tsc) = perthread_get_stable(last_tsc);

	/* increment the RCU generation number (odd is in thread) */
	store_release(&l->rcu_gen, l->rcu_gen + 1);
	ACCESS_ONCE(l->q_ptrs->rcu_gen) = l->rcu_gen;
	assert((l->rcu_gen & 0x1) == 0x1);

	/* and jump into the next thread */
	jmp_thread(th);
}

static __always_inline void enter_schedule(thread_t *curth)
{
	struct kthread *k = myk();
	thread_t *th;
	uint64_t now_tsc;

	assert_preempt_disabled();

	/* prepare current thread for sleeping */
	curth->last_cpu = k->curr_cpu;

	spin_lock(&k->lock);
	now_tsc = rdtsc();

	/* slow path: switch from the uthread stack to the runtime stack */
	if (k->rq_head == k->rq_tail ||
	    preempt_cede_needed(k) ||
#ifdef GC
	    get_gc_gen() != k->local_gc_gen ||
#endif
	    (!disable_watchdog &&
	     unlikely(now_tsc - k->last_softirq_tsc >
		      cycles_per_us * RUNTIME_WATCHDOG_US))) {
		jmp_runtime(schedule);
		return;
	}

	/* fast path: switch directly to the next uthread */
	STAT(PROGRAM_CYCLES) += now_tsc - perthread_get_stable(last_tsc);
	perthread_get_stable(last_tsc) = now_tsc;

	/* pop the next runnable thread from the queue */
	th = k->rq[k->rq_tail++ % RUNTIME_RQ_SIZE];
	ACCESS_ONCE(k->q_ptrs->rq_tail)++;

	/* move overflow tasks into the runqueue */
	if (unlikely(!list_empty(&k->rq_overflow)))
		drain_overflow(k);

	update_oldest_tsc(k);
	spin_unlock(&k->lock);

	/* update exported thread run start time */
	th->run_start_tsc = perthread_get_stable(last_tsc);
	ACCESS_ONCE(k->q_ptrs->run_start_tsc) = perthread_get_stable(last_tsc);

	/* increment the RCU generation number (odd is in thread) */
	store_release(&k->rcu_gen, k->rcu_gen + 2);
	ACCESS_ONCE(k->q_ptrs->rcu_gen) = k->rcu_gen;
	assert((k->rcu_gen & 0x1) == 0x1);

	/* check for misuse of preemption disabling */
	BUG_ON((perthread_read(preempt_cnt) & ~PREEMPT_NOT_PENDING) != 1);

	/* check if we're switching into the same thread as before */
	if (unlikely(th == curth)) {
		th->thread_ready = false;
		preempt_enable();
		return;
	}

	/* switch stacks and enter the next thread */
	STAT(RESCHEDULES)++;
	if (cores_have_affinity(th->last_cpu, k->curr_cpu))
		STAT(LOCAL_RUNS)++;
	else
		STAT(REMOTE_RUNS)++;
	jmp_thread_direct(curth, th);
}

/**
 * thread_park_and_unlock_np - puts a thread to sleep, unlocks the lock @l,
 *                             and schedules the next thread
 * @l: the lock to be released
 */
void thread_park_and_unlock_np(spinlock_t *l)
{
	thread_t *curth = thread_self();

	assert_preempt_disabled();
	assert_spin_lock_held(l);
	spin_unlock(l);
	enter_schedule(curth);
}

/**
 * thread_park_and_preempt_enable - puts a thread to sleep, enables preemption,
 *                                  and schedules the next thread
 */
void thread_park_and_preempt_enable(void)
{
	thread_t *curth = thread_self();

	assert_preempt_disabled();
	enter_schedule(curth);
}

static void thread_ready_prepare(struct kthread *k, thread_t *th)
{
	/* check for misuse where a ready thread is marked ready again */
	BUG_ON(th->thread_ready);

	/* prepare thread to be runnable */
	th->thread_ready = true;
	th->ready_tsc = rdtsc();
	if (cores_have_affinity(th->last_cpu, k->curr_cpu))
		STAT(LOCAL_WAKES)++;
	else
		STAT(REMOTE_WAKES)++;
}

/**
 * thread_ready_locked - makes a uthread runnable (at tail, kthread lock held)
 * @th: the thread to mark runnable
 *
 * This function can only be called when @th is parked.
 * This function must be called with preemption disabled and the kthread lock
 * held.
 */
void thread_ready_locked(thread_t *th)
{
	struct kthread *k = myk();

	assert_preempt_disabled();
	assert_spin_lock_held(&k->lock);

	/* ensure new thread lands after older threads */
	if (unlikely(!list_empty(&k->rq_overflow)))
		drain_overflow(k);

	thread_ready_prepare(k, th);
	if (unlikely(k->rq_head - k->rq_tail >= RUNTIME_RQ_SIZE)) {
		assert(k->rq_head - k->rq_tail == RUNTIME_RQ_SIZE);
		list_add_tail(&k->rq_overflow, &th->link);
		ACCESS_ONCE(k->q_ptrs->rq_head)++;
		STAT(RQ_OVERFLOW)++;
		return;
	}

	k->rq[k->rq_head++ % RUNTIME_RQ_SIZE] = th;
	if (k->rq_head - k->rq_tail == 1)
		ACCESS_ONCE(k->q_ptrs->oldest_tsc) = th->ready_tsc;
	ACCESS_ONCE(k->q_ptrs->rq_head)++;
}

/**
 * thread_ready_head_locked - makes a uthread runnable (at head, kthread lock held)
 * @th: the thread to mark runnable
 *
 * This function can only be called when @th is parked.
 * This function must be called with preemption disabled and the kthread lock
 * held.
 */
void thread_ready_head_locked(thread_t *th)
{
	struct kthread *k = myk();
	thread_t *oldestth;

	assert_preempt_disabled();
	assert_spin_lock_held(&k->lock);

	thread_ready_prepare(k, th);

	if (k->rq_head != k->rq_tail)
		th->ready_tsc = k->rq[k->rq_tail % RUNTIME_RQ_SIZE]->ready_tsc;
	oldestth = k->rq[--k->rq_tail % RUNTIME_RQ_SIZE];
	k->rq[k->rq_tail % RUNTIME_RQ_SIZE] = th;
	if (unlikely(k->rq_head - k->rq_tail > RUNTIME_RQ_SIZE)) {
		list_add(&k->rq_overflow, &oldestth->link);
		k->rq_head--;
		STAT(RQ_OVERFLOW)++;
	}
	ACCESS_ONCE(k->q_ptrs->oldest_tsc) = th->ready_tsc;
	ACCESS_ONCE(k->q_ptrs->rq_head)++;
}

/**
 * thread_ready - makes a uthread runnable (at the tail of the queue)
 * @th: the thread to mark runnable
 *
 * This function can only be called when @th is parked.
 */
void thread_ready(thread_t *th)
{
	struct kthread *k;
	uint32_t rq_tail;

	k = getk();
	thread_ready_prepare(k, th);
	rq_tail = load_acquire(&k->rq_tail);
	if (unlikely(k->rq_head - rq_tail >= RUNTIME_RQ_SIZE ||
	             !list_empty_volatile(&k->rq_overflow))) {
		assert(k->rq_head - rq_tail <= RUNTIME_RQ_SIZE);
		spin_lock(&k->lock);
		list_add_tail(&k->rq_overflow, &th->link);
		drain_overflow(k);
		spin_unlock(&k->lock);
		ACCESS_ONCE(k->q_ptrs->rq_head)++;
		putk();
		STAT(RQ_OVERFLOW)++;
		return;
	}

	k->rq[k->rq_head % RUNTIME_RQ_SIZE] = th;
	store_release(&k->rq_head, k->rq_head + 1);
	if (k->rq_head - load_acquire(&k->rq_tail) == 1)
		ACCESS_ONCE(k->q_ptrs->oldest_tsc) = th->ready_tsc;
	ACCESS_ONCE(k->q_ptrs->rq_head)++;
	putk();
}

/**
 * thread_ready_head - makes a uthread runnable (at the head of the queue)
 * @th: the thread to mark runnable
 *
 * This function can only be called when @th is parked.
 */
void thread_ready_head(thread_t *th)
{
	struct kthread *k;
	thread_t *oldestth;

	k = getk();
	thread_ready_prepare(k, th);
	spin_lock(&k->lock);
	if (k->rq_head != k->rq_tail)
		th->ready_tsc = k->rq[k->rq_tail % RUNTIME_RQ_SIZE]->ready_tsc;
	oldestth = k->rq[--k->rq_tail % RUNTIME_RQ_SIZE];
	k->rq[k->rq_tail % RUNTIME_RQ_SIZE] = th;
	if (unlikely(k->rq_head - k->rq_tail > RUNTIME_RQ_SIZE)) {
		list_add(&k->rq_overflow, &oldestth->link);
		k->rq_head--;
		STAT(RQ_OVERFLOW)++;
	}
	spin_unlock(&k->lock);
	ACCESS_ONCE(k->q_ptrs->oldest_tsc) = th->ready_tsc;
	ACCESS_ONCE(k->q_ptrs->rq_head)++;
	putk();
}

static void thread_finish_cede(void)
{
	struct kthread *k = myk();
	thread_t *myth = thread_self();
	uint64_t tsc = rdtsc();

	/* update stats and scheduler state */
	myth->thread_running = false;
	myth->last_cpu = k->curr_cpu;
	perthread_store(__self, NULL);
	STAT(PROGRAM_CYCLES) += tsc - perthread_get_stable(last_tsc);

	/* mark ceded thread ready at head of runqueue */
	thread_ready_head(myth);

	/* increment the RCU generation number (even - pretend in sched) */
	store_release(&k->rcu_gen, k->rcu_gen + 1);
	ACCESS_ONCE(k->q_ptrs->rcu_gen) = k->rcu_gen;
	assert((k->rcu_gen & 0x1) == 0x0);

	/* cede this kthread to the iokernel */
	ACCESS_ONCE(k->parked) = true; /* deliberately racy */
	kthread_park_now();
	perthread_get_stable(last_tsc) = rdtsc();

	/* increment the RCU generation number (odd - back in thread) */
	store_release(&k->rcu_gen, k->rcu_gen + 1);
	ACCESS_ONCE(k->q_ptrs->rcu_gen) = k->rcu_gen;
	assert((k->rcu_gen & 0x1) == 0x1);

	/* re-enter the scheduler */
	spin_lock(&k->lock);
	k->parked = false;
	schedule();
}

/**
 * thread_cede - yields the running thread and gives the core back to iokernel
 */
void thread_cede(void)
{
	/* this will switch from the thread stack to the runtime stack */
	assert_preempt_disabled();
	jmp_runtime(thread_finish_cede);
}

/**
 * thread_yield - yields the currently running thread
 *
 * Yielding will give other threads and softirqs a chance to run.
 */
void thread_yield(void)
{
	thread_t *curth = thread_self();

	/* check for softirqs */
	softirq_run();

	preempt_disable();
	curth->thread_ready = false;
	thread_ready(curth);
	enter_schedule(curth);
}

static __always_inline thread_t *__thread_create(void)
{
	struct thread *th;
	struct stack *s;

	preempt_disable();
	th = tcache_alloc(perthread_ptr(thread_pt));
	if (unlikely(!th)) {
		preempt_enable();
		return NULL;
	}

	s = stack_alloc();
	if (unlikely(!s)) {
		tcache_free(perthread_ptr(thread_pt), th);
		preempt_enable();
		return NULL;
	}
	th->last_cpu = myk()->curr_cpu;
	preempt_enable();

	th->stack = s;
	th->main_thread = false;
	th->thread_ready = false;
	th->thread_running = false;

	return th;
}

/**
 * thread_create - creates a new thread
 * @fn: a function pointer to the starting method of the thread
 * @arg: an argument passed to @fn
 *
 * Returns 0 if successful, otherwise -ENOMEM if out of memory.
 */
thread_t *thread_create(thread_fn_t fn, void *arg)
{
	thread_t *th = __thread_create();
	if (unlikely(!th))
		return NULL;

	th->tf.rsp = stack_init_to_rsp(th->stack, thread_exit);
	th->tf.rdi = (uint64_t)arg;
	th->tf.rbp = (uint64_t)0; /* just in case base pointers are enabled */
	th->tf.rip = (uint64_t)fn;
	gc_register_thread(th);
	return th;
}

/**
 * thread_create_with_buf - creates a new thread with space for a buffer on the
 * stack
 * @fn: a function pointer to the starting method of the thread
 * @buf: a pointer to the stack allocated buffer (passed as arg too)
 * @buf_len: the size of the stack allocated buffer
 *
 * Returns 0 if successful, otherwise -ENOMEM if out of memory.
 */
thread_t *thread_create_with_buf(thread_fn_t fn, void **buf, size_t buf_len)
{
	void *ptr;
	thread_t *th = __thread_create();
	if (unlikely(!th))
		return NULL;

	th->tf.rsp = stack_init_to_rsp_with_buf(th->stack, &ptr, buf_len,
						thread_exit);
	th->tf.rdi = (uint64_t)ptr;
	th->tf.rbp = (uint64_t)0; /* just in case base pointers are enabled */
	th->tf.rip = (uint64_t)fn;
	*buf = ptr;
	gc_register_thread(th);
	return th;
}

/**
 * thread_spawn - creates and launches a new thread
 * @fn: a function pointer to the starting method of the thread
 * @arg: an argument passed to @fn
 *
 * Returns 0 if successful, otherwise -ENOMEM if out of memory.
 */
int thread_spawn(thread_fn_t fn, void *arg)
{
	thread_t *th = thread_create(fn, arg);
	if (unlikely(!th))
		return -ENOMEM;
	thread_ready(th);
	return 0;
}

/**
 * thread_spawn_main - creates and launches the main thread
 * @fn: a function pointer to the starting method of the thread
 * @arg: an argument passed to @fn
 *
 * WARNING: Only can be called once.
 *
 * Returns 0 if successful, otherwise -ENOMEM if out of memory.
 */
int thread_spawn_main(thread_fn_t fn, void *arg)
{
	static bool called = false;
	thread_t *th;

	BUG_ON(called);
	called = true;

	th = thread_create(fn, arg);
	if (!th)
		return -ENOMEM;
	th->main_thread = true;
	thread_ready(th);
	return 0;
}

static void thread_finish_exit(void)
{
	struct thread *th = thread_self();

	gc_remove_thread(th);
	perthread_store(__self, NULL);

	/* if the main thread dies, kill the whole program */
	if (unlikely(th->main_thread))
		init_shutdown(EXIT_SUCCESS);

	stack_free(th->stack);
	tcache_free(perthread_ptr(thread_pt), th);

	spin_lock(&myk()->lock);
	schedule();
}

/**
 * thread_exit - terminates a thread
 */
void thread_exit(void)
{
	/* can't free the stack we're currently using, so switch */
	preempt_disable();
	jmp_runtime_nosave(thread_finish_exit);
}

/**
 * immediately park each kthread when it first starts up, only schedule it once
 * the iokernel has granted it a core
 */
static __noreturn void schedule_start(void)
{
	struct kthread *k = myk();

	/*
	 * force kthread parking (iokernel assumes all kthreads are parked
	 * initially). Update RCU generation so it stays even after entering
	 * schedule().
	 */
	if (k->q_ptrs->oldest_tsc == 0)
		ACCESS_ONCE(k->q_ptrs->oldest_tsc) = UINT64_MAX;
	ACCESS_ONCE(k->parked) = true;
	kthread_wait_to_attach();
	perthread_store(last_tsc, rdtsc());
	store_release(&k->rcu_gen, 1);
	ACCESS_ONCE(k->q_ptrs->rcu_gen) = 1;

	spin_lock(&k->lock);
	k->parked = false;
	schedule();
}

/**
 * sched_start - used only to enter the runtime the first time
 */
void sched_start(void)
{
	preempt_disable();
	jmp_runtime_nosave(schedule_start);
}

static void runtime_top_of_stack(void)
{
	panic("a thread returned to the top of the stack");
}

/**
 * sched_init_thread - initializes per-thread state for the scheduler
 *
 * Returns 0 if successful, or -ENOMEM if out of memory.
 */
int sched_init_thread(void)
{
	struct stack *s;

	tcache_init_perthread(thread_tcache, perthread_ptr(thread_pt));

	s = stack_alloc();
	if (!s)
		return -ENOMEM;

	perthread_store(runtime_stack, (void *)stack_init_to_rsp(s, runtime_top_of_stack));

	return 0;
}

/**
 * sched_init - initializes the scheduler subsystem
 *
 * Returns 0 if successful, or -ENOMEM if out of memory.
 */
int sched_init(void)
{
	int ret, i, j, siblings;

	/*
	 * set up allocation routines for threads
	 */
	ret = slab_create(&thread_slab, "runtime_threads",
			  sizeof(struct thread), 0);
	if (ret)
		return ret;

	thread_tcache = slab_create_tcache(&thread_slab,
					   TCACHE_DEFAULT_MAG_SIZE);
	if (!thread_tcache) {
		slab_destroy(&thread_slab);
		return -ENOMEM;
	}

	for (i = 0; i < cpu_count; i++) {
		siblings = 0;
		bitmap_for_each_set(cpu_info_tbl[i].thread_siblings_mask,
				    cpu_count, j) {
			if (i == j)
				continue;
			BUG_ON(siblings++);
			cpu_map[i].sibling_core = j;
		}
	}

	return 0;
}
