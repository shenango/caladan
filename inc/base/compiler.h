/*
 * compiler.h - useful compiler hints, intrinsics, and attributes
 */

#pragma once

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#ifndef __cplusplus
#define unreachable() __builtin_unreachable()
#endif

#define prefetch0(x) __builtin_prefetch((x), 0, 3)
#define prefetch1(x) __builtin_prefetch((x), 0, 2)
#define prefetch2(x) __builtin_prefetch((x), 0, 1)
#define prefetchnta(x) __builtin_prefetch((x), 0, 0)
#define prefetch(x) prefetch0(x)

/* variable attributes */
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#define __notused __attribute__((unused))
#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif

/* function attributes */
#define __noinline __attribute__((noinline))
#define __noreturn __attribute__((noreturn))
#define __must_use_return __attribute__((warn_unused_result))
#define __pure __attribute__((pure))
#define __weak __attribute__((weak))
#define __malloc __attribute__((malloc))
#define __assume_aligned(x) __attribute__((assume_aligned(x)))
#define __nofp __attribute__((target("general-regs-only")))

#define GCC_VERSION (__GNUC__ * 10000        \
		     + __GNUC_MINOR__ * 100  \
		     + __GNUC_PATCHLEVEL__)

#if GCC_VERSION >= 40800
#define HAS_BUILTIN_BSWAP 1
#endif

#define barrier() asm volatile("" ::: "memory")

#define	ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

#define type_is_native(t) \
	(sizeof(t) == sizeof(char)  || \
	 sizeof(t) == sizeof(short) || \
	 sizeof(t) == sizeof(int)   || \
	 sizeof(t) == sizeof(long))

/*
 * These attributes are defined only with the sparse checker tool.
 */
#ifdef __CHECKER__
#define __rcu		__attribute__((noderef, address_space(1)))
#define __perthread	__attribute__((noderef, address_space(2)))
#define __force		__attribute__((force))
#undef __assume_aligned
#define __assume_aligned(x)
#else /* __CHECKER__ */
#define __rcu
#define __perthread
#define __force
#endif /* __CHECKER__ */
