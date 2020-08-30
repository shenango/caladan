/*
 * ias_ht.c - the hyperthread subcontroller
 */

#include <stdlib.h>

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

struct ias_ht_data {
	/* the scheduler's generation counter */
	uint64_t	sgen;
	/* the runtime's generation counter */
	uint64_t	rgen;
	/* the last time these counters were updated */
	uint64_t	last_us;
};

/* per-core data for this subcontroller */
static struct ias_ht_data ias_ht_percore[NCPU];

static void ias_ht_punish(struct ias_data *sd, unsigned int core)
{
	struct ias_data *sib_sd;
	unsigned int sib = sched_siblings[core];

	/* check if the core is already punished */
	if (bitmap_test(ias_ht_punished_cores, core) ||
	    bitmap_test(ias_ht_punished_cores, sib))
		return;

	/* don't preempt an LC task if we can't add back a core, or queues have built up */
	sib_sd = cores[sib];
	if (sib_sd && sib_sd->is_lc && (sib_sd->current_qdelay_us >= sib_sd->ht_punish_us || !ias_can_add_kthread(sib_sd)))
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

static uint64_t ias_ht_poll_one(unsigned int core)
{
	struct ias_ht_data *htd = &ias_ht_percore[core];
	struct ias_data *sd = cores[core];
	struct thread *th = sched_get_thread_on_core(core);
	uint64_t sgen, rgen;

	/* check if we might be able to punish the sibling's HT lane */
	if (sd && sd->ht_punish_us > 0 && th != NULL) {
		/* update generation counters */
		sgen = ias_gen[core];
		rgen = ACCESS_ONCE(th->q_ptrs->rcu_gen);
		if (htd->sgen != sgen || htd->rgen != rgen) {
			htd->sgen = sgen;
			htd->rgen = rgen;
			htd->last_us = now_us;
		}
		/* skip if stuck in the runtime scheduler */
		if ((rgen & 0x1) != 0x1)
			return 0;

		return now_us - htd->last_us;
	}

	return 0;
}

struct tarr {
	uint64_t service_us;
	unsigned int core;
};

static int cmptarr(const void *p1, const void *p2)
{
	const struct tarr *t1 = p1;
	const struct tarr *t2 = p2;

	if (t1->service_us < t2->service_us) {
		return 1;
	} else if (t1->service_us > t2->service_us) {
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

	/* loop over cores to update service times */
	sched_for_each_allowed_core(core, tmp) {
		arr[num].service_us = ias_ht_poll_one(core);
		arr[num++].core = core;
	}

	/* sort by longest service time */
	qsort(arr, num, sizeof(struct tarr), cmptarr);

	/* adjust which cores are punished and relaxed */
	for (tmp = 0; tmp < num; tmp++) {
		const struct tarr *ta = &arr[tmp];
		struct ias_data *sd = cores[ta->core];

		if (sd && sd->ht_punish_us > 0 &&
		    ta->service_us >= sd->ht_punish_us) {
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
	uint64_t service_us, shortest_service_us = UINT64_MAX;
	unsigned int core, tmp, best_core;

	sched_for_each_allowed_core(core, tmp) {
		if (cores[core] != sd)
			continue;
		if (!bitmap_test(ias_ht_punished_cores, core))
			continue;

		htd = &ias_ht_percore[sched_siblings[core]];
		service_us = now_us - htd->last_us;
		if (service_us < shortest_service_us) {
			best_core = core;
			shortest_service_us = service_us;
		}
	}

	/* relax a core if we found one */
	if (shortest_service_us < UINT64_MAX) {
		ias_ht_relax(sd, best_core);
		return best_core;
	}

	return NCPU;
}
