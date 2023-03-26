/*
 * numa.c - a numa-aware scheduler policy
 */

#include <stdlib.h>
#include <string.h>

#include <base/cpu.h>
#include <base/log.h>
#include <base/stddef.h>

#include "defs.h"
#include "sched.h"

/* a list of processes that are waiting for more cores */
static LIST_HEAD(congested_procs);
/* a bitmap of all available cores that are currently idle */
static DEFINE_BITMAP(numa_idle_cores, NCPU);

#define NRECENT 4
struct numa_data {
	struct proc		*p;
	unsigned int		is_congested:1;
	struct list_node	congested_link;
	bool			waking;
	uint64_t		qdelay_us;

	/* thread usage limits */
	int			threads_guaranteed;
	int			threads_max;
	int			threads_active;
	int			preferred_socket;

	/* recently used cores */
	unsigned int		recent_cores[NRECENT];
};

static bool numa_proc_is_preemptible(struct numa_data *cursd,
				struct numa_data *nextsd)
{
	return cursd->threads_active > cursd->threads_guaranteed &&
	       nextsd->threads_active < nextsd->threads_guaranteed;
}

/* the current process running on each core */
static struct numa_data *cores[NCPU];

/* the history of processes running on each core */
#define NHIST	4
static struct numa_data *hist[NCPU][NHIST];

/* clean up the state of core to reflect that its proc is no lonoger running
   on it */
static void numa_cleanup_core(unsigned int core)
{
	struct numa_data *sd = cores[core];
	int i;

	if (!sd)
		return;

	if (cores[core])
		cores[core]->threads_active--;
	cores[core] = NULL;
	for (i = NHIST-1; i > 0; i--)
		hist[core][i] = hist[core][i - 1];
	hist[core][0] = sd;

	for (i = NRECENT-1; i > 0; i--)
		sd->recent_cores[i] = sd->recent_cores[i-1];
	sd->recent_cores[0] = core;
}

static void numa_mark_congested(struct numa_data *sd)
{
	if (sd->is_congested)
		return;
	sd->is_congested = true;
	list_add(&congested_procs, &sd->congested_link);
}

static void numa_unmark_congested(struct numa_data *sd)
{
	if (!sd->is_congested)
		return;
	sd->is_congested = false;
	list_del_from(&congested_procs, &sd->congested_link);
}

static int numa_attach(struct proc *p, struct sched_spec *cfg)
{
	struct numa_data *sd;
	int i;

	/* TODO: validate if there are enough cores available for @cfg */

	sd = malloc(sizeof(*sd));
	if (!sd)
		return -ENOMEM;

	memset(sd, 0, sizeof(*sd));
	sd->p = p;
	sd->threads_guaranteed = cfg->guaranteed_cores;
	sd->threads_max = cfg->max_cores;
	sd->threads_active = 0;
	sd->waking = false;
	sd->preferred_socket = cfg->preferred_socket;
	sd->qdelay_us = cfg->qdelay_us;
	for (i = 0; i < NRECENT; i++)
		sd->recent_cores[i] = sched_cores_tbl[0];
	p->policy_data = (unsigned long)sd;
	return 0;
}

static void numa_detach(struct proc *p)
{
	struct numa_data *sd = (struct numa_data *)p->policy_data;
	int i, j;

	numa_unmark_congested(sd);

	for (i = 0; i < NCPU; i++) {
		if (cores[i] == sd)
			cores[i] = NULL;
		for (j = 0; j < NHIST; j++) {
			if (hist[i][j] == sd)
				hist[i][j] = NULL;
		}
	}

	free(sd);
}

static int numa_run_kthread_on_core(struct proc *p, unsigned int core)
{
	struct numa_data *sd = (struct numa_data *)p->policy_data;
	int ret;

	/*
	 * WARNING: A kthread could be stuck waiting to detach and thus
	 * temporarily unavailable even if it is no longer assigned to a core.
	 * We check with the scheduler layer here to catch such a race
	 * condition.  In this sense, applications can get new cores more
	 * quickly if they yield promptly when requested.
	 */
	if (sched_threads_avail(p) == 0)
		return -EBUSY;

	ret = sched_run_on_core(p, core);
	if (ret)
		return ret;

	numa_cleanup_core(core);
	cores[core] = sd;
	bitmap_clear(numa_idle_cores, core);
	sd->threads_active++;
	return 0;
}

/**
 * numa_choose_core_in_subset - returns a core from within core_subset
 * or NCPU if none exists.
 */
static inline unsigned int numa_choose_core_in_subset(struct numa_data *sd,
						unsigned long *core_subset)
{
	int i;
	unsigned int core;
	DEFINE_BITMAP(idle_core_subset, NCPU);

	/* check for a previously used core (to improve locality) */
	for (i = 0; i < NRECENT; i++) {
		core = sd->recent_cores[i];
		if (!bitmap_test(core_subset, core))
			continue;

		if (cores[core] != sd && (cores[core] == NULL ||
						numa_proc_is_preemptible(cores[core], sd)))
			return core;


		if (cfg.noht)
			continue;

		/* sibling core has equally good locality */
		core = sched_siblings[core];
		if (!bitmap_test(core_subset, core))
			continue;

		if (cores[core] != sd && (cores[core] == NULL ||
						numa_proc_is_preemptible(cores[core], sd)))
			return core;
	}

	/* check for an idle core */
	bitmap_and(idle_core_subset, numa_idle_cores, core_subset, NCPU);
	core = bitmap_find_next_set(idle_core_subset, NCPU, 0);
	if (core != NCPU)
		return core;

	/* check for a preemptible core */
	bitmap_for_each_set(core_subset, NCPU, i) {
		if (cores[i] == sd)
			continue;

		if (cores[i] &&
			numa_proc_is_preemptible(cores[i], sd))
			return i;
	}

	return NCPU;
}

