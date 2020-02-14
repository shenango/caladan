

#include <base/list.h>
#include <base/lock.h>
#include <base/log.h>
#include <runtime/gc.h>
#include <runtime/sync.h>
#include <runtime/thread.h>

#include <signal.h>

#include "defs.h"

#ifdef GC

struct all_threads_percore {
	struct list_head		threads;
	spinlock_t		lock;
	unsigned int		pad[11];
};
BUILD_ASSERT(sizeof(struct all_threads_percore) == CACHE_LINE_SIZE);


/* global lock protecting gc state */
static DEFINE_SPINLOCK(gc_lock);
/* list of paused runnable uthreads */
static LIST_HEAD(paused_uthreads);
/* list of all threads with live stacks */
static struct all_threads_percore all_threads[NCPU];
/* bitmap of kthreads that have reported in for GC */
static DEFINE_BITMAP(gc_kthread_reports, NCPU);
/* this process's PID */
static pid_t mypid;

volatile bool world_stopped;
volatile uint64_t gc_gen;

void gc_kthread_report(struct kthread *k)
{
	uint32_t i, avail;
	thread_t *th;

	spin_lock(&gc_lock);

	assert_spin_lock_held(&k->lock);
	assert(myk() == k || ACCESS_ONCE(k->parked));
	assert(k->local_gc_gen + 1 == gc_gen);
	assert(!bitmap_test(gc_kthread_reports, k->kthread_idx));

	avail = load_acquire(&k->rq_head) - k->rq_tail;

	for (i = 0; i < avail; i++) {
		th = k->rq[k->rq_tail++ % RUNTIME_RQ_SIZE];
		list_add_tail(&paused_uthreads, &th->link);
	}

	while (true) {
		th = list_pop(&k->rq_overflow, struct thread, link);
		if (!th)
			break;

		avail++;
		list_add_tail(&paused_uthreads, &th->link);
	}

	ACCESS_ONCE(k->q_ptrs->rq_tail) += avail;
	k->local_gc_gen = gc_gen;
	bitmap_atomic_set(gc_kthread_reports, k->kthread_idx);
	spin_unlock(&gc_lock);
}

void gc_start_world(void)
{
	LIST_HEAD(tmp);
	thread_t *th;

	spin_lock_np(&gc_lock);
	assert(world_stopped);
	world_stopped = false;
	list_append_list(&tmp, &paused_uthreads);

	spin_unlock_np(&gc_lock);

	while (true) {
		th = list_pop(&tmp, thread_t, link);
		if (!th)
			break;

		th->state = THREAD_STATE_SLEEPING;
		thread_ready(th);
	}


}

void gc_stop_world(void)
{
	struct kthread *k;
	uint32_t i, done;

	spin_lock_np(&gc_lock);
	assert(!world_stopped);

	ACCESS_ONCE(gc_gen) = gc_gen + 1;
	ACCESS_ONCE(world_stopped) = true;

	bitmap_init(gc_kthread_reports, NCPU, false);
	spin_unlock(&gc_lock);

	k = myk();
	spin_lock(&k->lock);
	gc_kthread_report(k);
	spin_unlock_np(&k->lock);

	while (true) {
		for (i = 0; i < maxks; i++) {

			if (bitmap_test(gc_kthread_reports, i))
				continue;

			k = ks[i];
			if (ACCESS_ONCE(k->parked)) {
				spin_lock_np(&k->lock);
				if (ACCESS_ONCE(k->parked))
					gc_kthread_report(k);
				spin_unlock_np(&k->lock);
			} else {
				WARN_ON_ONCE(syscall(SYS_tgkill, mypid, k->tid, SIGUSR2));
			}
		}

		spin_lock_np(&gc_lock);
		done = bitmap_popcount(gc_kthread_reports, NCPU);
		if (done == maxks) {
			spin_unlock_np(&gc_lock);
			BUG_ON(!preempt_enabled());
			return;
		}
		spin_unlock_np(&gc_lock);
		delay_us(10);
		thread_yield();
	}

	BUG_ON(!preempt_enabled());

}

void gc_discover_all_stacks(stack_bounds_cb discover_cb)
{
	struct thread *th, *myth = thread_self();
	unsigned int i;
	uint64_t top;

	spin_lock_np(&gc_lock);
	assert(world_stopped);

	for (i = 0; i < maxks; i++) {
		spin_lock(&all_threads[i].lock);
		list_for_each(&all_threads[i].threads, th, gc_link) {
			top = th->tf.rsp;
			if (th == myth)
				top = (uint64_t)&th;
			else
				discover_cb(sizeof(th->tf) + (uintptr_t)&th->tf, (uintptr_t)&th->tf); // scan trapframes also
			discover_cb((uintptr_t)&th->stack->usable[STACK_PTR_SIZE], top);
		}
		spin_unlock(&all_threads[i].lock);
	}

	spin_unlock_np(&gc_lock);

}

int gc_remove_thread(thread_t *th)
{
	unsigned int aff;
	BUG_ON(preempt_enabled());
	assert_preempt_disabled();
	aff = th->onk;
	spin_lock_np(&all_threads[aff].lock);
	list_del_from(&all_threads[aff].threads, &th->gc_link);
	spin_unlock_np(&all_threads[aff].lock);

	return 0;
}

int gc_register_thread(thread_t *th)
{
	unsigned int aff;
	preempt_disable();
	aff = get_current_affinity();
	spin_lock(&all_threads[aff].lock);
	list_add(&all_threads[aff].threads, &th->gc_link);
	th->onk = aff;
	spin_unlock_np(&all_threads[aff].lock);

	return 0;
}

int gc_init(void)
{
	unsigned int i;

	for (i = 0; i < maxks; i++) {
		list_head_init(&all_threads[i].threads);
		spin_lock_init(&all_threads[i].lock);
	}
	mypid = getpid();
	return 0;
}

#endif
