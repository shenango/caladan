/*
 * pmc.h - useful definitions for Intel Performance Counters
 */

#pragma once

#define PMC_ESEL_UMASK_SHIFT    8
#define PMC_ESEL_CMASK_SHIFT    24
#define PMC_ESEL_ENTRY(event, umask, cmask)		\
        (((event) & 0xFFUL) |				\
         (((umask) & 0xFFUL) << PMC_ESEL_UMASK_SHIFT) |	\
         (((cmask) & 0xFFUL) << PMC_ESEL_CMASK_SHIFT))
#define PMC_ESEL_USR            (1ULL << 16) /* User Mode */
#define PMC_ESEL_OS             (1ULL << 17) /* Kernel Mode */
#define PMC_ESEL_EDGE           (1ULL << 18) /* Edge detect */
#define PMC_ESEL_PC             (1ULL << 19) /* Pin control */
#define PMC_ESEL_INT            (1ULL << 20) /* APIC interrupt enable */
#define PMC_ESEL_ANY            (1ULL << 21) /* Any thread */
#define PMC_ESEL_ENABLE         (1ULL << 22) /* Enable counters */
#define PMC_ESEL_INV            (1ULL << 23) /* Invert counter mask */

/* architectural performance counters (works on all Intel CPUs) */
#define PMC_ARCH_CORE_CYCLES    PMC_ESEL_ENTRY(0x3C, 0x00, 0)
#define PMC_ARCH_INSTR_RETIRED  PMC_ESEL_ENTRY(0xC0, 0x00, 0)
#define PMC_ARCH_REF_CYCLES     PMC_ESEL_ENTRY(0x3C, 0x01, 0)
#define PMC_ARCH_LLC_REF        PMC_ESEL_ENTRY(0x2E, 0x4F, 0)
#define PMC_ARCH_LLC_MISSES     PMC_ESEL_ENTRY(0x2E, 0x41, 0)
#define PMC_ARCH_BRANCHES       PMC_ESEL_ENTRY(0xC4, 0x00, 0)
#define PMC_ARCH_BRANCH_MISSES  PMC_ESEL_ENTRY(0xC5, 0x00, 0)

/* this performance counter measures LLC misses as a proxy for mem bandwidth */
#define PMC_LLC_MISSES (PMC_ARCH_LLC_MISSES | PMC_ESEL_USR | PMC_ESEL_OS | \
			PMC_ESEL_ANY | PMC_ESEL_ENABLE)
