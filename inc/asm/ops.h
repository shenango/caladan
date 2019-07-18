/*
 * ops.h - useful x86_64 instructions
 */

#pragma once

#include <base/types.h>

static inline void cpu_relax(void)
{
	asm volatile("pause");
}

static inline void cpu_serialize(void)
{
        asm volatile("xorl %%eax, %%eax\n\t"
		     "cpuid" : : : "%rax", "%rbx", "%rcx", "%rdx");
}

static inline uint64_t rdtsc(void)
{
	uint32_t a, d;
	asm volatile("rdtsc" : "=a" (a), "=d" (d));
	return ((uint64_t)a) | (((uint64_t)d) << 32);
}

static inline uint64_t rdtscp(uint32_t *auxp)
{
	uint32_t a, d, c;
	asm volatile("rdtscp" : "=a" (a), "=d" (d), "=c" (c));
	if (auxp)
		*auxp = c;
	return ((uint64_t)a) | (((uint64_t)d) << 32);
}

static inline uint64_t __mm_crc32_u64(uint64_t crc, uint64_t val)
{
	asm("crc32q %1, %0" : "+r" (crc) : "rm" (val));
	return crc;
}
