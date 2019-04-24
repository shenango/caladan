/*
 * bitmap.c - a library for bit array manipulation
 */

#include <ctype.h>
#include <stdlib.h>

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

/* code copied from util-linux */
static const char *nexttoken(const char *q,  int sep)
{
	if (q)
		q = strchr(q, sep);
	if (q)
		q++;
	return q;
}

/* code copied from util-linux */
static int nextnumber(const char *str, char **end, unsigned int *result)
{
	errno = 0;
	if (str == NULL || *str == '\0' || !isdigit(*str))
		return -EINVAL;
	*result = (unsigned int) strtoul(str, end, 10);
	if (errno)
		return -errno;
	if (str == *end)
		return -EINVAL;
	return 0;
}

/* code copied from util-linux */
int string_to_bitmap(const char *str, unsigned long *bits, int nbits)
{
	const char *p, *q;
	char *end = NULL;

	q = str;
	bitmap_init(bits, nbits, false);

	while (p = q, q = nexttoken(q, ','), p) {
		unsigned int a;	/* beginning of range */
		unsigned int b;	/* end of range */
		unsigned int s;	/* stride */
		const char *c1, *c2;

		if (nextnumber(p, &end, &a) != 0)
			return 1;
		b = a;
		s = 1;
		p = end;

		c1 = nexttoken(p, '-');
		c2 = nexttoken(p, ',');

		if (c1 != NULL && (c2 == NULL || c1 < c2)) {
			if (nextnumber(c1, &end, &b) != 0)
				return 1;

			c1 = end && *end ? nexttoken(end, ':') : NULL;

			if (c1 != NULL && (c2 == NULL || c1 < c2)) {
				if (nextnumber(c1, &end, &s) != 0)
					return 1;
				if (s == 0)
					return 1;
			}
		}

		if (!(a <= b))
			return 1;
		while (a <= b) {
			if (a >= nbits)
				return 1;
			bitmap_set(bits, a);
			a += s;
		}
	}

	if (end && *end)
		return 1;
	return 0;
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
