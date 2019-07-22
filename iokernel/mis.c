/*
 * mis.c - a scheduler policy for microarchitectural interference
 */

#include <stdlib.h>
#include <string.h>

#include <base/stddef.h>
#include <base/log.h>

#include "defs.h"
#include "sched.h"
#include "ksched.h"
#include "pmc.h"

/* a list of all processes */
static LIST_HEAD(all_procs);
/* a list of processes that are waiting for more cores */
static LIST_HEAD(congested_procs);
/* a list of proccesses that are limited due to bandwidth overconsumption */
static LIST_HEAD(bwlimited_procs);
/* a bitmap of all available cores that are currently idle */
static DEFINE_BITMAP(mis_idle_cores, NCPU);
/* a bitmap of all cores with performance counters being sampled */
static DEFINE_BITMAP(mis_sampled_cores, NCPU);

/* this performance counter measures LLC misses as a proxy for mem bandwidth */
#define PMC_LLC_MISSES (PMC_ARCH_LLC_MISSES | PMC_ESEL_USR | PMC_ESEL_OS | \
			PMC_ESEL_ANY | PMC_ESEL_ENABLE)

/* poll the global (system-wide) memory bandwidth over this time interval */
#define MIS_BW_MEASURE_INTERVAL	50
/* wait for performance counter results over this time interval */
#define MIS_BW_PUNISH_INTERVAL	10
/* punish processes consuming high bandwidth over this threshold */
#define MIS_BW_THRESHOLD	20000 /* FIXME: should not be hard coded */

struct mis_data {
	struct proc		*p;
	unsigned int		is_congested:1;
	unsigned int		is_bwlimited:1;
	struct list_node	all_link;
	struct list_node	congested_link;
	struct list_node	bwlimited_link;

	/* thread usage limits */
	int			threads_guaranteed;/* the number promised */
	int			threads_max;	/* the most possible */
	int			threads_limit;	/* the most allowed */
	int			threads_active;	/* the number active */

	/* congestion info */
	float			load;
	uint64_t		standing_queue_us;

	/* bandwidth monitoring */
	int			threads_monitored;
	uint64_t		llc_misses;
};

static bool mis_proc_is_preemptible(struct mis_data *cursd,
				       struct mis_data *nextsd)
{
	return cursd->threads_active > cursd->threads_guaranteed &&
	       nextsd->threads_active < nextsd->threads_guaranteed;
}

/* the current process running on each core */
static struct mis_data *cores[NCPU];

/* the history of processes running on each core */
#define NHIST	4
static struct mis_data *hist[NCPU][NHIST];

static void mis_cleanup_core(unsigned int core)
{
	struct mis_data *sd = cores[core];
	int i;

	if (!sd)
		return;

	if (cores[core])
		cores[core]->threads_active--;
	cores[core] = NULL;
	for (i = 1; i < NHIST; i++)
		hist[core][i] = hist[core][i - 1];
	hist[core][0] = sd;
}

static void mis_mark_congested(struct mis_data *sd)
{
	if (sd->is_congested)
		return;
	sd->is_congested = true;
	list_add(&congested_procs, &sd->congested_link);
}

static void mis_unmark_congested(struct mis_data *sd)
{
	if (!sd->is_congested)
		return;
	sd->is_congested = false;
	list_del_from(&congested_procs, &sd->congested_link);
}

static int mis_attach(struct proc *p, struct sched_spec *cfg)
{
	struct mis_data *sd;

	/* TODO: validate if there are enough cores available for @cfg */

	sd = malloc(sizeof(*sd));
	if (!sd)
		return -ENOMEM;

	memset(sd, 0, sizeof(*sd));
	sd->p = p;
	sd->threads_guaranteed = cfg->guaranteed_cores;
	sd->threads_max = cfg->max_cores;
	sd->threads_limit = cfg->max_cores;
	sd->threads_active = 0;
	p->policy_data = (unsigned long)sd;
	list_add(&all_procs, &sd->all_link);
	return 0;
}

