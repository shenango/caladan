/*
 * cpu.h - detection for CPU topology
 */

#pragma once

#include <base/stddef.h>
#include <base/limits.h>
#include <base/bitmap.h>

extern int cpu_count; /* the number of available CPUs */
extern int numa_count; /* the number of NUMA nodes */
extern int numa_count_with_mem; /* the number of NUMA nodes with local memory */

struct cpu_info {
	DEFINE_BITMAP(thread_siblings_mask, NCPU);
	DEFINE_BITMAP(core_siblings_mask, NCPU);
	int package;
};

extern struct cpu_info cpu_info_tbl[NCPU];
