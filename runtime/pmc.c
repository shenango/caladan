/*
 * pmc.c - support for accessing performance counters from userspace
 */

#include "defs.h"

#define PMC_FIXED_CTR_OFFSET		(1 << 30)

/* see table 18-2 in Intel Vol. 3B */
enum {
	PMC_FIXED_INSTR_RETIRED_ANY	= 0,
	PMC_FIXED_CLK_UNHALTED		= 1, /* influenced by cpufreq */
	PMC_FIXED_CLK_UNHALTED_REF_TSC	= 2, /* stable regardless of cpufreq */
};

/**
 * rdpmc - reads a performance counter
 * @idx: the fixed performance counter index to read
 *
 * Returns the current counter value.
 */
static unsigned long rdpmc(int idx)
{
	uint32_t a, d, c;

	c = PMC_FIXED_CTR_OFFSET + idx;
	asm volatile("rdpmc" : "=a" (a), "=d" (d) : "c" (c));
	return ((unsigned long)a) | (((unsigned long)d) << 32);
}

/**
 * pmc_periodic - updates the performance counters shared with the iokernel
 * @k: the kthread to update
 */
void pmc_periodic(struct kthread *k)
{
	ACCESS_ONCE(k->q_ptrs->tsc) = rdtsc();
	ACCESS_ONCE(k->q_ptrs->instr) = rdpmc(PMC_FIXED_INSTR_RETIRED_ANY);
}
