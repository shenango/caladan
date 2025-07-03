/*
 * kthread.c - support for adding and removing kernel threads
 */

#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>

#include <base/atomic.h>
#include <base/cpu.h>
#include <base/list.h>
#include <base/lock.h>
#include <base/log.h>
#include <base/syscall.h>
#include <runtime/sync.h>
#include <runtime/timer.h>

#define __user
#include "defs.h"
#include "../ksched/ksched.h"

/* number of kthreads */
unsigned int maxks;
/* the number of busy spinning kthreads (threads that don't park) */
unsigned int spinks;
/* the number of guaranteed kthreads (we can always have this many if we want) */
unsigned int guaranteedks;
/* the number of active kthreads */
atomic_t runningks;
/* an array of kthreads (thread_count/maxks in total) */
struct kthread ks[NCPU];
/* kernel thread-local data */
DEFINE_PERTHREAD(struct kthread *, mykthread);
/* Map of cpu to kthread */
struct cpu_record cpu_map[NCPU] __attribute__((aligned(CACHE_LINE_SIZE)));
/* the file descriptor for the ksched module */
int ksched_fd;

/**
 * kthread_init_thread - initializes state for the kthread
 *
 * Returns 0 if successful, or -ENOMEM if out of memory.
 */
int kthread_init_thread(void)
{
	long ret;
	struct kthread *mykthread = myk();

	spin_lock_init(&mykthread->lock);
	list_head_init(&mykthread->rq_overflow);
	mbufq_init(&mykthread->txpktq_overflow);
	mbufq_init(&mykthread->txcmdq_overflow);
	spin_lock_init(&mykthread->timer_lock);

	mykthread->tid = thread_gettid();

	ret = syscall_ioctl(ksched_fd, KSCHED_IOC_GETTID, 0);
	BUG_ON(ret <= 0);

	iok.threads[kthread_idx(mykthread)].tid = ret;

	return 0;
}

/*
 * kthread_yield_to_iokernel - block until iokernel wakes us up
 */
static __always_inline void kthread_yield_to_iokernel(void)
{
	struct kthread *k = myk();
	uint64_t last_core = k->curr_cpu;
	ssize_t s;

	/* yield to the iokernel */
	do {
		clear_preempt_needed();
		s = syscall_ioctl(ksched_fd, KSCHED_IOC_PARK, perthread_read(uintr_stack));
	} while (unlikely(s < 0 || preempt_cede_needed(k)));

	k->curr_cpu = s;
	if (k->curr_cpu != last_core)
		STAT(CORE_MIGRATIONS)++;
	store_release(&cpu_map[s].recent_kthread, k);
}


#ifdef DIRECTPATH

static atomic64_t kthread_gen;
static uint64_t flow_assignment_gen;
static DEFINE_SPINLOCK(flow_assignment_lock);
static DEFINE_BITMAP(kthread_awake, NCPU);

static void flows_update(void)
{
	int i, pos, nrawake;
	uint64_t start = rdtsc(), cur_gen;
	static unsigned int fg_map[NCPU];
	static unsigned int awakeks[NCPU];
	static DEFINE_BITMAP(kawake_local, NCPU);

again:

	if (!spin_try_lock_np(&flow_assignment_lock))
		goto done;

	cur_gen = atomic64_read(&kthread_gen);
	if (cur_gen == flow_assignment_gen) {
		spin_unlock_np(&flow_assignment_lock);
		goto done;
	}

	ACCESS_ONCE(flow_assignment_gen) = cur_gen;

	/* make a copy of kthread_awake */
	for (i = 0; i < BITMAP_LONG_SIZE(NCPU); i++)
		kawake_local[i] = ACCESS_ONCE(kthread_awake[i]);

	nrawake = 0;
	bitmap_for_each_set(kawake_local, maxks, i) {
		fg_map[i] = i;
		awakeks[nrawake++] = i;
	}

	if (!nrawake)
		goto out;

	pos = 0;
	bitmap_for_each_cleared(kawake_local, maxks, i) {
		/* steer packets away from this kthread */
		fg_map[i] = awakeks[pos++];
		if (pos == nrawake)
			pos = 0;
	}

	net_ops.steer_flows(fg_map);

out:
	spin_unlock_np(&flow_assignment_lock);

	if (unlikely(ACCESS_ONCE(flow_assignment_gen) != atomic64_read(&kthread_gen)))
		goto again;

done:
	STAT(FLOW_STEERING_CYCLES) += rdtsc() - start;
}

