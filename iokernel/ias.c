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

/* a list of all processes */
LIST_HEAD(all_procs);
/* a bitmap of all available cores that are currently idle */
static DEFINE_BITMAP(ias_idle_cores, NCPU);
/* a bitmap of all cores that tasks have reserved */
static DEFINE_BITMAP(ias_reserved_cores, NCPU);
/* the current process running on each core */
struct ias_data *cores[NCPU];
/* the generation number (to detect context switches on a core) */
uint64_t ias_gen[NCPU];
/* the current time in microseconds */
uint64_t now_us;

#ifdef IAS_DEBUG
static int owners[NCPU];
#endif

/* process that are currently congested sorted by current active thread count */
struct list_head congested_procs[NCPU + 1];
/* number of congested lc procs */
uint64_t congested_lc_procs_nr;

/*
 * make sure sd is in the right congestion list, should be called any time
 * sd->thread_active changes.
 */
static void ias_update_congestion(struct ias_data *sd)
{
	uint64_t congestion_rank;

	if (!sd->is_congested)
		return;

	congestion_rank = sd->threads_active;

	/* don't allow BE tasks to occupy rank 0 */
	congestion_rank += !sd->is_lc * !sd->threads_active;

	list_del(&sd->congested_link);
	list_add_tail(&congested_procs[congestion_rank], &sd->congested_link);
}

static void ias_mark_congested(struct ias_data *sd)
{
	uint64_t congestion_rank;

	if (sd->is_congested)
		return;

	congestion_rank = sd->threads_active;

	congested_lc_procs_nr += sd->is_lc;
	congestion_rank += !sd->is_lc * !sd->threads_active;

	sd->is_congested = true;
	list_add_tail(&congested_procs[congestion_rank], &sd->congested_link);
}

static void ias_unmark_congested(struct ias_data *sd)
{
	if (!sd->is_congested)
		return;

	congested_lc_procs_nr -= sd->is_lc;

	sd->is_congested = false;
	list_del(&sd->congested_link);
}

static void ias_cleanup_core(unsigned int core)
{
	struct ias_data *sd = cores[core];

	if (sd) {
		sd->last_run_us = now_us;
		sd->loc_last_us[core] = now_us;
		sd->threads_active--;
		ias_update_congestion(sd);
	}
	cores[core] = NULL;
}

static int ias_attach(struct proc *p, struct sched_spec *sched_cfg)
{
	struct ias_data *sd;
	int i, core, sib;

	/* validate parameters */
	if (!cfg.noht && sched_cfg->guaranteed_cores % 2 != 0) {
		log_err("ias: tried to attach proc with odd number of guaranteed cores");
		return -EINVAL;
	}

	/* allocate and initialize process state */
	sd = malloc(sizeof(*sd));
	if (!sd)
		return -ENOMEM;
	memset(sd, 0, sizeof(*sd));
	sd->p = p;
	sd->threads_guaranteed = sched_cfg->guaranteed_cores;
	sd->threads_max = sched_cfg->max_cores;
	sd->threads_limit = sched_cfg->max_cores;
	sd->is_lc = sched_cfg->priority == SCHED_PRIO_LC;
	if (sd->is_lc)
		sd->ht_punish_us = sched_cfg->ht_punish_us;
	sd->qdelay_us = sched_cfg->qdelay_us;
	sd->quantum_us = sched_cfg->quantum_us;
	sd->threads_active = 0;
	p->policy_data = (unsigned long)sd;
	list_add(&all_procs, &sd->all_link);

	/* reserve priority cores */
	i = sd->threads_guaranteed;
	sd->has_core_resv = i > 0;
	while (i > 0) {
		core = bitmap_find_next_cleared(ias_reserved_cores, NCPU, 0);
		if (core == NCPU)
			goto fail_reserve;

		bitmap_set(sd->reserved_cores, core);
		bitmap_set(ias_reserved_cores, core);
		i--;
#ifdef IAS_DEBUG
		owners[core] = p->pid;
#endif

		if (cfg.noht)
			continue;

		sib = sched_siblings[core];
		bitmap_set(sd->reserved_cores, sib);
		bitmap_set(ias_reserved_cores, sib);
		i--;
#ifdef IAS_DEBUG
		owners[sib] = p->pid;
#endif
	}

	return 0;

fail_reserve:
	log_err("ias: not enough cores available for reservation");
	list_del_from(&all_procs, &sd->all_link);
	bitmap_xor(ias_reserved_cores, ias_reserved_cores,
		   sd->reserved_cores, NCPU);
	free(sd);
	return -ENOENT;
}

