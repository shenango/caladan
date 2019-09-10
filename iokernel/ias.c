/*
 * ias.c - the Interference-Aware Scheduler (IAS) policy
 */

#include <stdlib.h>
#include <string.h>
#include <float.h>

#include <base/stddef.h>
#include <base/log.h>

#include "defs.h"
#include "sched.h"
#include "ksched.h"
#include "ias.h"

// #define IAS_DEBUG 1

/* a list of all processes */
LIST_HEAD(all_procs);
/* a bitmap of all available cores that are currently idle */
static DEFINE_BITMAP(ias_idle_cores, NCPU);
/* a bitmap of all cores that have been assigned to a LC priority process */
static DEFINE_BITMAP(ias_claimed_cores, NCPU);
/* used for calculating a unique index number */
static struct ias_data *ias_procs[IAS_NPROC];
/* used for calculating a unique index number */
static unsigned int ias_procs_nr;
/* the current process running on each core */
struct ias_data *cores[NCPU];
/* when does the core gets idled */
uint64_t cores_idle_tsc[NCPU];
/* the generation number for reschedules on each core */
uint64_t ias_gen[NCPU];
/* the current time in microseconds */
static uint64_t now_us;

#ifdef IAS_DEBUG
int owners[NCPU];
#endif

static void ias_cleanup_core(unsigned int core)
{
	struct ias_data *sd = cores[core];

	if (sd) {
		sd->loc_last_us[core] = now_us;
		sd->threads_active--;
	}
	cores[core] = NULL;
}

static int ias_attach(struct proc *p, struct sched_spec *cfg)
{
	struct ias_data *sd;
	int i, core, sib;

	/* validate parameters */
	if (ias_procs_nr >= IAS_NPROC)
		return -ENOENT;
	if (cfg->guaranteed_cores % 2 != 0)
		return -EINVAL;

	/* allocate and initialize process state */
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

	/* reserve priority cores */
	i = sd->threads_guaranteed;
	while (i > 0) {
		core = bitmap_find_next_cleared(ias_claimed_cores, NCPU, 0);
		if (core == NCPU)
			goto fail_reserve;

		sib = sched_siblings[core];
#ifdef IAS_DEBUG
		owners[core] = owners[sib] = p->pid;
#endif
		bitmap_set(sd->claimed_cores, core);
		bitmap_set(ias_claimed_cores, core);
		bitmap_set(sd->claimed_cores, sib);
		bitmap_set(ias_claimed_cores, sib);
		i -= 2;
	}

	/* reserve a unique index */
	for (i = 0; i < ias_procs_nr; i++) {
		if (ias_procs[i] == NULL) {
			sd->idx = i;
			ias_procs[i] = sd;
			return 0;
		}
	}
	sd->idx = ias_procs_nr;
	ias_procs[ias_procs_nr++] = sd;
	return 0;

fail_reserve:
	list_del_from(&all_procs, &sd->all_link);
	bitmap_xor(ias_claimed_cores, ias_claimed_cores,
		   sd->claimed_cores, NCPU);
	free(sd);
	return -ENOENT;
}

static void ias_detach(struct proc *p)
{
	struct ias_data *sd = (struct ias_data *)p->policy_data;
	int i;

	ias_procs[sd->idx] = NULL;
	if (sd->idx == ias_procs_nr)
		ias_procs_nr--;

	list_del_from(&all_procs, &sd->all_link);

	for (i = 0; i < NCPU; i++) {
		if (cores[i] == sd)
			cores[i] = NULL;
	}
	bitmap_xor(ias_claimed_cores, ias_claimed_cores,
		   sd->claimed_cores, NCPU);

	free(sd);
}

static int ias_run_kthread_on_core(struct proc *p, unsigned int core)
{
	struct ias_data *sd = (struct ias_data *)p->policy_data;
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

	ias_cleanup_core(core);
	sd->ht_start_running_tsc[core] = rdtsc();
	cores[core] = sd;
	ias_gen[core]++;
	bitmap_clear(ias_idle_cores, core);
	sd->threads_active++;
	return 0;
}

/**
 * ias_idle_on_core - evict the current process and idle the core
 * @core: the core to make idle
 *
 * Returns 0 if successful.
 */
