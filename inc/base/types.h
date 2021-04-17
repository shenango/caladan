/*
 * types.h - primitive type definitions
 */

#pragma once

#include <stdbool.h>
#include <asm/cpu.h>

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;

#ifndef __WORD_SIZE
#error __WORD_SIZE is undefined
#endif

#if __WORD_SIZE == __64BIT_WORDS

typedef unsigned long uint64_t;
typedef signed long int64_t;

#else /* __WORDSIZE == __64BIT_WORDS */

typedef unsigned long long uint64_t;
typedef signed long long int64_t;

#endif /* __WORDSIZE == __64BIT_WORDS */

typedef unsigned long	uintptr_t;
typedef long		intptr_t;
typedef long		off_t;
typedef unsigned long	size_t;
typedef long		ssize_t;

typedef struct {
	volatile int locked;
} spinlock_t;

typedef struct {
	volatile int cnt;
} atomic_t;

typedef struct {
	volatile long cnt;
} atomic64_t;
