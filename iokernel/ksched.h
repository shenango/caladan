/*
 * ksched.h - an interface to the ksched kernel module
 */

#pragma once

#include <sched.h>
#include <sys/ioctl.h>
#include <signal.h>

#include <base/stddef.h>
#include <base/atomic.h>
#include <base/limits.h>

#define __user
#include "../ksched/ksched.h"

extern int ksched_fd, ksched_count, ksched_pmc_count, last_intr_core;
extern bool ksched_has_uintr;
extern struct ksched_shm_cpu *ksched_shm;
extern cpu_set_t ksched_set;
extern unsigned int ksched_gens[NCPU];

/**
 * ksched_run - runs a kthread on a specific core
 * @core: the core to run a kthread on
 * @tid: the kthread's TID (or zero to idle the core)
 */
static inline void ksched_run(unsigned int core, pid_t tid)
{
	unsigned int gen = ++ksched_gens[core];

	ksched_shm[core].tid = tid;
	store_release(&ksched_shm[core].gen, gen);
}

/**
 * ksched_poll_run_done - determines if the last ksched_run() call finished
 * @core: the core on which kthread_run() was called
 *
 * Returns true if finished.
 */
static inline bool ksched_poll_run_done(unsigned int core)
{
	return load_acquire(&ksched_shm[core].last_gen) == ksched_gens[core];
}

/**
 * ksched_poll_idle - determines if a core is currently idle
 * @core: the core to check if it is idle
 *
 * Returns true if idle.
 */
static inline bool ksched_poll_idle(unsigned int core)
{
	return !load_acquire(&ksched_shm[core].busy);
}

static inline void ksched_idle_hint(unsigned int core, unsigned int hint)
{
	ksched_shm[core].mwait_hint = hint;
}

enum {
	KSCHED_INTR_CEDE = 0,
	KSCHED_INTR_YIELD,
};

/**
 * ksched_enqueue_intr - enqueues an interrupt request on a core
 * @core: the core to interrupt
 * @type: the type of interrupt to enqueue
 *
 * The interrupt will not be sent until ksched_send_intrs(). This is done to
 * create an opportunity for batching interrupts. If ksched_run() is called on
 * the same core after ksched_enqueue_intr(), it may prevent interrupts
 * still pending for the last kthread from being delivered.
 */
static inline void ksched_enqueue_intr(unsigned int core, int type)
{
	unsigned int signum;

	switch (type) {
	case KSCHED_INTR_CEDE:
		signum = SIGUSR1;
		break;

	case KSCHED_INTR_YIELD:
		signum = SIGUSR2;
		break;

	default:
		WARN();
		return;
	}

	assert(signum - 1 < 64);
	ksched_shm[core].upid.puir |= 1UL << (signum - 1);
	ksched_shm[core].signum = signum;
	store_release(&ksched_shm[core].sig, ksched_gens[core]);
	CPU_SET(core, &ksched_set);
	ksched_count++;
	last_intr_core = core;
}

/**
 * ksched_enqueue_pmc - enqueues a performance counter request on a core
 * @core: the core to measure
 * @sel: the architecture-specific counter selector
 */
static inline void ksched_enqueue_pmc(unsigned int core, uint64_t sel)
{
	ksched_shm[core].pmcsel = sel;
	store_release(&ksched_shm[core].pmc, 1);
	CPU_SET(core, &ksched_set);
	ksched_count++;
	ksched_pmc_count++;
}

/**
 * ksched_poll_pmc - polls for a performance counter result
 * @core: the core to poll
 * @val: a pointer to store the result
 * @tsc: a pointer to store the timestamp of the result
 *
 * Returns true if succesful, otherwise counter is still being measured.
 */
static inline bool ksched_poll_pmc(unsigned int core, uint64_t *val, uint64_t *tsc)
{
	if (load_acquire(&ksched_shm[core].pmc) != 0)
		return false;
	*val = ACCESS_ONCE(ksched_shm[core].pmcval);
	*tsc = ACCESS_ONCE(ksched_shm[core].pmctsc);
	return true;
}

/**
 * ksched_send_intrs - sends any pending interrupts
 */
static inline void ksched_send_intrs(void)
{
	struct ksched_intr_req req;
	unsigned long request;
	int ret;

	if (ksched_count == 0)
		return;

	request = KSCHED_IOC_INTR;

	/* use UINTR if available (and not collecting pmc samples) */
	if (ksched_has_uintr && !ksched_pmc_count) {
		/* use senduipi instruction if there's just one interrupt */
		if (ksched_count == 1) {
			__builtin_ia32_senduipi(last_intr_core);
			goto done;
		}

		/* otherwise use ksched to multicast the UIPI */
		request = KSCHED_IOC_UINTR_MULTICAST;
	}

	req.len = sizeof(ksched_set);
	req.mask = &ksched_set;
	ret = ioctl(ksched_fd, request, &req);
	BUG_ON(ret);
	ksched_pmc_count = 0;

done:
	ksched_count = 0;
	CPU_ZERO(&ksched_set);
}
