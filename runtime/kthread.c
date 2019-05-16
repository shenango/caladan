/*
 * kthread.c - support for adding and removing kernel threads
 */

#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <base/atomic.h>
#include <base/cpu.h>
#include <base/list.h>
#include <base/lock.h>
#include <base/log.h>
#include <runtime/sync.h>
#include <runtime/timer.h>

#include "defs.h"

/* protects @ks and @nrks below */
DEFINE_SPINLOCK(klock);
/* the maximum number of kthreads */
unsigned int maxks;
/* the total number of attached kthreads (i.e. the size of @ks) */
unsigned int nrks;
/* the number of busy spinning kthreads (threads that don't park) */
unsigned int spinks;
/* the number of guaranteed kthreads (we can always have this many if we want,
 * must be >= 1) */
unsigned int guaranteedks = 1;
/* the number of active kthreads */
static atomic_t runningks;
/* an array of attached kthreads (@nrks in total) */
struct kthread *ks[NCPU];
/* kernel thread-local data */
__thread struct kthread *mykthread;
/* Map of cpu to kthread */
struct cpu_record cpu_map[NCPU] __attribute__((aligned(CACHE_LINE_SIZE)));

static struct kthread *allock(void)
{
	struct kthread *k;

	k = aligned_alloc(CACHE_LINE_SIZE,
			  align_up(sizeof(*k), CACHE_LINE_SIZE));
	if (!k)
		return NULL;

	memset(k, 0, sizeof(*k));
	spin_lock_init(&k->lock);
	list_head_init(&k->rq_overflow);
	mbufq_init(&k->txpktq_overflow);
	mbufq_init(&k->txcmdq_overflow);
	spin_lock_init(&k->timer_lock);
	k->park_efd = eventfd(0, 0);
	BUG_ON(k->park_efd < 0);
	k->detached = true;
	return k;
}

/**
 * kthread_init_thread - initializes state for the kthread
 *
 * Returns 0 if successful, or -ENOMEM if out of memory.
 */
int kthread_init_thread(void)
{
	mykthread = allock();
	if (!mykthread)
		return -ENOMEM;

	spin_lock_np(&klock);
	mykthread->kthread_idx = nrks;
	ks[nrks++] = mykthread;
	assert(nrks <= maxks);
	spin_unlock_np(&klock);

	return 0;
}

/*
 * kthread_yield_to_iokernel - block on eventfd until iokernel wakes us up
 */
static void kthread_yield_to_iokernel(void)
{
	struct kthread *k = myk();
	ssize_t s;
	uint64_t assigned_core, last_core = k->curr_cpu;

	clear_preempt_needed();

	/* yield to the iokernel */
	s = read(k->park_efd, &assigned_core, sizeof(assigned_core));
	while (unlikely(s != sizeof(uint64_t) && errno == EINTR)) {
		/* preempted while yielding, yield again */
		assert(preempt_needed());
		clear_preempt_needed();
		s = read(k->park_efd, &assigned_core, sizeof(assigned_core));
	}
	BUG_ON(s != sizeof(uint64_t));

	k->curr_cpu = assigned_core - 1;
	if (k->curr_cpu != last_core)
		STAT(CORE_MIGRATIONS)++;
	store_release(&cpu_map[assigned_core - 1].recent_kthread, k);
}

/*
 * kthread_park - block this kthread until the iokernel wakes it up.
 * @voluntary: true if this kthread parked because it had no work left
 *
 * This variant must be called with the local kthread lock held. It is intended
 * for use by the scheduler and for use by signal handlers.
 */
void kthread_park(bool voluntary)
{
	struct kthread *k = myk();

	assert_preempt_disabled();

	/* atomically verify we have at least @spinks kthreads running */
	if (voluntary && atomic_read(&runningks) <= spinks)
		return;
	int remaining_ks = atomic_sub_and_fetch(&runningks, 1);
	if (voluntary && unlikely(remaining_ks < spinks)) {
		atomic_inc(&runningks);
		return;
	}

	STAT(PARKS)++;

	/* signal to iokernel that we're about to park */
	while (!lrpc_send(&k->txcmdq, TXCMD_PARKED, 0))
		cpu_relax();

	/* perform the actual parking */
	kthread_yield_to_iokernel();

	/* iokernel has unparked us */
	atomic_inc(&runningks);
}

/**
 * kthread_wait_to_attach - block this kthread until the iokernel wakes it up.
 *
 * This variant is intended for initialization.
 */
void kthread_wait_to_attach(void)
{
	kthread_yield_to_iokernel();

	/* attach the kthread for the first time */
	atomic_inc(&runningks);
}
