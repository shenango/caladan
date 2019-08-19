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
__thread thread_t *__self;
/* a pointer to the top of the per-kthread (TLS) runtime stack */
static __thread void *runtime_stack;
/* a pointer to the bottom of the per-kthread (TLS) runtime stack */
static __thread void *runtime_stack_base;

/* Flag to prevent watchdog from running */
bool disable_watchdog;

/* real-time compute congestion signals (shared with the iokernel) */
struct congestion_info *runtime_congestion;

/* fast allocation of struct thread */
static struct slab thread_slab;
static struct tcache *thread_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, thread_pt);

/* used to track cycle usage in scheduler */
static __thread uint64_t last_tsc;
/* used to force timer and network processing after a timeout */
static __thread uint64_t last_watchdog_tsc;
/* used to update performance counters */
static __thread uint64_t last_pmc_tsc;

/**
 * In inc/runtime/thread.h, this function is declared inline (rather than static
 * inline) so that it is accessible to the Rust bindings. As a result, it must
 * also appear in a source file to avoid linker errors.
 */
thread_t *thread_self(void);

static __noreturn __noinline void schedule(void);
static void thread_finish_exit(void);

static __noreturn void jmp_runtime_nosave(runtime_fn_t fn);
static void jmp_runtime(runtime_fn_t fn);


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
	assert(th->state == THREAD_STATE_RUNNABLE);

	__self = th;
	th->state = THREAD_STATE_RUNNING;
	if (unlikely(load_acquire(&th->stack_busy))) {
		/* wait until the scheduler finishes switching stacks */
		while (load_acquire(&th->stack_busy))
			cpu_relax();
	} else if (!th->stack) {
		th->stack = stack_alloc();
		if (unlikely(!th->stack)) {
			log_warn_ratelimited("runtime: out of stacks");
			th->state = THREAD_STATE_SLEEPING;
			thread_ready(th);
			spin_lock(&myk()->lock);
			jmp_runtime_nosave(schedule);
		}
		th->tf.rsp = stack_init_to_rsp(th->stack, thread_exit);
	}

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
	assert(newth->state == THREAD_STATE_RUNNABLE);

	__self = newth;
	newth->state = THREAD_STATE_RUNNING;
	if (unlikely(load_acquire(&newth->stack_busy))) {
		/* wait until the scheduler finishes switching stacks */
		while (load_acquire(&newth->stack_busy))
			cpu_relax();
	} else if (!newth->stack) {
		newth->stack = stack_alloc();
		if (unlikely(!newth->stack)) {
			log_warn_ratelimited("runtime: out of stacks");
			newth->state = THREAD_STATE_SLEEPING;
			thread_ready(newth);
			spin_lock(&myk()->lock);
			jmp_runtime(schedule);
			return;
		}
		newth->tf.rsp = stack_init_to_rsp(newth->stack, thread_exit);
	}

	__jmp_thread_direct(&oldth->tf, &newth->tf, &oldth->stack_busy);
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

	__jmp_runtime(&thread_self()->tf, fn, runtime_stack);
}

/**
 * jmp_runtime_nosave - jumps to a function in the runtime without saving the
 *			caller's state
 * @fn: the runtime function to call
 */
static __noreturn void jmp_runtime_nosave(runtime_fn_t fn)
{
	assert_preempt_disabled();

	__jmp_runtime_nosave(fn, runtime_stack);
}

static void drain_overflow(struct kthread *l)
{
	thread_t *th;

	assert_spin_lock_held(&l->lock);

	while (l->rq_head - l->rq_tail < RUNTIME_RQ_SIZE) {
		th = list_pop(&l->rq_overflow, thread_t, link);
		if (!th)
			break;
		l->rq[l->rq_head++ % RUNTIME_RQ_SIZE] = th;
	}
}

static bool work_available(struct kthread *k)
{
	return ACCESS_ONCE(k->rq_tail) != ACCESS_ONCE(k->rq_head) ||
	        !list_empty(&k->rq_overflow) || softirq_work_available(k);
}

