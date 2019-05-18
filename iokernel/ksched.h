/*
 * ksched.h - an interface to the ksched kernel module
 */

#pragma once

#include <sched.h>
#include <sys/ioctl.h>

#include <base/stddef.h>
#include <base/atomic.h>
#include <base/limits.h>

#define __user
#include "../ksched/ksched.h"

extern int ksched_fd;
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
	return ksched_poll_run_done(core) &&
	       !load_acquire(&ksched_shm[core].busy);
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
 * the same core after ksched_enqueue_intr(), it will prevent any pending
 * interrupts from being delivered.
 */
static inline void ksched_enqueue_intr(unsigned int core, int type)
{
	switch(type) {
	case KSCHED_INTR_CEDE:
		store_release(&ksched_shm[core].cede, ksched_gens[core]);
		break;

	case KSCHED_INTR_YIELD:
		store_release(&ksched_shm[core].yield, ksched_gens[core]);
		break;

	default:
		WARN();
	}

	CPU_SET(core, &ksched_set);
}

/**
 * ksched_send_intrs - sends any pending interrupts
 */
static inline void ksched_send_intrs(void)
{
	struct ksched_intr_req req;
	int ret;

	if (CPU_COUNT(&ksched_set) == 0)
		return;

	req.len = sizeof(ksched_set); 
	req.mask = &ksched_set;
	ret = ioctl(ksched_fd, KSCHED_IOC_INTR, &req);
	BUG_ON(ret);

	CPU_ZERO(&ksched_set);
}
