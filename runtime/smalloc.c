/*
 * smalloc.c - a simple malloc implementation built on top of the base
 * libary slab and thread-local cache allocator
 */

#include <base/page.h>
#include <base/slab.h>
#include <base/tcache.h>
#include <runtime/sync.h>
#include <runtime/smalloc.h>

#include "defs.h"

#define SMALLOC_MAG_SIZE	8
#define SMALLOC_BITS            15
#define SMALLOC_MIN_SIZE	SLAB_MIN_SIZE
#define SMALLOC_MAX_SIZE        (SMALLOC_MIN_SIZE << (SMALLOC_BITS - 1))
BUILD_ASSERT(SMALLOC_MIN_SIZE >= SLAB_MIN_SIZE);

static struct slab smalloc_slabs[SMALLOC_BITS];
static struct tcache *smalloc_tcaches[SMALLOC_BITS];
static DEFINE_PERTHREAD(struct tcache_perthread, smalloc_pts[SMALLOC_BITS]);

/**
 * smalloc_size_to_idx - converts a size to a cache index
 * @size: the size of the item to allocate
 *
 * Returns the smalloc cache index.
 */
static inline int smalloc_size_to_idx(size_t size)
{
	return  64 - __builtin_ctz(SMALLOC_MIN_SIZE) -
		__builtin_clzl((size - 1) | SMALLOC_MIN_SIZE);
}

static const char *slab_names[SMALLOC_BITS] = {
	"smalloc (16 B)",
	"smalloc (32 B)",
	"smalloc (64 B)",
	"smalloc (128 B)",
	"smalloc (256 B)",
	"smalloc (512 B)",
	"smalloc (1 KB)",
	"smalloc (2 KB)",
	"smalloc (4 KB)",
	"smalloc (8 KB)",
	"smalloc (16 KB)",
	"smalloc (32 KB)",
	"smalloc (64 KB)",
	"smalloc (128 KB)",
	"smalloc (256 KB)",
};

/**
 * smalloc - allocates memory (non-inlined path)
 * @size: the size of the item
 *
 * Returns an item or NULL if out of memory.
 */
void *smalloc(size_t size)
{
	struct tcache_perthread *pt;
	void *item;

	if (unlikely(size > SMALLOC_MAX_SIZE))
		return NULL;

	preempt_disable();
	pt = perthread_ptr(smalloc_pts[smalloc_size_to_idx(size)]);
	item = tcache_alloc(pt);
	preempt_enable();

	return item;
}

/**
 * __szmalloc - allocates zeroed memory (non-inlined path)
 * @size: the size of the item
 *
 * Returns an item or NULL if out of memory.
 */
void *__szalloc(size_t size)
{
	void *item = smalloc(size);
	if (unlikely(!item))
		return NULL;

	memset(item, 0, size);
	return item;
}

/*
 * sfree - frees memory back to the generic allocator
 * @item: the item to free
 */
void sfree(void *item)
{
	struct slab_node *n = addr_to_page(item)->snode;
	struct tcache_perthread *pt;

	preempt_disable();
	pt = perthread_ptr(smalloc_pts[smalloc_size_to_idx(n->size)]);
	tcache_free(pt, item);
	preempt_enable();
}

/**
 * smalloc_init - initializes slab malloc
 *
 * NOTE: requires slab to be initialized first
 */
int smalloc_init(void)
{
	int i, ret;

	for (i = 0; i < SMALLOC_BITS; i++) {
		ret = slab_create(&smalloc_slabs[i], slab_names[i],
				  (SMALLOC_MIN_SIZE << i),
				  SLAB_FLAG_FALSE_OKAY);
		if (ret)
			return ret;

		smalloc_tcaches[i] = slab_create_tcache(&smalloc_slabs[i],
							SMALLOC_MAG_SIZE);
		if (!smalloc_tcaches[i])
			return -ENOMEM;
	}

	return 0;
}

/**
 * smalloc_init_thread - initializes slab malloc (per-thread)
 */
int smalloc_init_thread(void)
{
	int i;

	for (i = 0; i < SMALLOC_BITS; i++)
		tcache_init_perthread(smalloc_tcaches[i],
				      perthread_ptr(smalloc_pts[i]));

	return 0;
}
