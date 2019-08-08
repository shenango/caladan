/*
 * bitmap.h - a library for bit array manipulation
 */

#pragma once

#include <string.h>

#include <base/stddef.h>
#include <base/atomic.h>

#define BITS_PER_LONG	(sizeof(long) * 8)
#define BITMAP_LONG_SIZE(nbits) \
	div_up(nbits, (typeof(nbits))BITS_PER_LONG)

#define DEFINE_BITMAP(name, nbits) \
	unsigned long name[BITMAP_LONG_SIZE(nbits)]
#define DECLARE_BITMAP(name, nbits) \
	extern DEFINE_BITMAP(name, nbits)

typedef unsigned long *bitmap_ptr_t;

#define BITMAP_POS_IDX(pos)	((pos) / BITS_PER_LONG)
#define BITMAP_POS_SHIFT(pos)	((pos) % BITS_PER_LONG)

/**
 * bitmap_set - sets a bit in the bitmap
 * @bits: the bitmap
 * @pos: the bit number
 */
static inline void bitmap_set(unsigned long *bits, int pos)
{
	bits[BITMAP_POS_IDX(pos)] |= (1ul << BITMAP_POS_SHIFT(pos));
}

/**
 * bitmap_clear - clears a bit in the bitmap
 * @bits: the bitmap
 * @pos: the bit number
 */
static inline void bitmap_clear(unsigned long *bits, int pos)
{
	bits[BITMAP_POS_IDX(pos)] &= ~(1ul << BITMAP_POS_SHIFT(pos));
}

/**
 * bitmap_test - tests if a bit is set in the bitmap
 * @bits: the bitmap
 * @pos: the bit number
 *
 * Returns true if the bit is set, otherwise false.
 */
static inline bool bitmap_test(const unsigned long *bits, int pos)
{
	return (bits[BITMAP_POS_IDX(pos)] & (1ul << BITMAP_POS_SHIFT(pos))) != 0;
}

/**
 * bitmap_popcount - calculates the bit weight of the bitmap
 * @bits: the bitmap
 * @nbits: the number of bits in the bitmap
 *
 * Returns the bit weight.
 */
static inline int bitmap_popcount(const unsigned long *bits, int nbits)
{
	int i, val = 0;

	for (i = 0; i < BITMAP_LONG_SIZE(nbits); i++)
		val += __builtin_popcountl(bits[i]);

	return val;
}

/**
 * bitmap_and - performs a bitwise AND operation
 * @dst: the destination to store the result
 * @src1: the first input
 * @src2: the second input
 * @nbits: the number of bits in the bitmap
 */
static inline void bitmap_and(unsigned long *dst, const unsigned long *src1,
			      const unsigned long *src2, int nbits)
{
	int i;

	for (i = 0; i < BITMAP_LONG_SIZE(nbits); i++)
		dst[i] = src1[i] & src2[i];
}

/**
 * bitmap_or - performs a bitwise OR operation
 * @dst: the destination to store the result
 * @src1: the first input
 * @src2: the second input
 * @nbits: the number of bits in the bitmap
 */
static inline void bitmap_or(unsigned long *dst, const unsigned long *src1,
			     const unsigned long *src2, int nbits)
{
	int i;

	for (i = 0; i < BITMAP_LONG_SIZE(nbits); i++)
		dst[i] = src1[i] | src2[i];
}

/**
 * bitmap_xor - performs a bitwise XOR operation
 * @dst: the destination to store the result
 * @src1: the first input
 * @src2: the second input
 * @nbits: the number of bits in the bitmap
 */
static inline void bitmap_xor(unsigned long *dst, const unsigned long *src1,
			      const unsigned long *src2, int nbits)
{
	int i;

	for (i = 0; i < BITMAP_LONG_SIZE(nbits); i++)
		dst[i] = src1[i] ^ src2[i];
}

/**
 * bitmap_atomic_set - atomically sets a bit in the bitmap
 * @bits: the bitmap
 * @pos: the bit number
 */
static inline void bitmap_atomic_set(unsigned long *bits, int pos)
{
	atomic64_fetch_and_or((atomic64_t *)&bits[BITMAP_POS_IDX(pos)],
			      (1ul << BITMAP_POS_SHIFT(pos)));
}

/**
 * bitmap_atomic_test_and_set - atomically tests and sets a bit in the bitmap
 * @bits: the bitmap
 * @pos; the bit number
 */
static inline bool bitmap_atomic_test_and_set(unsigned long *bits, int pos)
{
	unsigned long bit = (1ul << BITMAP_POS_SHIFT(pos));
	return (atomic64_fetch_and_or((atomic64_t *)&bits[BITMAP_POS_IDX(pos)],
				      bit) & bit) != 0;
}

/**
 * bitmap_atomic_clear - atomically clears a bit in the bitmap
 * @bits: the bitmap
 * @pos: the bit number
 */
static inline void bitmap_atomic_clear(unsigned long *bits, int pos)
{
	atomic64_fetch_and_and((atomic64_t *)&bits[BITMAP_POS_IDX(pos)],
			       ~(1ul << BITMAP_POS_SHIFT(pos)));
}

/**
 * bitmap_atomic_test - atomically tests a bit in the bitmap
 * @bits: the bitmap
 * @pos: the bit number
 */
static inline bool bitmap_atomic_test(unsigned long *bits, int pos)
{
	return (atomic64_read((atomic64_t *)&bits[BITMAP_POS_IDX(pos)]) &
		(1ul << BITMAP_POS_SHIFT(pos))) != 0;
		
}

/**
 * bitmap_init - initializes a bitmap
 * @bits: the bitmap
 * @nbits: the number of total bits
 * @state: if true, all bits are set, otherwise all bits are cleared
 */
static inline void bitmap_init(unsigned long *bits, int nbits, bool state)
{
	memset(bits, state ? 0xff : 0, BITMAP_LONG_SIZE(nbits) * sizeof(long));
}

extern int bitmap_find_next_set(unsigned long *bits, int nbits, int pos);
extern int bitmap_find_next_cleared(unsigned long *bits, int nbits, int pos);
extern int string_to_bitmap(const char *str, unsigned long *bits, int nbits);

/**
 * bitmap_for_each_set - generates a loop iteration over each set bit
 * @bits: the bitmap
 * @nbits: the number of total bits
 * @pos: the bit position (int)
 */
#define bitmap_for_each_set(bits, nbits, pos)				\
	for ((pos) = -1;						\
	     (pos) = bitmap_find_next_set((bits), (nbits), ((pos) + 1)),\
	     (pos) < (nbits);)

/**
 * bitmap_for_each_cleared - generates a loop iteration over each cleared bit
 * @bits: the bitmap
 * @nbits: the number of total bits
 * @pos: the bit position (int)
 */
#define bitmap_for_each_cleared(bits, nbits, pos)			\
	for ((pos) = -1;						\
	     (pos) = bitmap_find_next_cleared((bits), (nbits), ((pos) + 1)),\
	     (pos) < (nbits);)
