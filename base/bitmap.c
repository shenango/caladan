/*
 * bitmap.c - a library for bit array manipulation
 */

#include <base/stddef.h>
#include <base/bitmap.h>

static __always_inline int
bitmap_find_next(unsigned long *bits, int nbits, int pos, bool invert)
{
	unsigned long val, mask = ~((1UL << BITMAP_POS_SHIFT(pos)) - 1);
	int idx;

	for (idx = align_down(pos, BITS_PER_LONG);
	     idx < nbits; idx += BITS_PER_LONG) {
		val = bits[BITMAP_POS_IDX(idx)];
		if (invert)
			val = ~val;
		val &= mask;
		if (val)
			return MIN(idx + __builtin_ffsl(val) - 1, nbits);
		mask = ~0UL;
	}

	return nbits;
}

/**
 * bitmap_find_next_cleared - finds the next cleared bit
 * @bits: the bitmap
 * @nbits: the number of total bits
 * @pos: the starting bit
 *
 * Returns the bit index of the next zero bit, or the total size if none exists.
 */
int bitmap_find_next_cleared(unsigned long *bits, int nbits, int pos)
{
	return bitmap_find_next(bits, nbits, pos, true);
}

/**
 * bitmap_find_next_set - finds the next set bit
 * @bits: the bitmap
 * @nbits: the number of total bits
 * @pos: the starting bit
 *
 * Returns the bit index of the next zero bit, or the total size if none exists.
 */
int bitmap_find_next_set(unsigned long *bits, int nbits, int pos)
{
	return bitmap_find_next(bits, nbits, pos, false);
}
