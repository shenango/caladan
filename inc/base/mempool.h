/*
 * mempool.h - a simple, preallocated pool of memory
 */

#pragma once

#include <base/stddef.h>
#include <base/tcache.h>

struct mempool {
	void			**free_items;
	size_t			allocated;
	size_t			capacity;
	void			*buf;
	size_t			len;
	size_t			pgsize;
	size_t			item_len;
};

#ifdef DEBUG
extern void __mempool_alloc_debug_check(struct mempool *m, void *item);
extern void __mempool_free_debug_check(struct mempool *m, void *item);
#else /* DEBUG */
static inline void __mempool_alloc_debug_check(struct mempool *m, void *item) {}
static inline void __mempool_free_debug_check(struct mempool *m, void *item) {}
#endif /* DEBUG */

static inline bool mempool_member(struct mempool *m, void *addr)
{
	return addr >= m->buf && addr < m->buf + m->len;
}

/**
 * mempool_alloc - allocates an item from the pool
 * @m: the memory pool to allocate from
 *
 * Returns an item, or NULL if the pool is empty.
 */
static inline void *mempool_alloc(struct mempool *m)
{
	void *item;
	if (unlikely(m->allocated >= m->capacity))
		return NULL;
	item = m->free_items[m->allocated++];
	__mempool_alloc_debug_check(m, item);
	return item;
}

/**
 * mempool_free - returns an item to the pool
 * @m: the memory pool the item was allocated from
 * @item: the item to return
 */
static inline void mempool_free(struct mempool *m, void *item)
{
	__mempool_free_debug_check(m, item);
	m->free_items[--m->allocated] = item;
	assert(m->allocated <= m->capacity); /* could have overflowed */
}

extern int mempool_create(struct mempool *m, void *buf, size_t len,
			  size_t pgsize, size_t item_len);
extern void mempool_destroy(struct mempool *m);

extern struct tcache *mempool_create_tcache(struct mempool *m, const char *name,
					    unsigned int mag_size);