static void ias_detach(struct proc *p)
{
	struct ias_data *sd = (struct ias_data *)p->policy_data;
	int i;

	list_del_from(&all_procs, &sd->all_link);
	bitmap_xor(ias_reserved_cores, ias_reserved_cores,
		   sd->reserved_cores, NCPU);

	for (i = 0; i < NCPU; i++) {
		if (cores[i] == sd)
			cores[i] = NULL;
	}

	ias_unmark_congested(sd);

	free(sd);
}

static int ias_run_kthread_on_core(struct ias_data *sd, unsigned int core)
{
	int ret;

	/*
	 * WARNING: A kthread could be stuck waiting to detach and thus
	 * temporarily unavailable even if it is no longer assigned to a core.
	 * We check with the scheduler layer here to catch such a race
	 * condition.  In this sense, applications can get new cores more
	 * quickly if they yield promptly when requested.
	 */
	if (sched_threads_avail(sd->p) == 0)
		return -EBUSY;

	ret = sched_run_on_core(sd->p, core);
	if (ret)
		return ret;

	ias_cleanup_core(core);
	cores[core] = sd;
	ias_gen[core]++;
	bitmap_clear(ias_idle_cores, core);
	sd->threads_active++;
	sd->waking = true;
	ias_update_congestion(sd);
	return 0;
}

/**
 * ias_idle_placeholder_on_core - idle the core but count it as active for @sd
 * @sd: the task to account the core to
 * @core: the core to make idle
 *
 * Returns 0 if successful.
 */
int ias_idle_placeholder_on_core(struct ias_data *sd, unsigned int core)
{
	int ret;

	if (!bitmap_test(ias_idle_cores, core)) {
		ret = sched_idle_on_core(0, core);
		if (ret)
			return -EBUSY;
	}

	ias_cleanup_core(core);
	cores[core] = sd;
	ias_gen[core]++;
	bitmap_set(ias_idle_cores, core);
	sd->threads_active++;
	ias_update_congestion(sd);
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

	if (!bitmap_test(ias_idle_cores, core)) {
		ret = sched_idle_on_core(0, core);
		if (ret)
			return -EBUSY;
	}

	ias_cleanup_core(core);
	cores[core] = NULL;
	ias_gen[core]++;
	bitmap_set(ias_idle_cores, core);
	return 0;
}

static bool ias_can_preempt_core(struct ias_data *sd, unsigned int core)
{
	struct ias_data *cur = cores[core];
	struct thread *th;

	/* any task can take idle cores */
	if (cur == NULL)
		return true;

	/* can't preempt if the current task is the same task */
	if (sd == cur)
		return false;
	/* can't preempt if the current task reserved this core */
	if (cur->has_core_resv && bitmap_test(cur->reserved_cores, core))
		return false;
	/* BE tasks can't preempt LC tasks */
	if (cur->is_lc && !sd->is_lc)
		return false;
	/* can't preempt a task that will be using <= burst cores */
	if (cur->is_lc == sd->is_lc &&
	    cur->threads_active - cur->threads_guaranteed <=
	    sd->threads_active - sd->threads_guaranteed + 1) {

		/* preempt a BE task running for greater than IAS_QUANTA_US */
		if (!cur->is_lc && sd->threads_active == 0) {
			th = sched_get_thread_on_core(core);
			if (th && th->metrics.kthread_elapsed_us >= IAS_QUANTA_US)
				return true;
		}

		return false;
	}

	/* all conditions passed, can preempt */
	return true;
}

/**
 * ias_core_score - estimates the amount of cache locality in the core's cache
 * @sd: the process to check
 * @core: the core to check
 * @now_us: the current time in microseconds
 *
 * Returns a locality score, higher is better.
 */
static float ias_locality_score(struct ias_data *sd, unsigned int core)
{
	uint64_t delta_us;
	unsigned int sib = sched_siblings[core];

	/* if the sibling is already running the process, locality is perfect */
	if (!cfg.noht && cores[sib] == sd)
		return 1.0f;

	/* avoid checking loc_last_us array for very cold procs */
	if (now_us - sd->last_run_us > IAS_LOC_EVICTED_US)
		return 0.0f;

	delta_us = now_us - MAX(sd->loc_last_us[core],
				cfg.noht ? 0 : sd->loc_last_us[sib]);
	if (delta_us >= IAS_LOC_EVICTED_US)
		return 0.0f;
	return (float)(IAS_LOC_EVICTED_US - delta_us) / IAS_LOC_EVICTED_US;
}

static float ias_core_score(struct ias_data *sd, unsigned int core)
{
	float score;
	struct ias_data *sib_task;

	score = ias_locality_score(sd, core);

	if (sd->has_core_resv && bitmap_test(sd->reserved_cores, core))
		score += 7.0f;

	if (cfg.noht)
		return score;

	sib_task = cores[sched_siblings[core]];
	if (!cfg.ias_prefer_selfpair && sib_task != sd) {
		score += 3.0f;
		if (sib_task == NULL && cores[core] == NULL)
			score += 2.0f;
	}

	/* bias in favor of cores whose siblings are most under HT budget */
	score += 1.0 - MIN(1.0, ias_ht_budget_used(sched_siblings[core]));

	return score;
}

