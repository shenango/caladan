/*
 * ias_bw.c - the bandwidth subcontroller
 */

#include <base/stddef.h>
#include <base/log.h>

#include "defs.h"
#include "sched.h"
#include "ksched.h"
#include "pmc.h"
#include "ias.h"

uint64_t ias_count_bw_punish;
uint64_t ias_count_bw_relax;
float    ias_count_bw_cur;

/* a mask of the cores currently being sampled */
static DEFINE_BITMAP(ias_sampled_cores, NCPU);

static void ias_bw_sample_pmc(uint64_t sel)
{
	struct ias_data *sd;
	int core, sib, tmp;

	bitmap_clear(ias_sampled_cores, NCPU);
	ias_for_each_proc(sd) {
		sd->bw_threads_monitored = 0;
		sd->bw_llc_misses = 0;
	}

	sched_for_each_allowed_sibling(core, tmp) {
		struct ias_data *sd1, *sd2;
		sib = sched_siblings[core];
		sd1 = cores[core];
		sd2 = cores[sib];

		if (!sd1 && !sd2)
			continue;

		if (sd1 && !ias_has_priority(sd1, core) && (!sd2 ||
		    sd1->bw_threads_monitored <= sd2->bw_threads_monitored)) {
			sd1->bw_threads_monitored++;
			ksched_enqueue_pmc(sib, sel);
			bitmap_set(ias_sampled_cores, sib);
			continue;
		}

		if (sd2 && !ias_has_priority(sd2, sib) && (!sd1 ||
		    sd2->bw_threads_monitored <= sd1->bw_threads_monitored)) {
			sd2->bw_threads_monitored++;
			ksched_enqueue_pmc(core, sel);
			bitmap_set(ias_sampled_cores, core);
		}
	}
}

static struct ias_data *ias_bw_choose_victim(void)
{
	struct ias_data *sd, *victim = NULL;
	float highest_l3miss;
	uint64_t pmc;
	int i;

	bitmap_for_each_set(ias_sampled_cores, NCPU, i) {
		sd = cores[sched_siblings[i]];
		if (unlikely(!ksched_poll_pmc(i, &pmc))) {
			if (sd)
				sd->bw_threads_monitored--;
			continue;
		}
		if (sd)
			sd->bw_llc_misses += pmc;
	}

	ias_for_each_proc(sd) {
		float estimated_l3miss = (float)sd->bw_llc_misses /
					 (float)sd->bw_threads_monitored *
					 (float)sd->threads_active;

		if (sd->bw_threads_monitored == 0)
			continue;
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

static void ias_bw_punish(void)
{
	struct ias_data *sd;
	unsigned int core, tmp;

	sd = ias_bw_choose_victim();
	if (!sd)
		return;

	sd->threads_limit = MIN(sd->threads_limit - 1,
				sd->threads_active - 1);
	sd->is_bwlimited = true;
	ias_count_bw_punish++;

	/* first prefer lone hyperthreads */
	sched_for_each_allowed_core(core, tmp) {
		if (cores[core] == sd &&
		    cores[sched_siblings[core]] != sd) {
			if (ias_add_kthread_on_core(core))
				ias_idle_on_core(core);
			return;
		}
	}

	/* then try any core */
	sched_for_each_allowed_core(core, tmp) {
		if (cores[core] == sd) {
			if (ias_add_kthread_on_core(core))
				ias_idle_on_core(core);
			return;
		}
	}
}

static void ias_bw_relax(void)
{
	struct ias_data *sd;

	ias_for_each_proc(sd) {
		if (!sd->is_bwlimited)
			continue;

		sd->threads_limit++;
		if (sd->threads_limit >= sd->threads_max)
			sd->is_bwlimited = false;
		ias_count_bw_relax++;
		break;
	}
}

void ias_bw_poll(uint64_t now_us)
{
	static bool bw_punish_triggered = false;
	static uint64_t last_tsc = 0;
	static uint32_t last_cas = 0;
	uint64_t tsc;
	uint32_t cur_cas;
	float bw_estimate;

	/* punish a process that is using too much bandwidth */
	if (bw_punish_triggered) {
		bw_punish_triggered = false;
		ias_bw_punish();

		/* reset counters to detect the new bw more quickly */
		barrier();
		last_tsc = rdtsc();
		barrier();
		last_cas = get_cas_count_all();
		return;
	}

	/* update the bandwidth estimate */
	barrier();
	tsc = rdtsc();
	barrier();
	cur_cas = get_cas_count_all();
	bw_estimate = (float)(cur_cas - last_cas) / (float)(tsc - last_tsc);
	last_cas = cur_cas;
	last_tsc = tsc;
	ias_count_bw_cur = bw_estimate;

	/* check if the bandwidth limit has been exceeded */
	if (bw_estimate >= IAS_BW_UPPER_LIMIT) {
		ias_bw_sample_pmc(PMC_LLC_MISSES);
		bw_punish_triggered = true;
	} else if (bw_estimate <= IAS_BW_LOWER_LIMIT) {
		ias_bw_relax();
	}
}
