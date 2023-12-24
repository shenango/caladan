/*
 * ops.h - useful x86_64 instructions
 */

#pragma once

#include <features.h>
#include <base/types.h>

static inline void cpu_relax(void)
{
#if __GNUC_PREREQ(10, 0)
#  if __has_builtin(__builtin_ia32_pause)
	__builtin_ia32_pause();
#  endif
#else
	asm volatile("pause");
#endif
}

static inline void cpu_serialize(void)
{
        asm volatile("xorl %%eax, %%eax\n\t"
		     "cpuid" : : : "%rax", "%rbx", "%rcx", "%rdx");
}

struct cpuid_info {
	unsigned int eax, ebx, ecx, edx;
};

static inline void cpuid(int leaf, int subleaf, struct cpuid_info *regs)
{
	asm volatile("cpuid" : "=a" (regs->eax), "=b" (regs->ebx),
		     "=c" (regs->ecx), "=d" (regs->edx) : "a" (leaf),
		     "c"(subleaf));
}

static inline uint64_t rdtsc(void)
{
#if __GNUC_PREREQ(10, 0)
#  if __has_builtin(__builtin_ia32_rdtsc)
	return __builtin_ia32_rdtsc();
#  endif
#else
	uint64_t a, d;
	asm volatile("rdtsc" : "=a" (a), "=d" (d));
	return a | (d << 32);
#endif
}

static inline uint64_t rdtscp(uint32_t *auxp)
{
	uint64_t ret;
	uint32_t c;

#if __GNUC_PREREQ(10, 0)
#  if __has_builtin(__builtin_ia32_rdtscp)
	ret = __builtin_ia32_rdtscp(&c);
#  endif
#else
	uint64_t a, d;
	asm volatile("rdtscp" : "=a" (a), "=d" (d), "=c" (c));
	ret = a | (d << 32);
#endif

	if (auxp)
		*auxp = c;
	return ret;
}

static inline uint64_t __mm_crc32_u64(uint64_t crc, uint64_t val)
{
	asm("crc32q %1, %0" : "+r" (crc) : "rm" (val));
	return crc;
}