int ias_idle_on_core(unsigned int core)
{
	int ret;

	ret = sched_idle_on_core(0, core);
	if (ret)
		return -EBUSY;

	ias_cleanup_core(core);
	cores[core] = NULL;
	cores_idle_tsc[core] = rdtsc();
	ias_gen[core]++;
	bitmap_set(ias_idle_cores, core);
	return 0;
}

static float ias_calculate_score(struct ias_data *sd, unsigned int core,
				 uint64_t now_tsc)
{
	float score, ht_score;
	unsigned int sib;	

	/* occasionally we choose a random pairing */
	static int rr_cnt = 0;
	if (rr_cnt++ == IAS_HT_RANDOM_PAIRING_CNT) {
		rr_cnt = 0;
		return FLT_MAX;
	}

	if (is_banned(sd, cores[sched_siblings[core]], now_tsc))
		return -FLT_MAX;

	/* try to estimate how well the core and process pair together */
	score = ias_has_priority(sd, core) ? IAS_PRIORITY_WEIGHT : 0.0;
	score += ias_loc_score(sd, core, now_us);
	
	sib = sched_siblings[core];
	ht_score = ias_ht_pairing_score(sd, cores[sib]);
	
	/* encourage to use a full HT pair when the IPC degradation is acceptable */
	ht_score += cores[sib] ? GET_MAX_IPC_DEGRADE_RATIO(sd) : 0;

	return score + IAS_HT_WEIGHT * ht_score;
}

static unsigned int ias_choose_core(struct ias_data *sd, bool lc)
{
	unsigned int core, best_core = NCPU, tmp;
	float score, best_score = 0;
	uint64_t now_tsc = rdtsc();

	sched_for_each_allowed_core(core, tmp) {
		if (lc) {
			/* LC tasks have reserved cores */
			if (!ias_has_priority(sd, core))
				continue;

			/* can't choose a core we're already running on */
			if (cores[core] == sd)
				continue;
		} else {
			/* BE tasks can only take idle cores */
			if (cores[core] != NULL)
				continue;
		}

		/* try to estimate how good this core is for the process */
		score = ias_calculate_score(sd, core, now_tsc);
		if (score > best_score) {
			best_score = score;
			best_core = core;
		}
	}

	return best_core;
}

static int ias_add_kthread(struct proc *p)
{
	struct ias_data *sd = (struct ias_data *)p->policy_data;
	unsigned int core;

	/* check if we're constrained by the thread limit */
	if (sd->threads_active >= sd->threads_limit)
		return -ENOENT;

	/* choose the best core to run the process on */
	core = ias_choose_core(sd, is_lc(sd));
	if (core == NCPU)
		return -ENOENT;

	/* finally, wake up the thread on the chosen core */
	return ias_run_kthread_on_core(p, core);
}

static int ias_notify_core_needed(struct proc *p)
{
	return ias_add_kthread(p);
}

static void ias_notify_congested(struct proc *p, bitmap_ptr_t threads,
				 bitmap_ptr_t io)
{
	struct ias_data *sd = (struct ias_data *)p->policy_data;
	int ret;

	/* check if congested */
	if (bitmap_popcount(threads, NCPU) +
            bitmap_popcount(io, NCPU) == 0) {
		sd->is_congested = false;
		return;
	}

	/* try to add an additional core right away */
	ret = ias_add_kthread(p);
	if (!ret)
		return;

	/* otherwise mark the process as congested, cores can be added later */
	sd->is_congested = true;
}

static struct ias_data *ias_choose_kthread(unsigned int core, uint64_t now_tsc)
{
	struct ias_data *sd, *best_sd = NULL;
	float score, best_score = 0;

	ias_for_each_proc(sd) {
		/* only congested processes need more cores */
		if (!sd->is_congested)
			continue;
		/* check if we're constrained by the thread limit */
		if (sd->threads_active >= sd->threads_limit)
			continue;

		/* try to estimate how good this core is for the process */
		score = ias_calculate_score(sd, core, now_tsc);
		if (score > best_score) {
			best_score = score;
			best_sd = sd;
		}
	}

	return best_sd;
}