static bool steal_work(struct kthread *l, struct kthread *r)
{
	thread_t *th;
	uint32_t i, avail, rq_tail, lrq_head, lrq_tail;

	assert_spin_lock_held(&l->lock);

	if (!work_available(r))
		return false;
	if (!spin_try_lock(&r->lock))
		return false;

	/* try to steal directly from the runqueue */
	avail = load_acquire(&r->rq_head) - r->rq_tail;
	if (avail) {
		/* steal half the tasks */
		avail = div_up(avail, 2);
		rq_tail = r->rq_tail;
		lrq_tail = l->rq_tail;
		lrq_head = l->rq_head;
		for (i = 0; i < avail && lrq_head - lrq_tail < RUNTIME_RQ_SIZE; i++)
			l->rq[lrq_head++ % RUNTIME_RQ_SIZE] = r->rq[rq_tail++ % RUNTIME_RQ_SIZE];
		for (; i < avail; i++)
			list_add_tail(&l->rq_overflow, &r->rq[rq_tail++ % RUNTIME_RQ_SIZE]->link);
		store_release(&r->rq_tail, rq_tail);
		ACCESS_ONCE(r->q_ptrs->rq_tail) += avail;
		spin_unlock(&r->lock);

		l->rq_head = lrq_head;
		ACCESS_ONCE(l->q_ptrs->rq_head) += avail;
		STAT(THREADS_STOLEN) += avail;
		return true;
	}

	/* check for overflow tasks */
	th = list_pop(&r->rq_overflow, thread_t, link);
	if (th) {
		ACCESS_ONCE(r->q_ptrs->rq_tail)++;
		goto done;
	}

	/* check for softirqs */
	th = softirq_run_thread(r, RUNTIME_SOFTIRQ_REMOTE_BUDGET);
	if (th) {
		STAT(SOFTIRQS_STOLEN)++;
		goto done;
	}

done:
	if (th) {
		if (likely(l->rq_head - l->rq_tail < RUNTIME_RQ_SIZE))
			l->rq[l->rq_head++ % RUNTIME_RQ_SIZE] = th;
		else
			list_add_tail(&l->rq_overflow, &th->link);
		ACCESS_ONCE(l->q_ptrs->rq_head)++;
		STAT(THREADS_STOLEN)++;
	}

	spin_unlock(&r->lock);
	return th != NULL;
}

static __noinline struct thread *do_watchdog(struct kthread *l)
{
	thread_t *th;

	assert_spin_lock_held(&l->lock);

	/* then check the network queues */
	th = softirq_run_thread(l, RUNTIME_SOFTIRQ_LOCAL_BUDGET);
	if (th) {
		STAT(SOFTIRQS_LOCAL)++;
		return th;
	}

	return NULL;
}