static unsigned int ias_choose_core(struct ias_data *sd)
{
	unsigned int core, best_core = NCPU, tmp;
	float score, best_score = -1.0f;

	sched_for_each_allowed_core(core, tmp) {
		/* check if this process can preempt this core */
		if (!ias_can_preempt_core(sd, core))
			continue;

		/* try to estimate how good this core is for the process */
		score = ias_core_score(sd, core);
		if (score > best_score) {
			best_score = score;
			best_core = core;
		}
	}

	return best_core;
}

/**
 * ias_can_add_kthread - checks if a core can be added to a process
 * @sd: the process to check
 * @ignore_ht_punish_cores: exclude cores that may be pending HT punishment
 * *
 * Returns true if a core can be added.
 */
bool ias_can_add_kthread(struct ias_data *sd, bool ignore_ht_punish_cores)
{
	unsigned int core, tmp;

	sched_for_each_allowed_core(core, tmp) {
		if (!ias_can_preempt_core(sd, core))
			continue;
		if (!cfg.noht && ignore_ht_punish_cores &&
		    ias_ht_budget_used(sched_siblings[core]) >= 1.0f)
			continue;
		return true;
	}

	return false;
}

int ias_add_kthread(struct ias_data *sd)
{
	unsigned int core;

	/* check if we're constrained by the thread limit */
	if (sd->threads_active >= sd->threads_limit) {
		if (!cfg.noht) {
			core = ias_ht_relinquish_core(sd);
			if (core != NCPU)
				goto done;
		}
		return -ENOENT;
	}

	/* choose the best core to run the process on */
	core = ias_choose_core(sd);
	if (core == NCPU) {
		if (!cfg.noht)
			core = ias_ht_relinquish_core(sd);
		if (core == NCPU)
			return -ENOENT;
	}

done:
	/* finally, wake up the thread on the chosen core */
	return ias_run_kthread_on_core(sd, core);
}

static bool ias_should_add_kthread_now(struct ias_data *sd)
{
	return congested_lc_procs_nr == 0;
}

static int ias_notify_core_needed(struct proc *p)
{
	int ret;
	struct ias_data *sd = (struct ias_data *)p->policy_data;

	if (ias_should_add_kthread_now(sd)) {
		ret = ias_add_kthread(sd);
		if (!ret)
			return 0;
	}

	ias_mark_congested(sd);
	return 0;
}

static bool ias_can_unpoll(struct ias_data *sd)
{
	bool is_congested = sd->is_congested;
	assert(is_congested);
	/* this process is starved and in the high priority waiting list */
	return sd->threads_active == 0;
}

static bool ias_notify_congested(struct proc *p, struct delay_info *delay)
{
	struct ias_data *sd = (struct ias_data *)p->policy_data;
	int ret;
	bool congested;

	/* detect congestion */
	congested = sd->qdelay_us == 0 ?
		        delay->standing_queue : delay->max_delay_us >= sd->qdelay_us;
	congested |= delay->parked_thread_busy;

	if (sd->is_lc && delay->parked_thread_busy)
		STAT_INC(PARKED_THREAD_BUSY_WAKE, 1);

	/* stop if there is no congestion */
	if (!congested) {
		ias_unmark_congested(sd);
		return false;
	}

	if (sd->waking) {
		sd->waking = false;
		return false;
	}

	if (sd->is_congested)
		return ias_can_unpoll(sd);

	if (ias_should_add_kthread_now(sd)) {
		/* try to add an additional core right away */
		ret = ias_add_kthread(sd);
		if (!ret)
			return false;
	}

	/* otherwise mark the process as congested, cores can be added later */
	ias_mark_congested(sd);
	return ias_can_unpoll(sd);
}

static int ias_kthread_score(struct ias_data *sd, int core)
{
	bool has_resv = sd->has_core_resv;
	bool is_lc = sd->is_lc;
	int burst_score;

	burst_score = sd->threads_active - sd->threads_guaranteed;
	burst_score = MAX(burst_score, 0);
	burst_score = NCPU - burst_score;

	if (has_resv)
		has_resv = bitmap_test(sd->reserved_cores, core);

	return has_resv * NCPU * 4 + is_lc * NCPU * 2 + burst_score;
}

static struct ias_data *ias_choose_kthread(unsigned int core)
{
	struct ias_data *sd, *best_sd = NULL;
	int rank, score, best_score = -1;

