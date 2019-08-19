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
	int i, idx;

	/* update the IPC estimation for each core */
	ias_for_each_proc(sd) {
		for (i = 0; i < sd->p->active_thread_count; i++) {
			float ipc;
			uint64_t last_tsc, last_instr, cur_tsc, cur_instr;

			th = sd->p->active_threads[i];
			if (!th->active)
				continue;

			idx = th - sd->p->threads;
			core = th->core;
			sib = sched_siblings[core];

			/* calculate IPC and update counters */
			last_tsc = sd->ht_last_tsc[idx];
			last_instr = sd->ht_last_instr[idx];
			cur_tsc = th->q_ptrs->tsc;
			cur_instr = th->q_ptrs->instr;
			if (cur_tsc == last_tsc)
				continue;
			ipc = (float)(cur_instr - last_instr) /
			      (float)(cur_tsc - last_tsc);
			sd->ht_last_tsc[idx] = cur_tsc;
			sd->ht_last_instr[idx] = cur_instr;
			if (ipc > 5.0)
				continue; /* bad sample */

			/* update IPC metrics */
			if (!cores[sib]) {
				ias_ewma(&sd->ht_unpaired_ipc, ipc,
					 IAS_EWMA_FACTOR);
			} else {
				ias_ewma(&sd->ht_pairing_ipc[cores[sib]->idx],
					 ipc, IAS_EWMA_FACTOR);
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
