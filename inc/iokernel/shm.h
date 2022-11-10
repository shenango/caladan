/*
 * shm.h - shared memory communication between the iokernel and clients
 */

#pragma once

#include <limits.h>

#include <base/atomic.h>
#include <base/gen.h>
#include <base/lrpc.h>
#include <base/mem.h>
#include <base/stddef.h>

#define INGRESS_MBUF_SHM_KEY 0x696d736b /* "imsk" */
#define INGRESS_MBUF_SHM_SIZE 0x20000000

#define IOKERNEL_INFO_KEY 0x696e666f /* "info" */
#define IOKERNEL_INFO_SIZE PGSIZE_4KB

/*
 * Shared memory pointer support. These are pointers that are passed across
 * address spaces, so the mapped regions will have different base offsets.
 */

typedef uintptr_t shmptr_t;

/* shared memory pointers have a special non-zero NULL value */
#define SHMPTR_NULL	ULONG_MAX

struct shm_region {
	void		*base;
	size_t		len;
};

/**
 * ptr_to_shmptr - converts a normal pointer to a shared memory pointer
 * @r: the shared memory region the pointer resides in
 * @ptr: the normal pointer to convert
 * @len: the size of the object
 *
 * Returns a shared memory pointer.
 */
static inline shmptr_t
ptr_to_shmptr(struct shm_region *r, void *ptr, size_t len)
{
	assert((uintptr_t)r->base <= (uintptr_t)ptr);
	assert((uintptr_t)ptr + len <= (uintptr_t)r->base + r->len);
	return (uintptr_t)ptr - (uintptr_t)r->base;
}

/**
 * shmptr_to_ptr - converts a shared memory pointer to a normal pointer
 * @r: the shared memory region the shared memory pointer resides in
 * @shmptr: the shared memory pointer
 * @len: the size of the object
 *
 * Returns a normal pointer, or NULL if the shared memory pointer is outside
 * the region.
 */
static inline void *
shmptr_to_ptr(struct shm_region *r, shmptr_t shmptr, size_t len)
{
	/* WARNING: could wrap around! */
	if (unlikely(ULONG_MAX - shmptr < r->len || shmptr + len > r->len))
		return NULL;
	return (void *)(shmptr + (uintptr_t)r->base);
}


/*
 * message chains are singly-linked lists of commands passed over shared memory.
 */

struct shm_chain {
	unsigned long	type;
	shmptr_t	next;
};

/* For performance, we intend for reading the chain to pull in command data */
BUILD_ASSERT(sizeof(struct shm_chain) <= CACHE_LINE_SIZE);

/**
 * shm_chain_get_next - retrieves the next request in the chain
 * @r: the shared memory region the chains reside in
 * @c: the current request
 * @len: the size of the object
 *
 * Returns a request, or NULL if at the end of the chain or invalid.
 */
static inline struct shm_chain *
shm_chain_get_next(struct shm_region *r, struct shm_chain *c, size_t len)
{
	return (struct shm_chain *)shmptr_to_ptr(r, load_acquire(&c->next), len);
}

/**
 * shm_chain_set_next - appends the next request to the chain
 * @r: the shared memory region the chains reside in
 * @c: the current request
 * @next: the next request
 * @len: the size of the object
 */
static inline void
shm_chain_set_next(struct shm_region *r, struct shm_chain *c,
		   struct shm_chain *next, size_t len)
{
	shmptr_t shmptr = ptr_to_shmptr(r, next, len);
	assert(c->next == SHMPTR_NULL);
	store_release(&c->next, shmptr);
}

/**
 * Functions for initializing lrpc channels in shared memory.
 */

/* describes a shared memory queue */
struct queue_spec {
	size_t			msg_count;
	shmptr_t		msg_buf;
	shmptr_t		wb;
};

static inline int shm_init_lrpc_in(struct shm_region *r, struct queue_spec *s,
		struct lrpc_chan_in *c)
{
	struct lrpc_msg *tbl;
	uint32_t *wb;

	tbl = (struct lrpc_msg *) shmptr_to_ptr(r, s->msg_buf,
			sizeof(struct lrpc_msg) * s->msg_count);
	if (!tbl)
		return -EINVAL;

	wb = (uint32_t *) shmptr_to_ptr(r, s->wb, sizeof(*wb));
	if (!wb)
		return -EINVAL;

	return lrpc_init_in(c, tbl, s->msg_count, wb);
}

static inline int shm_init_lrpc_out(struct shm_region *r, struct queue_spec *s,
		struct lrpc_chan_out *c)
{
	struct lrpc_msg *tbl;
	uint32_t *wb;

	tbl = (struct lrpc_msg *) shmptr_to_ptr(r, s->msg_buf,
			sizeof(struct lrpc_msg) * s->msg_count);
	if (!tbl)
		return -EINVAL;

	wb = (uint32_t *) shmptr_to_ptr(r, s->wb, sizeof(*wb));
	if (!wb)
		return -EINVAL;

	return lrpc_init_out(c, tbl, s->msg_count, wb);
}

/**
 * shm_init_gen - initializes a generation number in shared memory from a
 * shared memory pointer
 * @r: the shared memory region the gen number resides in
 * @gen_ptr: the shared memory pointer to the gen number
 * @g: the struct gen_num to initialize
 */
static inline int shm_init_gen(struct shm_region *r, shmptr_t gen_ptr,
		struct gen_num *g)
{
	uint32_t *gen;

	gen = (uint32_t *) shmptr_to_ptr(r, gen_ptr, sizeof(uint32_t));
	if (!gen)
		return -EINVAL;

	gen_init(g, gen);
	return 0;
}
