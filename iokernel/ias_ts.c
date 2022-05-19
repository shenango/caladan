/*
 * ias_ts.c - the time sharing controller
 */

#include <base/stddef.h>
#include <base/log.h>

#include "defs.h"
#include "sched.h"
#include "ksched.h"
#include "ias.h"

/* statistics */
uint64_t ias_ts_yield_count;

/**
 * ias_ts_poll - runs the time sharing controller
 */
void ias_ts_poll(void)
{
	struct thread *th;
	struct ias_data *sd;
	struct thread_metrics *m;
	unsigned int core, tmp;

	sched_for_each_allowed_core(core, tmp) {
		sd = cores[core];
		if (!sd || sd->quantum_us == 0)
			continue;
		th = sched_get_thread_on_core(core);
		m = &th->metrics;
		if (!m->work_pending || m->uthread_elapsed_us < sd->quantum_us)
			continue;

		ias_ts_yield_count++;
		sched_yield_on_core(core);
	}
}