/* the main scheduler routine, decides what to run next */
static __noreturn __noinline void schedule(void)
{
	struct kthread *r = NULL, *l = myk();
	uint64_t start_tsc, end_tsc;
	thread_t *th = NULL;
	unsigned int start_idx;
	unsigned int iters = 0;
	int i, sibling;

	assert_spin_lock_held(&l->lock);
	assert(l->parked == false);

	/* unmark busy for the stack of the last uthread */
	if (__self != NULL) {
		store_release(&__self->stack_busy, false);
		__self = NULL;
	}

	/* detect misuse of preempt disable */
	BUG_ON((preempt_cnt & ~PREEMPT_NOT_PENDING) != 1);

	/* update entry stat counters */
	STAT(RESCHEDULES)++;
	start_tsc = rdtsc();
	STAT(PROGRAM_CYCLES) += start_tsc - last_tsc;

	/* increment the RCU generation number (even is in scheduler) */
	store_release(&l->rcu_gen, l->rcu_gen + 1);
	assert((l->rcu_gen & 0x1) == 0x0);

	/* check if we need to refresh performance counters */
	if (start_tsc - last_pmc_tsc >= cycles_per_us * RUNTIME_PMC_US) {
		last_pmc_tsc = start_tsc;
		pmc_periodic(l);
	}

	/* if it's been too long, run the softirq handler */
	if (!disable_watchdog &&
	    unlikely(start_tsc - last_watchdog_tsc >=
	             cycles_per_us * RUNTIME_WATCHDOG_US)) {
		last_watchdog_tsc = start_tsc;
		th = do_watchdog(l);
		if (th)
			goto done;
	}

	/* move overflow tasks into the runqueue */
	if (unlikely(!list_empty(&l->rq_overflow)))
		drain_overflow(l);

	/* first try the local runqueue */
	if (l->rq_head != l->rq_tail)
		goto done;

	/* reset the local runqueue since it's empty */
	l->rq_head = l->rq_tail = 0;

again:
	/* then check for local softirqs */
	th = softirq_run_thread(l, RUNTIME_SOFTIRQ_LOCAL_BUDGET);
	if (th) {
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
	for (i = 0; i < nrks; i++) {
		int idx = (start_idx + i) % nrks;
		if (ks[idx] != l && steal_work(l, ks[idx]))
			goto done;
	}

	/* recheck for local softirqs one last time */
	th = softirq_run_thread(l, RUNTIME_SOFTIRQ_LOCAL_BUDGET);
	if (th) {
		STAT(SOFTIRQS_LOCAL)++;
		goto done;
	}

	/* keep trying to find work until the polling timeout expires */
	if (!preempt_needed() &&
	    (++iters < RUNTIME_SCHED_POLL_ITERS ||
	     rdtsc() - start_tsc < cycles_per_us * RUNTIME_SCHED_MIN_POLL_US ||
	     softirq_work_soon(l, start_tsc)))
		goto again;

	l->parked = true;
	spin_unlock(&l->lock);

	/* did not find anything to run, park this kthread */
	STAT(SCHED_CYCLES) += rdtsc() - start_tsc;
	/* we may have got a preempt signal before voluntarily yielding */
	kthread_park(!preempt_needed());
	start_tsc = rdtsc();
	iters = 0;

	spin_lock(&l->lock);
	l->parked = false;
	goto again;

done:
	/* pop off a thread and run it */
	if (!th) {
		assert(l->rq_head != l->rq_tail);
		th = l->rq[l->rq_tail++ % RUNTIME_RQ_SIZE];
		ACCESS_ONCE(l->q_ptrs->rq_tail)++;
	}

	/* move overflow tasks into the runqueue */
	if (unlikely(!list_empty(&l->rq_overflow)))
		drain_overflow(l);

	spin_unlock(&l->lock);

	/* update exit stat counters */
	end_tsc = rdtsc();
	STAT(SCHED_CYCLES) += end_tsc - start_tsc;
	last_tsc = end_tsc;
	if (cores_have_affinity(th->last_cpu, l->curr_cpu))
		STAT(LOCAL_RUNS)++;
	else
		STAT(REMOTE_RUNS)++;

	/* increment the RCU generation number (odd is in thread) */
	store_release(&l->rcu_gen, l->rcu_gen + 1);
	assert((l->rcu_gen & 0x1) == 0x1);

	/* and jump into the next thread */
	jmp_thread(th);
}


static __always_inline void enter_schedule(thread_t *myth, bool is_exit)
{
	struct kthread *k = myk();
	thread_t *th;

	assert_preempt_disabled();

	spin_lock(&k->lock);

	th = k->rq[k->rq_tail % RUNTIME_RQ_SIZE];

	/* slow path: switch from the uthread stack to the runtime stack */
	if (k->rq_head == k->rq_tail ||
	    (is_exit && (myth->main_thread || th->stack)) ||
	    (!disable_watchdog &&
	     unlikely(rdtsc() - last_watchdog_tsc >
		      cycles_per_us * RUNTIME_WATCHDOG_US))) {
		if (is_exit) {
			jmp_runtime_nosave(thread_finish_exit);
		} else {
			jmp_runtime(schedule);
			return;
		}
	}

	/* fast path: switch directly to the next uthread */
#ifdef DEBUG
	uint64_t now = rdtsc();
	STAT(PROGRAM_CYCLES) += now - last_tsc;
	last_tsc = now;
#endif

	/* pop the next runnable thread from the queue */
	k->rq_tail++;
	ACCESS_ONCE(k->q_ptrs->rq_tail)++;
	spin_unlock(&k->lock);

	/* increment the RCU generation number (odd is in thread) */
	store_release(&k->rcu_gen, k->rcu_gen + 2);
	assert((k->rcu_gen & 0x1) == 0x1);

	/* check for misuse of preemption disabling */
	BUG_ON((preempt_cnt & ~PREEMPT_NOT_PENDING) != 1);

	/* check if we're switching into the same thread as before */
	if (!is_exit && unlikely(th == myth)) {
		th->state = THREAD_STATE_RUNNING;
		th->stack_busy = false;
		preempt_enable();
		return;
	}

	/* switch stacks and enter the next thread */
	STAT(RESCHEDULES)++;
	if (cores_have_affinity(th->last_cpu, k->curr_cpu))
		STAT(LOCAL_RUNS)++;
	else
		STAT(REMOTE_RUNS)++;

	if (is_exit) {
		th->stack = myth->stack;
		tcache_free(&perthread_get(thread_pt), myth);
		th->tf.rsp = stack_init_to_rsp(th->stack, thread_exit);
		assert(th->state == THREAD_STATE_RUNNABLE);
		__self = th;
		th->state = THREAD_STATE_RUNNING;
		__jmp_thread(&th->tf);
	} else {
		jmp_thread_direct(myth, th);
	}
}

static __noreturn __always_inline void enter_schedule_exit(thread_t *myth)
{
	enter_schedule(myth, true);
	 __builtin_unreachable();
}

static __always_inline void enter_schedule_yield(thread_t *myth)
{
	enter_schedule(myth, false);
}


/**
 * thread_park_and_unlock_np - puts a thread to sleep, unlocks the lock @l,
 * and schedules the next thread
 * @l: the lock to be released
 */
void thread_park_and_unlock_np(spinlock_t *l)
{
	thread_t *myth = thread_self();

	assert_preempt_disabled();
	assert_spin_lock_held(l);
	assert(myth->state == THREAD_STATE_RUNNING);

	myth->state = THREAD_STATE_SLEEPING;
	myth->stack_busy = true;
	myth->last_cpu = myk()->curr_cpu;
	spin_unlock(l);

	enter_schedule_yield(myth);
}

/**
 * thread_yield - yields the currently running thread
 *
 * Yielding will give other threads a chance to run.
 */
void thread_yield(void)
{
	static __thread unsigned long nextk;
	struct kthread *r, *k;
	thread_t *myth = thread_self();

	/* check for softirqs */
	softirq_run(RUNTIME_SOFTIRQ_LOCAL_BUDGET);

	k = getk();

	/* try to drain parked kthreads for fairness */
	r = ks[nextk++ % maxks];
	if (ACCESS_ONCE(r->parked)) {
		spin_lock(&k->lock);
		if (likely(ACCESS_ONCE(r->parked)))
			steal_work(k, r);
		spin_unlock(&k->lock);
	}

	assert(myth->state == THREAD_STATE_RUNNING);
	myth->state = THREAD_STATE_SLEEPING;
	myth->last_cpu = k->curr_cpu;
	store_release(&myth->stack_busy, true);
	thread_ready(myth);

	enter_schedule_yield(myth);
}

/**
 * thread_ready - marks a uthread as a runnable
 * @th: the thread to mark runnable
 *
 * This function can only be called when @th is sleeping.
 */
void thread_ready(thread_t *th)
{
	struct kthread *k;
	uint32_t rq_tail;

	assert(th->state == THREAD_STATE_SLEEPING);
	th->state = THREAD_STATE_RUNNABLE;

	k = getk();
	if (cores_have_affinity(th->last_cpu, k->curr_cpu))
		STAT(LOCAL_WAKES)++;
	else
		STAT(REMOTE_WAKES)++;
	rq_tail = load_acquire(&k->rq_tail);
	ACCESS_ONCE(k->q_ptrs->rq_head)++;
	if (unlikely(k->rq_head - rq_tail >= RUNTIME_RQ_SIZE)) {
		assert(k->rq_head - rq_tail == RUNTIME_RQ_SIZE);
		spin_lock(&k->lock);
		list_add_tail(&k->rq_overflow, &th->link);
		spin_unlock(&k->lock);
		putk();
		STAT(RQ_OVERFLOW)++;
		return;
	}

	k->rq[k->rq_head % RUNTIME_RQ_SIZE] = th;
	store_release(&k->rq_head, k->rq_head + 1);
	putk();
}

static void thread_finish_cede(void)
{
	struct kthread *k = myk();
	thread_t *myth = thread_self();

	assert(myth->state == THREAD_STATE_RUNNING);
	myth->state = THREAD_STATE_SLEEPING;
	myth->last_cpu = k->curr_cpu;
	thread_ready(myth);

	STAT(PROGRAM_CYCLES) += rdtsc() - last_tsc;

	/* increment the RCU generation number (even - pretend in sched) */
	store_release(&k->rcu_gen, k->rcu_gen + 1);
	assert((k->rcu_gen & 0x1) == 0x0);

	/* cede this kthread to the iokernel */
	ACCESS_ONCE(k->parked) = true; /* deliberately racy */
	kthread_park(false);
	last_tsc = rdtsc();

	/* increment the RCU generation number (odd - back in thread) */
	store_release(&k->rcu_gen, k->rcu_gen + 1);
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
	preempt_disable();
	jmp_runtime(thread_finish_cede);
}

static __always_inline thread_t *__thread_create(bool include_stack)
{
	struct thread *th;
	struct stack *s = NULL;

	preempt_disable();
	th = tcache_alloc(&perthread_get(thread_pt));
	if (unlikely(!th)) {
		preempt_enable();
		return NULL;
	}

	if (include_stack) {
		s = stack_alloc();
		if (unlikely(!s)) {
			tcache_free(&perthread_get(thread_pt), th);
			preempt_enable();
			return NULL;
		}
	}
	preempt_enable();

	th->stack = s;
	th->state = THREAD_STATE_SLEEPING;
	th->main_thread = false;
	th->last_cpu = myk()->curr_cpu;

	return th;
}

static __always_inline thread_t *__thread_create_nostack(void)
{
	return __thread_create(false);
}
static __always_inline thread_t *__thread_create_withstack(void)
{
	return __thread_create(true);
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
	thread_t *th = __thread_create_nostack();
	if (unlikely(!th))
		return NULL;

	th->tf.rdi = (uint64_t)arg;
	th->tf.rbp = (uint64_t)0; /* just in case base pointers are enabled */
	th->tf.rip = (uint64_t)fn;
	th->stack_busy = false;
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
	thread_t *th = __thread_create_withstack();
	if (unlikely(!th))
		return NULL;

	th->tf.rsp = stack_init_to_rsp_with_buf(th->stack, &ptr,
						buf_len, thread_exit);
	th->tf.rdi = (uint64_t)ptr;
	th->tf.rbp = (uint64_t)0; /* just in case base pointers are enabled */
	th->tf.rip = (uint64_t)fn;
	th->stack_busy = false;
	*buf = ptr;
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
	th->last_cpu = sched_getcpu();
	thread_ready(th);
	return 0;
}

static void thread_finish_exit(void)
{
	struct thread *th = thread_self();

	/* if the main thread dies, kill the whole program */
	if (unlikely(th->main_thread))
		init_shutdown(EXIT_SUCCESS);
	stack_free(th->stack);
	tcache_free(&perthread_get(thread_pt), th);
	__self = NULL;

	schedule();
}

/**
 * thread_exit - terminates a thread
 */
void thread_exit(void)
{
	preempt_disable();

	enter_schedule_exit(thread_self());
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
	kthread_wait_to_attach();
	last_tsc = rdtsc();
	store_release(&k->rcu_gen, 1);

	spin_lock(&k->lock);
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

	tcache_init_perthread(thread_tcache, &perthread_get(thread_pt));

	s = stack_alloc();
	if (!s)
		return -ENOMEM;

	runtime_stack_base = (void *)s;
	runtime_stack = (void *)stack_init_to_rsp(s, runtime_top_of_stack); 

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
