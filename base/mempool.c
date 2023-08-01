/*
 * mempool.c - a simple, preallocated, virtually contiguous pool of memory
 *
 * For convenience with DMA operations, items are not allowed to straddle page
 * boundaries.
 */

#include <string.h>
#include <stdlib.h>

#include <base/mempool.h>
#include <base/assert.h>

#ifdef DEBUG

static void mempool_common_check(struct mempool *m, void *item)
{
	uintptr_t pos = (uintptr_t)item;
	uintptr_t start = (uintptr_t)m->buf;

	/* is the item within the bounds of the pool */
	assert(pos >= start && pos < start + m->len);

	/* is the item properly aligned */
	assert((start & (m->pgsize - 1)) % m->item_len == 0);
}

void __mempool_alloc_debug_check(struct mempool *m, void *item)
{
	mempool_common_check(m, item);

	/* poison the item */
	memset(item, 0xAB, m->item_len);
}

void __mempool_free_debug_check(struct mempool *m, void *item)
{
	mempool_common_check(m, item);

	/* poison the item */
	memset(item, 0xCD, m->item_len);
}

#endif /* DEBUG */

static int mempool_populate(struct mempool *m, void *buf, size_t len,
			    size_t pgsize, size_t item_len)
{
	size_t items_per_page = pgsize / item_len;
	size_t nr_pages = len / pgsize;
	int i, j;

	m->free_items = calloc(nr_pages * items_per_page, sizeof(void *));
	if (!m->free_items)
		return -ENOMEM;

	for (i = 0; i < nr_pages; i++) {
		for (j = 0; j < items_per_page; j++) {
			m->free_items[m->capacity++] =
				(char *)buf + pgsize * i + item_len * j;
		}
	}

	return 0;
}

/**
 * mempool_create - initializes a memory pool
 * @m: the memory pool to initialize
 * @buf: the start of the buffer region managed by the pool
 * @len: the length of the buffer region managed by the pool
 * @pgsize: the size of the pages in the buffer region (must be uniform)
 * @item_len: the length of each item in the pool
 */
int mempool_create(struct mempool *m, void *buf, size_t len,
		   size_t pgsize, size_t item_len)
{
	if (item_len == 0 || !is_power_of_two(pgsize) || len % pgsize != 0)
		return -EINVAL;
		
	m->allocated = 0;
	m->buf = buf;
	m->len = len;
	m->pgsize = pgsize;
	m->item_len = item_len;

	return mempool_populate(m, buf, len, pgsize, item_len);
}

/**
 * mempool_destroy - tears down a memory pool
 * @m: the memory pool to tear down
 */
void mempool_destroy(struct mempool *m)
{
	free(m->free_items);
}

struct mempool_tc {
	struct mempool *m;
	spinlock_t lock;
};

static void mempool_tcache_free(struct tcache *tc, int nr, void **items)
{
	int i;

	struct mempool_tc *mptc = (struct mempool_tc *)tc->data;

	spin_lock(&mptc->lock);
	for (i = 0; i < nr; i++) {
		mempool_free(mptc->m, items[i]);
	}
	spin_unlock(&mptc->lock);
}

static int mempool_tcache_alloc(struct tcache *tc, int nr, void **items)
{
	int i;

	struct mempool_tc *mptc = (struct mempool_tc *)tc->data;

	if (unlikely(ACCESS_ONCE(mptc->m->allocated) + nr > mptc->m->capacity))
		return -ENOMEM;

	spin_lock(&mptc->lock);
	for (i = 0; i < nr; i++) {
		items[i] = mempool_alloc(mptc->m);
		if (items[i] == NULL) {
			spin_unlock(&mptc->lock);
			mempool_tcache_free(tc, i, items);
			return -ENOMEM;
		}
	}
	spin_unlock(&mptc->lock);
	return 0;
}

static const struct tcache_ops mempool_tcache_ops = {
    .alloc = mempool_tcache_alloc, .free = mempool_tcache_free,
};

struct tcache *mempool_create_tcache(struct mempool *m, const char *name,
				     unsigned int mag_size)
{
	struct mempool_tc *mptc;
	struct tcache *tc;

	mptc = malloc(sizeof(*mptc));
	if (mptc == NULL)
		return NULL;

	mptc->m = m;
	spin_lock_init(&mptc->lock);

	tc = tcache_create(name, &mempool_tcache_ops, mag_size, m->item_len);
	if (!tc) {
		free(mptc);
		return NULL;
	}

	tc->data = (unsigned long)mptc;
	return tc;
}