static void mis_detach(struct proc *p)
{
	struct mis_data *sd = (struct mis_data *)p->policy_data;
	int i, j;

	mis_unmark_congested(sd);
	list_del_from(&all_procs, &sd->all_link);
	if (sd->is_bwlimited)
		list_del_from(&bwlimited_procs, &sd->bwlimited_link);

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

static void mis_sample_pmc(uint64_t sel)
{
	struct mis_data *sd;
	int core, sib, tmp;

	bitmap_clear(mis_sampled_cores, NCPU);
	list_for_each(&all_procs, sd, all_link) {
		sd->threads_monitored = 0;
		sd->llc_misses = 0;
	}

	sched_for_each_allowed_sibling(core, tmp) {
		struct mis_data *sd1, *sd2;
		sib = sched_siblings[core];
		sd1 = cores[core];
		sd2 = cores[sib];

		if (!sd1 && !sd2)
			continue;
		if (sd1 &&
		    (!sd2 || sd1->threads_monitored < sd2->threads_monitored)) {
			sd1->threads_monitored++;
			ksched_enqueue_pmc(sib, sel);
			bitmap_set(mis_sampled_cores, sib);
		} else {
			sd2->threads_monitored++;
			ksched_enqueue_pmc(core, sel);
			bitmap_set(mis_sampled_cores, core);
		}
	}
}

static struct mis_data *mis_choose_bandwidth_victim(void)
{
	struct mis_data *sd, *victim = NULL;
	float highest_l3miss;
	uint64_t pmc;
	int i;

	bitmap_for_each_set(mis_sampled_cores, NCPU, i) {
		sd = cores[sched_siblings[i]];
		if (unlikely(!ksched_poll_pmc(i, &pmc))) {
			if (sd)
				sd->threads_monitored--;
			log_err_ratelimited("mis: pmc not ready");
			continue;
		}
		if (sd)
			sd->llc_misses += pmc;
	}

	list_for_each(&all_procs, sd, all_link) {
		float estimated_l3miss = (float)sd->llc_misses /
					 (float)sd->threads_monitored *
					 (float)sd->threads_active;
		log_info("mis: proc %d monitored %d L3Miss %f",
			 sd->p->pid, sd->threads_monitored, estimated_l3miss);
		if (sd->threads_limit == 0 ||
		    sd->threads_limit <= sd->threads_guaranteed)
			continue;
		if (!victim || estimated_l3miss > highest_l3miss) {
			highest_l3miss = estimated_l3miss;
			victim = sd;
		}
	}

	return victim;
}

static void mis_bandwidth_state_machine(uint64_t now)
{
	static bool bw_punish_triggered = false;
	static uint64_t last_tsc = 0, last_bw_measure_ts = 0, last_bw_punish_ts;
	static uint32_t last_cas = 0;
	uint64_t tsc;
	uint32_t cur_cas;
	float bw_estimate;

	/* punish a process that is using too much bandwidth */
	if (bw_punish_triggered &&
	    now - last_bw_punish_ts >= MIS_BW_PUNISH_INTERVAL) {
		struct mis_data *sd;

		bw_punish_triggered = false;
		sd = mis_choose_bandwidth_victim();
	}

	/* check if it's time to sample bandwidth */
	if (now - last_bw_measure_ts < MIS_BW_MEASURE_INTERVAL)
		return;

	/* update the bandwidth estimate */
	barrier();
	tsc = rdtsc();
	barrier();
	cur_cas = get_cas_count_all();
	bw_estimate = (float)(cur_cas - last_cas) / (float)(tsc - last_tsc);
	last_bw_measure_ts = now;
	last_cas = cur_cas;
	last_tsc = tsc;

	/* check if the bandwidth limit has been exceeded */
	if (bw_estimate > MIS_BW_THRESHOLD) {
		mis_sample_pmc(PMC_LLC_MISSES);
		bw_punish_triggered = true;
		last_bw_punish_ts = microtime();
	}

	log_info_ratelimited("bw estimate %f", bw_estimate);
}

static int mis_run_kthread_on_core(struct proc *p, unsigned int core)
{
	struct mis_data *sd = (struct mis_data *)p->policy_data;
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

	mis_cleanup_core(core);
	cores[core] = sd;
	bitmap_clear(mis_idle_cores, core);
	sd->threads_active++;
	return 0;
}

static int mis_idle_on_core(unsigned int core)
{
	int ret;

	ret = sched_idle_on_core(0, core);
	if (ret)
		return -EBUSY;

	mis_cleanup_core(core);
	cores[core] = NULL;
	bitmap_set(mis_idle_cores, core);
	return 0;
}

static unsigned int mis_choose_core(struct proc *p)
{
	struct mis_data *sd = (struct mis_data *)p->policy_data;
	struct thread *th;
	unsigned int core, tmp;

	/* first try to find a matching active hyperthread */
	sched_for_each_allowed_core(core, tmp) {
		unsigned int sib = sched_siblings[core];
		if (cores[core] != sd)
			continue;
		if (cores[sib] == sd || (cores[sib] != NULL &&
		    !mis_proc_is_preemptible(cores[sib], sd)))
			continue;
		return sib;
	}

	/* then try to find a previously used core (to improve locality) */
	list_for_each(&p->idle_threads, th, idle_link) {
		core = th->core;
		if (core >= NCPU)
			break;
		if (cores[core] != sd && (cores[core] == NULL ||
		    mis_proc_is_preemptible(cores[core], sd))) {
			return core;
		}

		/* sibling core has equally good locality */
		core = sched_siblings[th->core];
		if (cores[core] != sd && (cores[core] == NULL ||
		    mis_proc_is_preemptible(cores[core], sd))) {
			if (bitmap_test(sched_allowed_cores, core))
				return core;
		}
	}

	/* then look for any idle core */
	core = bitmap_find_next_set(mis_idle_cores, NCPU, 0);
	if (core != NCPU)
		return core;

	/* finally look for any preemptible core */
	sched_for_each_allowed_core(core, tmp) {
		if (cores[core] == sd)
			continue;
		if (cores[core] &&
		    mis_proc_is_preemptible(cores[core], sd))
			return core;
	}

	/* out of luck, couldn't find anything */
	return NCPU;
}

static int mis_add_kthread(struct proc *p)
{
	struct mis_data *sd = (struct mis_data *)p->policy_data;
	unsigned int core;

	if (sd->threads_active >= sd->threads_limit)
		return -ENOENT;

	core = mis_choose_core(p);
	if (core == NCPU)
		return -ENOENT;

	return mis_run_kthread_on_core(p, core);
}

static int mis_notify_core_needed(struct proc *p)
{
	return mis_add_kthread(p); 
}

#define EWMA_WEIGHT	0.1f

static void mis_update_congestion_info(struct mis_data *sd)
{
	struct congestion_info *info = sd->p->congestion_info;
	float instant_load;

	/* update the standing queue congestion microseconds */
	if (sd->is_congested)
		sd->standing_queue_us += IOKERNEL_POLL_INTERVAL;
	else
		sd->standing_queue_us = 0;
	ACCESS_ONCE(info->standing_queue_us) = sd->standing_queue_us;

	/* update the CPU load */
	/* TODO: handle using more than guaranteed cores */
	instant_load = (float)sd->threads_active / (float)sd->threads_limit;
	sd->load = sd->load * (1 - EWMA_WEIGHT) + instant_load * EWMA_WEIGHT;
	ACCESS_ONCE(info->load) = sd->load;
}

static void mis_notify_congested(struct proc *p, bitmap_ptr_t threads,
				 bitmap_ptr_t io)
{
	struct mis_data *sd = (struct mis_data *)p->policy_data;
	int ret;

	/* check if congested */
	if (bitmap_popcount(threads, NCPU) +
            bitmap_popcount(io, NCPU) == 0) {
		mis_unmark_congested(sd);
		goto done;
	}

	/* do nothing if already marked as congested */
	if (sd->is_congested)
		goto done;

	/* try to add an additional core right away */
	ret = mis_add_kthread(p);
	if (ret == 0)
		goto done;

	/* otherwise mark the process as congested, cores can be added later */
	mis_mark_congested(sd);

done:
	mis_update_congestion_info(sd);
}

static struct mis_data *mis_choose_kthread(unsigned int core)
{
	struct mis_data *sd;
	int i;

	/* first try to run the same process as the sibling */
	sd = cores[sched_siblings[core]];
	if (sd && sd->is_congested)
		return sd;

	/* then try to find a congested process that ran on this core last */
	for (i = 0; i < NHIST; i++) {
		sd = hist[core][i];
		if (sd && sd->is_congested)
			return sd;

		/* the hyperthread sibling has equally good locality */
		sd = hist[sched_siblings[core]][i];
		if (sd && sd->is_congested)
			return sd;
	}

	/* then try to find any congested process */
	return list_top(&congested_procs, struct mis_data, congested_link);
}

static void mis_sched_poll(uint64_t now, int idle_cnt, bitmap_ptr_t idle)
{
	struct mis_data *sd;
	unsigned int core;

	mis_bandwidth_state_machine(now);
	if (idle_cnt == 0)
		return;

	bitmap_for_each_set(idle, NCPU, core) {
		if (cores[core] != NULL)
			mis_unmark_congested(cores[core]);
		mis_cleanup_core(core);
		sd = mis_choose_kthread(core);
		if (!sd) {
			bitmap_set(mis_idle_cores, core);
			continue;
		}
		mis_unmark_congested(sd);
		if (unlikely(mis_run_kthread_on_core(sd->p, core))) {
			bitmap_set(mis_idle_cores, core);
			mis_mark_congested(sd);
		}
	}
}

struct sched_ops mis_ops = {
	.proc_attach		= mis_attach,
	.proc_detach		= mis_detach,
	.notify_congested	= mis_notify_congested,
	.notify_core_needed	= mis_notify_core_needed,
	.sched_poll		= mis_sched_poll,
};

/**
 * mis_init - initializes the mis scheduler policy
 *
 * Returns 0 (always successful).
 */
int mis_init(void)
{
	bitmap_or(mis_idle_cores, mis_idle_cores,
		  sched_allowed_cores, NCPU);
	return 0;
}
