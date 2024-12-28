/*
 * mbuf.c - buffer management for network packets
 */

#include <string.h>

#include <net/mbuf.h>

/**
 * mbuf_clone - creates an identical copy of an mbuf
 * @dst: the destination mbuf
 * @src: the source mbuf
 *
 * Returns the destination mbuf.
 */
struct mbuf *mbuf_clone(struct mbuf *dst, struct mbuf *src)
{
	/* copy the backing buffer */
	dst->data = dst->head + mbuf_headroom(src);
	memcpy(mbuf_put(dst, mbuf_length(src)),
	       mbuf_data(src), mbuf_length(src));

	/* copy packet metadata */
	dst->csum_type = src->csum_type;
	dst->txflags = src->txflags; /* NOTE: this is a union */

	return dst;
}
