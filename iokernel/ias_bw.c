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
float    bw_estimate;

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

static inline void ias_kick_core(int core)
{
	struct ias_data *sd = cores[core];
	sd->threads_limit = MAX(0,
				MIN(sd->threads_limit - 1,
				    sd->threads_active - 1));
	if (ias_add_kthread_on_core(core))
		ias_idle_on_core(core);
}

static void ias_bw_punish(void)
{
	struct ias_data *sd;
	unsigned int core, tmp;

	sd = ias_bw_choose_victim();
	if (!sd)
		return;

	sd->is_bwlimited = true;
	ias_count_bw_punish++;

	int kick_cnt = 0;
	int kick_thresh = IAS_KICK_OUT_THREAD_LIMIT_FACTOR * sd->threads_limit +
		((MAX(0, bw_estimate - IAS_BW_UPPER_LIMIT)) / IAS_KICK_OUT_BW_FACTOR);
	int sibling;

	// TODO: actually we need to spread kick_thresh among multiple sds
	// rather than a single sd, for example when all sds are single-threaded.
	sched_for_each_allowed_core(core, tmp) {
		if (cores[core] == sd) {
			ias_kick_core(core);
			kick_cnt++;
		}
		sibling = sched_siblings[core];
		if (cores[sibling] == sd) {
			ias_kick_core(sibling);
			kick_cnt++;
		}
		if (kick_cnt >= kick_thresh) {
			break;
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

	/* update the bandwidth estimate */
	barrier();
	tsc = rdtsc();
	barrier();
	cur_cas = get_cas_count_all();
	bw_estimate = (float)(cur_cas - last_cas) / (float)(tsc - last_tsc);
	last_cas = cur_cas;
	last_tsc = tsc;
	ias_count_bw_cur = bw_estimate;

	/* punish a process that is using too much bandwidth */
	if (bw_punish_triggered) {
		bw_punish_triggered = false;
		ias_bw_punish();
	} else {
		/* check if the bandwidth limit has been exceeded */
		if (bw_estimate >= IAS_BW_UPPER_LIMIT) {
			ias_bw_sample_pmc(PMC_LLC_MISSES);
			bw_punish_triggered = true;
		} else if (bw_estimate < IAS_BW_LOWER_LIMIT) {
			ias_bw_relax();
		}
	}
}
