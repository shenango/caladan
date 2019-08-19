/*
 * sched.h - low-level scheduler routines (e.g. adding or preempting cores)
 */

#pragma once

#include <base/stddef.h>
#include <base/bitmap.h>
#include <base/limits.h>

#include "defs.h"

struct sched_ops {
	/**
	 * proc_attach - attaches a new process to the scheduler
	 * @p: the new process to attach
	 * @cfg: the requested scheduler configuration of the process
	 *
	 * Typically this is an opportunity to allocate scheduler-specific
	 * data for the process and to validate if the provisioned resources
	 * are available.
	 */
	int (*proc_attach)(struct proc *p, struct sched_spec *cfg);

	/**
	 * proc_detach - detaches an existing process from the scheduler
	 * @p: the existing process to detach
	 *
	 * Typically this is an opportunity to free scheduler-specific
	 * data for the process.
	 */
	void (*proc_detach)(struct proc *p);

	/**
	 * notify_congested - notifies the scheduler of process congestion
	 * @p: the process for which congestion has changed
	 * @threads: a bitmap of congested uthreads (a bit per kthread)
	 * @io: a bitmap of congested I/Os (a bit per kthread)
	 *
	 * This notifier informs the scheduler of when processes become
	 * congested or uncongested, driving core allocation decisions.
	 */
	void (*notify_congested)(struct proc *p, bitmap_ptr_t threads,
			         bitmap_ptr_t io);

	/**
	 * notify_core_needed - notifies the scheduler that a core is needed
	 * @p: the process that needs an additional core
	 *
	 * Returns 0 if a core was added successfully.
	 */
	int (*notify_core_needed)(struct proc *p);

	/**
	 * sched_poll - called each poll loop
	 * @now: current time in microseconds
	 * @idle_cnt: the number of cores that went idle
	 * @idle: an edge-triggered bitmap of cores that have become idle
	 *
	 * Happens right after all notifications. In general, the scheduler
	 * should make adjustments and allocate idle cores during this phase.
	 */
	void (*sched_poll)(uint64_t now, int idle_cnt, bitmap_ptr_t idle);
};


/*
 * Global variables
 */

DECLARE_BITMAP(sched_allowed_cores, NCPU);
unsigned int sched_siblings[NCPU];
unsigned int sched_dp_core;
unsigned int sched_ctrl_core;
unsigned int sched_linux_core;
/* per socket state */
struct socket {
	DEFINE_BITMAP(cores, NCPU);
};
struct socket socket_state[NNUMA];


/*
 * API for scheduler policy modules
 */

extern int sched_run_on_core(struct proc *p, unsigned int core);
extern int sched_idle_on_core(uint32_t mwait_hint, unsigned int core);

static inline int sched_threads_active(struct proc *p)
{
	return p->active_thread_count;
}

static inline int sched_threads_avail(struct proc *p)
{
	return p->thread_count - p->active_thread_count;
}


/*
 * Core iterators
 */

extern unsigned int sched_cores_tbl[NCPU];
extern int sched_cores_nr;
extern unsigned int sched_siblings_tbl[NCPU];
extern int sched_siblings_nr;

#define sched_for_each_allowed_core(core, tmp)			\
	for ((core) = sched_cores_tbl[0], (tmp) = 0;		\
	     (tmp) < sched_cores_nr &&				\
	     ({(core) = sched_cores_tbl[(tmp)]; true;});	\
	     (tmp)++)

#define sched_for_each_allowed_sibling(core, tmp)		\
	for ((core) = sched_siblings_tbl[0], (tmp) = 0;		\
	     (tmp) < sched_siblings_nr &&			\
	     ({(core) = sched_siblings_tbl[(tmp)]; true;});	\
	     (tmp)++)


/*
 * API for the rest of the IOkernel
 */

extern void sched_poll(void);
extern int sched_add_core(struct proc *p);
extern int sched_attach_proc(struct proc *p);
extern void sched_detach_proc(struct proc *p);


/*
 * Scheduler policies
 */

extern const struct sched_ops *sched_ops;
extern struct sched_ops simple_ops;
extern struct sched_ops mis_ops;
extern struct sched_ops numa_ops;
extern struct sched_ops ias_ops;
