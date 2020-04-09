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

static void ias_ht_punish(struct ias_data *sd, unsigned int core)
{
	unsigned int sib = sched_siblings[core];
	bool idle = cores[sib] == NULL;

	/* check if the core is already punished */
	if (sd != cores[core] || bitmap_test(ias_ht_punished_cores, core) ||
	    bitmap_test(ias_ht_punished_cores, sib))
		return;

	/* don't preempt an LC task if we can't add back a different core */
	if (!idle && cores[sib]->is_lc && !ias_can_add_kthread(cores[sib]))
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
	if (!idle)
		ias_add_kthread(cores[sib]);

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

static void ias_ht_poll_one(struct ias_data *sd, struct thread *th,
			    uint64_t now_us)
{
	uint64_t rcugen, last_rcugen;
	ptrdiff_t idx = th - sd->p->threads;

	rcugen = ACCESS_ONCE(th->q_ptrs->rcu_gen);
	last_rcugen = sd->ht_last_rcugen[idx];

	/* check if we should relax this hyperthread lane */
	if (rcugen != last_rcugen) {
		sd->ht_last_rcugen[idx] = rcugen;
		sd->ht_last_us[idx] = now_us;
		ias_ht_relax(sd, th->core);
		return;
	}

	/* check if we should punish this hyperthread lane */
	if ((rcugen & 0x1) == 0x1 &&
	    now_us - sd->ht_last_us[idx] >= sd->ht_punish_us) {
		ias_ht_punish(sd, th->core);
	}
}

/**
 * ias_ht_poll - runs the hyperthread controller
 * now_us: the current time
 */
void ias_ht_poll(uint64_t now_us)
{
	struct ias_data *sd;
	struct proc *p;
	struct thread *th;
	int i;

	ias_for_each_proc(sd) {
		/* only LC tasks are eligible for HT control */
		if (!sd->is_lc)
			continue;
		/* skip tasks without an HT punish deadline */
		if (sd->ht_punish_us == 0)
			continue;

		/* check each thread */
		/* FIXME: can we constrain this to active threads? */
		p = sd->p;
		for (i = 0; i < p->thread_count; i++) {
			th = &p->threads[i];
			ias_ht_poll_one(sd, th, now_us);
		}
	}
}
