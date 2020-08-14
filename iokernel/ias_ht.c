/*
 * ias_ht.c - the hyperthread subcontroller
 */

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
	if (bitmap_test(ias_ht_punished_cores, sib))
		return;

	/* don't preempt an LC task if we can't add back a different core */
	sib_sd = cores[sib];
	if (sib_sd && sib_sd->is_lc && !ias_can_add_kthread(sib_sd, true))
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

	/* try to allocate another core to the preempted task (best effort) */
	if (sib_sd)
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

	/* find something else to run on the core (or mark it idle) */
	if (ias_add_kthread_on_core(sib)) {
		if (ias_idle_on_core(sib)) {
			WARN();
			return;
		}
	}
}

static void ias_ht_poll_one(unsigned int core, uint64_t now_us)
{
	struct ias_ht_data *htd = &ias_ht_percore[core];
	struct ias_data *sd = cores[core];
	struct thread *th = sched_get_thread_on_core(core);
	uint64_t sgen, rgen;

	/* check if we might be able to punish the sibling's HT lane */
	if (sd && sd->is_lc && sd->ht_punish_us > 0 && th != NULL) {
		/* update generation counters */
		sgen = ias_gen[core];
		rgen = ACCESS_ONCE(th->q_ptrs->rcu_gen);
		if (htd->sgen != sgen || htd->rgen != rgen) {
			htd->sgen = sgen;
			htd->rgen = rgen;
			htd->last_us = now_us;
		}

		/* punish if deadline has expired and a thread is running */
		if (now_us - htd->last_us >= sd->ht_punish_us &&
		    (rgen & 0x1) == 0x1) {
			ias_ht_punish(sd, core);
			return;
		}
	}

	/* otherwise relax the sibling's HT lane */
	ias_ht_relax(sd, core);
}

/**
 * ias_ht_poll - runs the hyperthread controller
 * now_us: the current time
 */
void ias_ht_poll(uint64_t now_us)
{
	unsigned int core, tmp;

	/* loop over cores and check if each should be punished or relaxed */
	sched_for_each_allowed_core(core, tmp) {
		ias_ht_poll_one(core, now_us);
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
	unsigned int core, tmp;

	sched_for_each_allowed_core(core, tmp) {
		if (cores[core] != sd)
			continue;
		if (!bitmap_test(ias_ht_punished_cores, core))
			continue;

		/* mark the core as relaxed */
		ias_ht_relax_count++;
		bitmap_clear(ias_ht_punished_cores, core);
		return core;
	}

	return NCPU;
}
