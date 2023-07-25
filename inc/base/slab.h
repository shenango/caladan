/*
 * slab.h - a SLAB allocator
 */

#pragma once

#include <base/stddef.h>
#include <base/list.h>
#include <base/thread.h>
#include <base/limits.h>

/* forward declarations */
struct slab_hdr;
struct slab_node;
struct tcache;


/*
 * slab support
 */

#define SLAB_CHUNK_SIZE 	8
#define SLAB_MIN_SIZE		16

/* function attributes for methods that allocate slab items */
#define __slab_malloc		__malloc __assume_aligned(SLAB_MIN_SIZE)

/* Slab nodes are per-numa node slab internal state. */
struct slab_node {
	size_t			size;
	int			numa_node;
	int			offset;
	int			flags;
	int			nr_elems;
	spinlock_t		page_lock;

	/* slab pages */
	off_t			pg_off;
	struct page		*cur_pg;
	struct list_head	full_list;
	struct list_head	partial_list;
	int			nr_pages;
};

struct slab {
	const char		*name;
	size_t			size;
	struct list_node	link;
	struct slab_node	*nodes[NNUMA];
} __aligned(CACHE_LINE_SIZE);

/* force the slab to be backed with large pages */
#define SLAB_FLAG_LGPAGE	BIT(0)
/* false sharing is okay (less internal fragmentation) */
#define SLAB_FLAG_FALSE_OKAY	BIT(1)
/* managing 4kb pages (internal use only) */
#define SLAB_FLAG_PAGES		BIT(2)

extern int slab_create(struct slab *s, const char *name, size_t size, int flags);
extern void slab_destroy(struct slab *s);
extern int slab_reclaim(struct slab *s);
extern void *slab_alloc_on_node(struct slab *s, int numa_node) __slab_malloc;
extern void slab_free(struct slab *s, void *item);
extern void slab_print_usage(void);

/**
 * slab_alloc - allocates an item on the local NUMA node
 * @s: the slab to allocate from
 *
 * Returns an item or NULL if out of memory.
 */
static __always_inline void *slab_alloc(struct slab *s)
{
	return slab_alloc_on_node(s, this_numa_node());
}

struct tcache *slab_create_tcache(struct slab *s, unsigned int mag_size);