/**
 * ias_add_kthread_on_core - pick a process and wake it on a core
 * @core: the core to schedule on
 * @now_tsc: the current tsc
 *
 * Returns 0 if successful.
 */
int ias_add_kthread_on_core(unsigned int core, uint64_t now_tsc)
{
	struct ias_data *sd;
	int ret;

	sd = ias_choose_kthread(core, now_tsc);
	if (unlikely(!sd))
		return -ENOENT;

	ret = ias_run_kthread_on_core(sd->p, core);
	if (unlikely(ret))
		return ret;

	return 0;
}

#ifdef IAS_DEBUG
static void ias_print_debug_info(void)
{
	struct ias_data *sd, *sd2;
	int core, sib;
	static bool printed[NCPU];

	ias_for_each_proc(sd) {
		log_info("PID %d: %s%s ACTIVE %d, LIMIT %d, MAX %d, MAX IPC %f, UP IPC %f",
			 sd->p->pid,
			 sd->is_congested ? "C" : "_",
			 sd->is_bwlimited ? "B" : "_",
			 sd->threads_active, sd->threads_limit, sd->threads_max,
			 sd->ht_max_ipc, sd->ht_unpaired_ipc);
		ias_for_each_proc(sd2) {
			log_info("\tPID %dx%d: IPC %f", sd->p->pid,
				 sd2->p->pid, sd->ht_pairing_ipc[sd2->idx]);
		}
	}
	log_info("bw_cur %f bw_punish %ld bw_relax %ld bw_punish_triggered %d",
		 ias_bw_estimate, ias_count_bw_punish, ias_count_bw_relax,
		 ias_bw_punish_triggered);
	memset(printed, 0, sizeof(printed));
	bitmap_for_each_set(sched_allowed_cores, NCPU, core) {
		if (printed[core]) {
			continue;
		}
		sib = sched_siblings[core];
		printed[core] = printed[sib] = true;
		int pid0 = cores[core] ? cores[core]->p->pid : -1;
		int pid1 = cores[sib] ? cores[sib]->p->pid : -1;
		log_info("core %d, %d (owner: %d): pid %d, %d", core, sib,
			 owners[core], pid0, pid1);
	}
}
#endif

static void ias_sched_poll(uint64_t now, int idle_cnt, bitmap_ptr_t idle)
{
#ifdef IAS_DEBUG
	static uint64_t debug_ts = 0;
#endif
	static uint64_t bw_ts = 0, ht_ts = 0;
		
	unsigned int core;

	now_us = now;

	/* handle timeouts for various subcontrollers */
	if (now - bw_ts >= IAS_BW_POLL_US) {
		bw_ts = now;
		ias_bw_poll(now);
	}
	if (now - ht_ts >= IAS_HT_POLL_US) {
		ht_ts = now;
		ias_ht_poll(now);
	}
#ifdef IAS_DEBUG
	if (now - debug_ts >= IAS_DEBUG_PRINT_US) {
		debug_ts = now;
		ias_print_debug_info();
	}
#endif
	
	/* mark cores idle */
	if (idle_cnt != 0)
		bitmap_or(ias_idle_cores, ias_idle_cores, idle, NCPU);

	/* try to allocate any idle cores */
	uint64_t now_tsc = rdtsc();
	bitmap_for_each_set(ias_idle_cores, NCPU, core) {
		if (cores[core] != NULL)
			cores[core]->is_congested = false;
		ias_cleanup_core(core);
		ias_add_kthread_on_core(core, now_tsc);
	}
}

void ias_migrate_kthread_on_core(int core) {
	struct proc *p = cores[core]->p;
	ias_idle_on_core(core);
	ias_notify_core_needed(p);
}

struct sched_ops ias_ops = {
	.proc_attach		= ias_attach,
	.proc_detach		= ias_detach,
	.notify_congested	= ias_notify_congested,
	.notify_core_needed	= ias_notify_core_needed,
	.sched_poll		= ias_sched_poll,
};

/**
 * ias_init - initializes the ias scheduler policy
 *
 * Returns 0 (always successful).
 */
int ias_init(void)
{
	bitmap_init(ias_claimed_cores, true, NCPU);
	bitmap_xor(ias_claimed_cores, ias_claimed_cores, sched_allowed_cores,
		   NCPU);
	return 0;
}
