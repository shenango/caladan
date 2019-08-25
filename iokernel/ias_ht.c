/*
 * ias_ht.c - the hyperthread subcontroller
 */

#include <base/stddef.h>
#include <base/log.h>

#include "defs.h"
#include "sched.h"
#include "ksched.h"
#include "pmc.h"
#include "ias.h"

#define WARMUP_US 10

static void ias_ht_poll_one(struct ias_data *sd, struct thread *th)
{
	float ipc, us, run_us;
	uint64_t last_tsc, last_instr, cur_tsc, cur_instr;
	int core, sib;

	core = th->core;
	sib = sched_siblings[core];

	/* calculate IPC and update counters */
	last_tsc = sd->ht_last_tsc[core];
	last_instr = sd->ht_last_instr[core];
	cur_tsc = th->q_ptrs->tsc;
	cur_instr = th->q_ptrs->instr;
	sd->ht_last_tsc[core] = cur_tsc;
	sd->ht_last_instr[core] = cur_instr;
	if (cur_tsc == last_tsc)
		return;
	if (ias_gen[core] != sd->ht_last_gen[core]) {
		sd->ht_last_gen[core] = ias_gen[core];
		return;
	}

	ipc = (float)(cur_instr - last_instr) / (float)(cur_tsc - last_tsc);
	if (ipc > 5.0 || ipc == 0)
		return; /* bad sample */
	us = (float)(cur_tsc - last_tsc) / (float)cycles_per_us;

	/* update unpaired IPC metrics */
	run_us = (float)(cur_tsc - cores[core]->ht_start_running_tsc[core]) /
		 cycles_per_us;
	if (run_us - us < WARMUP_US)
		return;
	if (!cores[sib]) {
		ias_ewma(&sd->ht_unpaired_ipc, ipc,
			 MIN(100.0, us) * IAS_EWMA_FACTOR);
		return;
	}

	/* update paired IPC metrics */
	run_us = (float)(cur_tsc - cores[sib]->ht_start_running_tsc[sib]) /
		 cycles_per_us;
	if (run_us - us < WARMUP_US)
		return;
	ias_ewma(&sd->ht_pairing_ipc[cores[sib]->idx], ipc,
		 MIN(100.0, us) * IAS_EWMA_FACTOR);
}

void ias_ht_poll(uint64_t now_us)
{
	struct ias_data *sd, *sd2;
	int i;

	/* update the IPC estimation for each core */
	ias_for_each_proc(sd) {
		for (i = 0; i < sd->p->active_thread_count; i++)
			ias_ht_poll_one(sd, sd->p->active_threads[i]);
	}

	/* refresh the maximum IPC for each process */
	ias_for_each_proc(sd) {
		sd->ht_max_ipc = 0;
		ias_for_each_proc(sd2) {
			sd->ht_max_ipc = MAX(sd->ht_max_ipc,
					     sd->ht_pairing_ipc[sd2->idx]);
		}
		sd->ht_max_ipc = MAX(sd->ht_max_ipc, sd->ht_unpaired_ipc);
	}
}

void ias_ht_random_kick(void)
{
	static int rr_cnt = 0;
	int cnt = 0, core;
	bitmap_for_each_set(sched_allowed_cores, NCPU, core) {
		struct ias_data *sd = cores[core];
		if (cnt == rr_cnt && sd) {
			if (sd->threads_active > sd->threads_guaranteed)
				ias_idle_on_core(core);
			break;
		}
		cnt++;
	}
	rr_cnt = (rr_cnt + 1) % num_sched_allowed_cores;	
}