static unsigned int numa_choose_core(struct proc *p)
{
	struct numa_data *sd = (struct numa_data *)p->policy_data;
	unsigned int core, tmp;
	DEFINE_BITMAP(core_subset, NCPU);

	/* first try to find a matching active hyperthread */
	if (!cfg.noht) {
		sched_for_each_allowed_core(core, tmp) {
			unsigned int sib = sched_siblings[core];
			if (cores[core] != sd)
				continue;
			if (cores[sib] == sd || (cores[sib] != NULL &&
				!numa_proc_is_preemptible(cores[sib], sd)))
				continue;

			return sib;
		}
	}

	/* then try to find a core on the preferred socket */
	bitmap_and(core_subset, sched_allowed_cores,
		socket_state[sd->preferred_socket].cores, NCPU);
	core = numa_choose_core_in_subset(sd, core_subset);
	if (core != NCPU)
		return core;

	/* finally try to find a core on any socket */
	if (numa_count > 1) {
		core = numa_choose_core_in_subset(sd, sched_allowed_cores);
		return core;
	}

	/* out of luck, couldn't find anything */
	return NCPU;
}

static int numa_add_kthread(struct proc *p)
{
	struct numa_data *sd = (struct numa_data *)p->policy_data;
	unsigned int core;

	if (sd->threads_active >= sd->threads_max)
		return -ENOENT;

	core = numa_choose_core(p);
	if (core == NCPU)
		return -ENOENT;

	return numa_run_kthread_on_core(p, core);
}

static int numa_notify_core_needed(struct proc *p)
{
	return numa_add_kthread(p);
}

static bool numa_notify_congested(struct proc *p, struct delay_info *delay)
{
	struct numa_data *sd = (struct numa_data *)p->policy_data;
	int ret;
	bool congested;

	/* detect congestion */
	congested = sd->qdelay_us == 0 ?
		        delay->standing_queue : delay->max_delay_us >= sd->qdelay_us;
	congested |= delay->parked_thread_busy;

	/* do nothing if we woke up a core during the last interval */
	if (sd->waking) {
		sd->waking = false;
		return false;
	}

	/* check if congested */
	if (!congested) {
		numa_unmark_congested(sd);
		return false;
	}

	/* do nothing if already marked as congested */
	if (sd->is_congested)
		return false;

	/* try to add an additional core right away */
	ret = numa_add_kthread(p);
	if (ret == 0)
		return false;

	/* otherwise mark the process as congested, cores can be added later */
	numa_mark_congested(sd);
	return false;
}

static struct numa_data *numa_choose_kthread(unsigned int core)
{
	struct numa_data *sd;
	int i;

	if (!cfg.noht) {
		/* first try to run the same process as the sibling */
		sd = cores[sched_siblings[core]];
		if (sd && sd->is_congested)
			return sd;
	}

	/* then try to find a congested process that ran on this core last */
	for (i = 0; i < NHIST; i++) {
		sd = hist[core][i];
		if (sd && sd->is_congested)
			return sd;

		if (cfg.noht)
			continue;

		/* the hyperthread sibling has equally good locality */
		sd = hist[sched_siblings[core]][i];
		if (sd && sd->is_congested)
			return sd;
	}

	/* then try to find any congested process */
	return list_top(&congested_procs, struct numa_data, congested_link);
}

static void numa_sched_poll(uint64_t now, int idle_cnt, bitmap_ptr_t idle)
{
	struct numa_data *sd;
	unsigned int core;

	if (idle_cnt == 0)
		return;

	bitmap_for_each_set(idle, NCPU, core) {
		if (cores[core] != NULL)
			numa_unmark_congested(cores[core]);
		numa_cleanup_core(core);
		sd = numa_choose_kthread(core);
		if (!sd) {
			bitmap_set(numa_idle_cores, core);
			continue;
		}

		if (unlikely(numa_run_kthread_on_core(sd->p, core))) {
			bitmap_set(numa_idle_cores, core);
			numa_mark_congested(sd);
		}
	}
}

struct sched_ops numa_ops = {
	.proc_attach		= numa_attach,
	.proc_detach		= numa_detach,
	.notify_congested	= numa_notify_congested,
	.notify_core_needed	= numa_notify_core_needed,
	.sched_poll		= numa_sched_poll,
};

/**
 * numa_init - initializes the numa scheduler policy
 *
 * Returns 0 (always successful).
 */
int numa_init(void)
{
	bitmap_or(numa_idle_cores, numa_idle_cores,
		  sched_allowed_cores, NCPU);
	return 0;
}