	/* feed starving LCs in a FIFO order */
	list_for_each(&congested_procs[0], sd, congested_link) {
		/* check if we're constrained by the thread limit */
		if (sd->threads_active >= sd->threads_limit)
			continue;

		return sd;
	}

	/* check remaining congested procs */
	for (rank = 1; rank < sched_cores_nr + 1; rank++) {
		list_for_each(&congested_procs[rank], sd, congested_link) {
			/* check if we're constrained by the thread limit */
			if (sd->threads_active >= sd->threads_limit)
				continue;

			/* try to estimate how good this core is for the process */
			score = ias_kthread_score(sd, core);
			if (score > best_score) {
				best_score = score;
				best_sd = sd;
			}
		}

		if (best_sd)
			return best_sd;
	}

	return NULL;
}

/**
 * ias_add_kthread_on_core - pick a process and wake it on a core
 * @core: the core to schedule on
 *
 * Returns 0 if successful.
 */
int ias_add_kthread_on_core(unsigned int core)
{
	struct ias_data *sd;
	int ret;

	sd = ias_choose_kthread(core);
	if (unlikely(!sd))
		return -ENOENT;

	ret = ias_run_kthread_on_core(sd, core);
	if (unlikely(ret))
		return ret;

	return 0;
}

#ifdef IAS_DEBUG
static void ias_print_debug_info(void)
{
	bool printed[NCPU];
	struct ias_data *sd;
	int core, sib;
	uint64_t now = rdtsc();

	ias_for_each_proc(sd) {
		log_info("PID %d: %s%s ACTIVE %d, LIMIT %d, MAX %d",
			 sd->p->pid,
			 sd->is_congested ? "C" : "_",
			 sd->is_bwlimited ? "B" : "_",
			 sd->threads_active, sd->threads_limit,
			 sd->threads_max);
	}

	log_info("tsc %lu bw_cur %f bw_punish %ld bw_relax %ld bw_sample_failures %ld "
		 "bw_sample_aborts %ld",
		 now, ias_bw_estimate * ias_bw_estimate_multiplier, ias_bw_punish_count,
		 ias_bw_relax_count, ias_bw_sample_failures, ias_bw_sample_aborts);
	log_info("tsc %lu ht_punish %ld ht_relax %ld", now, ias_ht_punish_count,
		 ias_ht_relax_count);
	log_info("tsc %lu ts_count %ld", now, ias_ts_yield_count);

	memset(printed, 0, sizeof(printed));
	bitmap_for_each_set(sched_allowed_cores, NCPU, core) {
		if (printed[core]) {
			continue;
		}
		sib = sched_siblings[core];
		int pid0 = cores[core] ? cores[core]->p->pid : -1;
		printed[core] = true;
		int pid1 = -1;

		if (!cfg.noht) {
			pid1 = cores[sib] ? cores[sib]->p->pid : -1;
			printed[sib] = true;
		}

		log_info("core %d, %d (owner: %d): pid %d, %d", core, sib,
			 owners[core], pid0, pid1);
	}
}
#endif

static void ias_sched_poll(uint64_t now, int idle_cnt, bitmap_ptr_t idle)
{
	static uint64_t last_us, last_core_ts_us;
#ifdef IAS_DEBUG
	static uint64_t debug_ts = 0;
#endif
	unsigned int core;

	now_us = now;

	/* mark cores idle */
	if (idle_cnt != 0)
		bitmap_or(ias_idle_cores, ias_idle_cores, idle, NCPU);

	/* try to allocate any idle cores */
	bitmap_for_each_set(ias_idle_cores, NCPU, core) {
		if (bitmap_test(ias_ht_punished_cores, core))
			continue;
		if (cores[core] != NULL)
			ias_unmark_congested(cores[core]);
		ias_cleanup_core(core);
		ias_add_kthread_on_core(core);
	}

	/* try to run the subcontroller polling stages */
	if (now - last_us >= IAS_POLL_INTERVAL_US) {
		last_us = now;
		if (!cfg.nobw)
			ias_bw_poll();
		if (!cfg.noht)
			ias_ht_poll();
		ias_ts_poll();
	}

	if (now - last_core_ts_us >= IAS_QUANTA_US) {
		last_core_ts_us = now;
		ias_core_ts_poll();
	}

#ifdef IAS_DEBUG
	if (now - debug_ts >= IAS_DEBUG_PRINT_US) {
		debug_ts = now;
		ias_print_debug_info();
	}
#endif
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
	unsigned int i;

	bitmap_init(ias_reserved_cores, NCPU, true);
	bitmap_xor(ias_reserved_cores, ias_reserved_cores, sched_allowed_cores,
		   NCPU);

	for (i = 0; i < sched_cores_nr + 1; i++)
		list_head_init(&congested_procs[i]);

	return ias_bw_init();
}