static void flows_notify_waking(void)
{
	if (!net_ops.steer_flows)
		return;

	bitmap_atomic_set(kthread_awake, kthread_idx(myk()));
	atomic64_inc(&kthread_gen);
	flows_update();
}

static void flows_notify_parking(bool voluntary)
{
	if (!net_ops.steer_flows)
		return;

	bitmap_atomic_clear(kthread_awake, kthread_idx(myk()));
	atomic64_inc(&kthread_gen);
	if (voluntary)
		flows_update();
}

#else
static inline void flows_notify_waking(void) {}
static inline void flows_notify_parking(bool voluntary) {}
#endif

static void merge_directpath_counters(void)
{
	struct kthread *k = myk();
	uint64_t tmp;

	tmp = k->q_ptrs->directpath_strides_consumed;

	if (!tmp)
		return;

	k->q_ptrs->directpath_strides_consumed = 0;
	atomic64_fetch_and_add(&runtime_info->directpath_strides_consumed, tmp);
}

/*
 * kthread_park_now - block this kthread until the iokernel wakes it up.
 *
 * This variant must be called without the local kthread lock held.
 */
void kthread_park_now(void)
{
	assert_preempt_disabled();

	atomic_sub_and_fetch(&runningks, 1);

	merge_directpath_counters();

	flows_notify_parking(false);

	STAT(PARKS)++;

	/* perform the actual parking */
	kthread_yield_to_iokernel();

	/* iokernel has unparked us */
	atomic_inc(&runningks);

	flows_notify_waking();
}


/*
 * kthread_park - block this kthread until the iokernel wakes it up.
 *
 * This variant must be called with the local kthread lock held.
 */
void kthread_park(void)
{
	struct kthread *k = myk();
	bool voluntary;

	voluntary = !preempt_cede_needed(k);
	voluntary &= !preempt_park_needed(k);

	assert_preempt_disabled();

	/* atomically verify we have at least @spinks kthreads running */
	if (voluntary && atomic_read(&runningks) <= spinks)
		return;
	int remaining_ks = atomic_sub_and_fetch(&runningks, 1);
	if (voluntary && unlikely(remaining_ks < spinks)) {
		atomic_inc(&runningks);
		return;
	}

	// Drop lock
	spin_unlock(&k->lock);

	merge_directpath_counters();

	flows_notify_parking(!preempt_cede_needed(k));

	STAT(PARKS)++;

	/* perform the actual parking */
	kthread_yield_to_iokernel();

	/* iokernel has unparked us */
	atomic_inc(&runningks);

	flows_notify_waking();

	spin_lock(&k->lock);
}

/**
 * kthread_wait_to_attach - block this kthread until the iokernel wakes it up.
 *
 * This variant is intended for initialization.
 */
void kthread_wait_to_attach(void)
{
	struct kthread *k = myk();
	int s;

	do {
		s = syscall_ioctl(ksched_fd, KSCHED_IOC_START, perthread_read(uintr_stack));
	} while (s < 0);

	k->curr_cpu = s;
	store_release(&cpu_map[s].recent_kthread, k);

	/* attach the kthread for the first time */
	atomic_inc(&runningks);

	flows_notify_waking();
}

/**
 * kthread_init - intitializes the kthread subsystem
 *
 * Returns 0 if successful.
 */
int kthread_init(void)
{
	ksched_fd = open("/dev/ksched", O_RDWR);
	if (ksched_fd < 0)
		return -errno;

	if (ioctl(ksched_fd, KSCHED_IOC_GET_USER_API_VER) != KSCHED_USER_API_VER) {
		log_err("ksched: version mismatch");
		return -1;
	}

	return 0;
}
