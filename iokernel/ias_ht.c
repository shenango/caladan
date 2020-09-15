/*
 * ias_ht.c - the hyperthread subcontroller
 */

#include <stdlib.h>
#include <float.h>

#include <base/stddef.h>
#include <base/log.h>

#include "defs.h"
#include "sched.h"
#include "ksched.h"
#include "ias.h"

/* statistics */
uint64_t ias_ht_punish_count;
uint64_t ias_ht_relax_count;

/* a bitmap of cores that are currently punished */
DEFINE_BITMAP(ias_ht_punished_cores, NCPU);

/* per-core data for this subcontroller */
struct ias_ht_data ias_ht_percore[NCPU];

static bool ias_ht_punish_selfpair(struct ias_data *sd, unsigned int sib)
{
	/* don't move a thread that is already over its HT budget */
	if (ias_ht_budget_used(sib) >= 1.0f)
		return false;

	/* check if we can find a lane that isn't going to be punished */
	if (!ias_can_add_kthread(sd, true))
		return false;

	return true;
}

static void ias_ht_punish(struct ias_data *sd, unsigned int core)
{
	struct ias_data *sib_sd;
	unsigned int sib = sched_siblings[core];

	if (!bitmap_test(sd->reserved_cores, core))
		return;

	/* check if the core is already punished */
	if (bitmap_test(ias_ht_punished_cores, core) ||
	    bitmap_test(ias_ht_punished_cores, sib))
		return;

	sib_sd = cores[sib];

	/*
	 * Breaking up self-pairings can harm throughput, ensure that there is
	 * enough spare capacity to do so
	 */
	if (sib_sd == sd && !ias_ht_punish_selfpair(sd, sib))
		return;
	else if (sib_sd && sib_sd->is_lc && !ias_can_add_kthread(sd, false))
		return;

	/* idle the core, but mark it as in use by the process */
	if (ias_idle_placeholder_on_core(sd, sib)) {
		WARN();
		return;
	}

	/* mark the core as punished */
	ias_ht_punish_count++;
	sd->ht_punish_count++;
	bitmap_set(ias_ht_punished_cores, sib);

	if (sib_sd && sib_sd->is_lc)
		ias_add_kthread(sib_sd);
}

static void ias_ht_relax(struct ias_data *sd, unsigned int core)
{
	unsigned int sib = sched_siblings[core];

	/* check if core is already relaxed */
	if (!bitmap_test(ias_ht_punished_cores, sib))
		return;

	/* mark the core as relaxed */
	ias_ht_relax_count++;
	bitmap_clear(ias_ht_punished_cores, sib);

	/* mark the core as idle */
	WARN_ON(ias_idle_on_core(sib));
}

static float ias_ht_poll_one(unsigned int core, uint64_t cur_tsc)
{
	struct ias_ht_data *htd = &ias_ht_percore[core];
	struct ias_data *sd = cores[core];
	struct thread *th = sched_get_thread_on_core(core);
	uint64_t sgen, start_ts, delay_ts;
	float budget_used = 0;

	/* check if we might be able to punish the sibling's HT lane */
	if (sd && sd->ht_punish_us > 0 && th != NULL) {
		/* update generation counters */
		sgen = ias_gen[core];

		/* relax once if scheduler gen changes */
		if (sgen == htd->sgen) {
			start_ts = ACCESS_ONCE(th->q_ptrs->run_start_tsc);
			delay_ts = cur_tsc - MIN(cur_tsc, start_ts);
			budget_used = (float)delay_ts * sd->ht_punish_tsc_inv;
		}
		htd->sgen = sgen;
	}

	htd->budget_used = budget_used;
	return budget_used;
}

struct tarr {
	float budget_used;
	unsigned int core;
};

static int cmptarr(const void *p1, const void *p2)
{
	const struct tarr *t1 = p1;
	const struct tarr *t2 = p2;

	if (t1->budget_used < t2->budget_used) {
		return 1;
	} else if (t1->budget_used > t2->budget_used) {
		return -1;
	} else {
		return 0;
	}
}

/**
 * ias_ht_poll - runs the hyperthread controller
 */
void ias_ht_poll(void)
{
	struct tarr arr[NCPU];
	unsigned int core, tmp, num = 0;
	uint64_t now_tsc = rdtsc();

	/* loop over cores to update service times */
	sched_for_each_allowed_core(core, tmp) {
		arr[num].budget_used = ias_ht_poll_one(core, now_tsc);
		arr[num++].core = core;
	}

	/* sort by longest service time */
	qsort(arr, num, sizeof(struct tarr), cmptarr);

	/* adjust which cores are punished and relaxed */
	for (tmp = 0; tmp < num; tmp++) {
		const struct tarr *ta = &arr[tmp];
		struct ias_data *sd = cores[ta->core];

		if (sd && sd->ht_punish_us > 0 &&
		    ta->budget_used >= 1.0) {
			ias_ht_punish(sd, ta->core);
		} else {
			ias_ht_relax(sd, ta->core);
		}
	}
}

/**
 * ias_ht_relinquish_core - try to unpunished a core to alleviate congestion
 * @sd: the task that is congested
 *
 * Returns a core to allocate or NCPU if no core is available.
 */
unsigned int ias_ht_relinquish_core(struct ias_data *sd)
{
	struct ias_ht_data *htd;
	float best_budget = FLT_MAX;
	unsigned int core, tmp, best_core = NCPU;

	if (!sd->ht_punish_us)
		return NCPU;

	sched_for_each_allowed_core(core, tmp) {
		if (cores[core] != sd)
			continue;
		if (!bitmap_test(ias_ht_punished_cores, core))
			continue;

		htd = &ias_ht_percore[sched_siblings[core]];
		if (htd->budget_used < best_budget) {
			best_core = core;
			best_budget = htd->budget_used;
		}
	}

	/* relax a core if we found one */
	if (best_core != NCPU) {
		ias_ht_relax_count++;
		bitmap_clear(ias_ht_punished_cores, best_core);
		WARN_ON(ias_idle_on_core(best_core));
		return best_core;
	}

	return NCPU;
}
