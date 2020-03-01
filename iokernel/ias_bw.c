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
float    ias_bw_estimate;
bool     ias_bw_punish_triggered;

/* a mask of the cores currently being sampled */
static DEFINE_BITMAP(ias_sampled_cores, NCPU);

static void ias_bw_sample_pmc(uint64_t sel)
{
	struct ias_data *sd;
	int core, sib, tmp;

	bitmap_init(ias_sampled_cores, NCPU, false);
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
			sd1->ok_bw_threads_monitored = 0;
			ksched_enqueue_pmc(sib, sel);
			bitmap_set(ias_sampled_cores, sib);
			continue;
		}

		if (sd2 && !ias_has_priority(sd2, sib) && (!sd1 ||
		    sd2->bw_threads_monitored <= sd1->bw_threads_monitored)) {
			sd2->bw_threads_monitored++;
			sd2->ok_bw_threads_monitored = 0;
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
			continue;
		}
		if (sd) {
			sd->bw_llc_misses += pmc;
			sd->ok_bw_threads_monitored++;
		}
	}

	ias_for_each_proc(sd) {
		if (sd->ok_bw_threads_monitored == 0)
			continue;
		float estimated_l3miss = (float)sd->bw_llc_misses /
					 (float)sd->ok_bw_threads_monitored *
					 (float)sd->threads_active;
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

static inline void ias_throttle_kthread_on_core(int core, uint64_t now_tsc)
{
        struct ias_data *sd = cores[core];
        sd->threads_limit = MAX(0,
                                MIN(sd->threads_limit - 1,
                                    sd->threads_active - 1));
        if (ias_add_kthread_on_core(core, now_tsc))
                ias_idle_on_core(core);
}

static int ias_bw_punish(void)
{
	struct ias_data *sd;
	unsigned int core, tmp;

	sd = ias_bw_choose_victim();
	if (!sd)
		return -EAGAIN;

	sd->is_bwlimited = true;
	ias_count_bw_punish++;

	int kick_cnt = 0;
	int kick_thresh = MAX(1, IAS_KICK_OUT_FACTOR * sd->threads_active);

	// TODO: actually we need to spread kick_thresh among multiple sds
	// rather than a single sd, for example when all sds are single-threaded.
	uint64_t now_tsc = rdtsc();
	sched_for_each_allowed_core(core, tmp) {
		if (cores[core] == sd) {
		        ias_throttle_kthread_on_core(core, now_tsc);
			kick_cnt++;
		}
		if (kick_cnt >= kick_thresh)
			break;
	}
	return 0;
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
	static uint64_t last_tsc = 0;
	static uint32_t last_cas = 0;
	uint64_t tsc;
	uint32_t cur_cas;

	/* update the bandwidth estimate */
	barrier();
	tsc = rdtsc();
	barrier();
	cur_cas = get_cas_count_all();
	ias_bw_estimate = (float)(cur_cas - last_cas) / (float)(tsc - last_tsc);
	last_cas = cur_cas;
	last_tsc = tsc;

	/* punish a process that is using too much bandwidth */
	if (ias_bw_punish_triggered) {
		ias_bw_punish_triggered =
			((ias_bw_estimate >= IAS_BW_UPPER_LIMIT) && ias_bw_punish());
	} else {
		/* check if the bandwidth limit has been exceeded */
		if (ias_bw_estimate >= IAS_BW_UPPER_LIMIT) {
			ias_bw_sample_pmc(PMC_LLC_MISSES);
			ias_bw_punish_triggered = true;
		} else if (ias_bw_estimate < IAS_BW_LOWER_LIMIT) {
			ias_bw_relax();
		}
	}
}
