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

void ias_ht_poll(uint64_t now_us)
{
	struct thread *th;
	struct ias_data *sd, *sd2;
	unsigned int core, sib;
	int i;

	/* update the IPC estimation for each core */
	ias_for_each_proc(sd) {
		for (i = 0; i < sd->p->active_thread_count; i++) {
			double ipc, us;
			uint64_t last_tsc, last_instr, cur_tsc, cur_instr;

			th = sd->p->active_threads[i];
			if (!th->active)
				continue;

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
				continue;
			if (ias_gen[core] != sd->ht_last_gen[core]) {
				sd->ht_last_gen[core] = ias_gen[core];
				continue;
			}

			ipc = (double)(cur_instr - last_instr) /
			      (double)(cur_tsc - last_tsc);
			if (ipc > 5.0)
				continue; /* bad sample */

			/* update IPC metrics */
			us = (double)(cur_tsc - last_tsc) / (double)cycles_per_us;
			if (us > 100.0)
				us = 100.0;
			if (!cores[sib]) {
				ias_ewma(&sd->ht_unpaired_ipc, ipc,
					 us * IAS_EWMA_FACTOR);
			} else {
				ias_ewma(&sd->ht_pairing_ipc[cores[sib]->idx],
					 ipc, us * IAS_EWMA_FACTOR);
			}
		}
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
