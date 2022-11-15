/*
 * slab.c - the SLAB memory allocator
 *
 * Note that we don't re-use initialized slab items like Solaris. Rather this
 * implementation just applies a similar approach to memory management. See the
 * following references for details:
 *
 * The Slab Allocator: An Object-Caching Kernel Memory Allocator
 * Jeff Bonwick
 *
 * The SLAB allocator is designed for simplicity rather than for multicore
 * scalability. When scalability is required use the thread-local cache on top
 * of the SLAB allocator.
 */

#include <string.h>

#include <base/slab.h>
#include <base/lock.h>
#include <base/log.h>
#include <base/page.h>
#include <base/list.h>
#include <base/thread.h>
#include <base/assert.h>
#include <base/init.h>
#include <base/tcache.h>

#include "init_internal.h"

/* embedded within free items. */
struct slab_hdr {
	struct slab_hdr		*next_hdr;
};
BUILD_ASSERT(sizeof(struct slab_hdr) <= SLAB_MIN_SIZE);

#define SLAB_PARTIAL_THRESH	(SLAB_CHUNK_SIZE / 2)

#define SLAB_4KB_LIMIT \
	(align_down(PGSIZE_4KB / (SLAB_PARTIAL_THRESH * 2), __WORD_SIZE))
#define SLAB_2MB_LIMIT \
	(align_down(PGSIZE_2MB / (SLAB_PARTIAL_THRESH * 2), __WORD_SIZE))

static LIST_HEAD(slab_list);
static DEFINE_SPINLOCK(slab_lock);
static struct slab node_slab;

struct slab smpage_slab;
struct tcache *smpage_tcache;
#define SMPAGE_MAG_SIZE		8

/* initialization bootstrapping */
static struct slab_node early_slab_nodes[NNUMA];
static struct slab_node early_smpage_nodes[NNUMA];

static void
__slab_create_node(struct slab_node *n, int numa_node,
		   size_t size, int offset, int flags, int nr_elems)
{
	n->numa_node = numa_node;
	n->size = size;
	n->offset = offset;
	n->flags = flags;
	n->nr_elems = nr_elems;

	n->cur_pg = NULL;
	n->pg_off = 0;
	n->nr_pages = 0;

	spin_lock_init(&n->page_lock);
	list_head_init(&n->full_list);
	list_head_init(&n->partial_list);
}

static void __slab_destroy_node(struct slab_node *n)
{
	struct page *pg, *pg_next;

	list_for_each_safe(&n->full_list, pg, pg_next, link)
		page_put(pg);
	list_for_each_safe(&n->partial_list, pg, pg_next, link)
		page_put(pg);
	if (n->cur_pg)
		page_put(n->cur_pg);
}

static int
__slab_create(struct slab *s, const char *name, size_t size,
	      int offset, int flags, int nr_elems)
{
	struct slab_node *n;
	int i;

	for (i = 0; i < numa_count; i++) {
		n = (struct slab_node *)slab_alloc_on_node(&node_slab, i);
		if (!n)
			goto fail;

		__slab_create_node(n, i, size, offset, flags, nr_elems);
		s->nodes[i] = n;
	}

	spin_lock(&slab_lock);
	list_add_tail(&slab_list, &s->link);
	spin_unlock(&slab_lock);
	s->name = name;
	s->size = size;
	return 0;

fail:
	for (i--; i >= 0; i--)
		slab_free(&node_slab, s->nodes[i]);
	return -ENOMEM;
}

static void
__slab_early_create(struct slab *s, struct slab_node *nodes,
		    const char *name, size_t size,
		    int offset, int flags, int nr_elems)
{
	int i;

	for (i = 0; i < numa_count; i++) {
		__slab_create_node(&nodes[i], i, size, offset, flags, nr_elems);
		s->nodes[i] = &nodes[i];
	}

	spin_lock(&slab_lock);
	list_add(&slab_list, &s->link);
	spin_unlock(&slab_lock);
	s->name = name;
	s->size = size;
}

static int __slab_early_migrate(struct slab *s)
{
	struct slab_node *n;
	int i;

	for (i = 0; i < numa_count; i++) {
		n = (struct slab_node *)slab_alloc_on_node(&node_slab, i);
		if (!n)
			goto fail;

		memcpy(n, s->nodes[i], sizeof(*n));
		assert(list_empty(&s->nodes[i]->full_list));
		list_head_init(&n->full_list);
		assert(list_empty(&s->nodes[i]->partial_list));
		list_head_init(&n->partial_list);
		s->nodes[i] = n;
		if (n->cur_pg)
			n->cur_pg->snode = n;
	}

	return 0;

fail:
	for (i--; i >= 0; i--)
		slab_free(&node_slab, s->nodes[i]);
	return -ENOMEM;
}


/**
 * slab_create - creates a slab
 * @s: the slab
 * @name: a human readable name
 * @size: the size of items
 * @flag: flags
 *
 * Returns 0 if successful, otherwise fail.
 */
int slab_create(struct slab *s, const char *name, size_t size, int flags)
{
	int pgsize;

	/* force cache line size alignment to prevent false sharing */
	if (!(flags & SLAB_FLAG_FALSE_OKAY))
		size = align_up(size, CACHE_LINE_SIZE);
	else
		size = align_up(size, __WORD_SIZE);

	if (size > SLAB_2MB_LIMIT)
		return -E2BIG;
	if (size > SLAB_4KB_LIMIT)
		flags |= SLAB_FLAG_LGPAGE;

	pgsize = (flags & SLAB_FLAG_LGPAGE) ? PGSIZE_2MB : PGSIZE_4KB;
	return __slab_create(s, name, size, 0, flags, pgsize / size);
}

/**
 * slab_destroy - destroys a slab
 * @s: the slab
 *
 * WARNING: Frees all pages belonging to the slab, so unsafe
 * to call until all references to the slab's items have been
 * dropped.
 */
void slab_destroy(struct slab *s)
{
	int i;

	spin_lock(&slab_lock);
	list_del(&s->link);
	spin_unlock(&slab_lock);

	for (i = 0; i < numa_count; i++) {
		__slab_destroy_node(s->nodes[i]);
		slab_free(&node_slab, s->nodes[i]);
	}
}

#ifdef DEBUG

static void slab_item_check(struct slab_node *n, void *item)
{
	struct page *pg;

	if (n->flags & SLAB_FLAG_LGPAGE)
		pg = addr_to_lgpage(item);
	else
		pg = addr_to_smpage(item);

	/* alignment */
	if (n->flags & SLAB_FLAG_LGPAGE)
		assert(PGOFF_2MB(item) % n->size == 0);
	else
		assert(PGOFF_4KB(item) % n->size == 0);

	if (unlikely(!perthread_read(thread_init_done)))
		return;

	/* NUMA node checks */
	assert(n->numa_node == this_numa_node());
	assert(addr_to_numa_node(item) == this_numa_node());

	/* page checks */
	assert(is_page_addr(item));
	assert(pg->flags & PAGE_FLAG_SLAB);
	assert(pg->snode == n);
}

void slab_alloc_check(struct slab_node *n, void *item)
{
	slab_item_check(n, item);

	/* poison the item */
	memset(item, 0xAB, n->size);
}

void slab_free_check(struct slab_node *n, void *item)
{
	slab_item_check(n, item);

	/* poison the item */
	memset(item, 0xCD, n->size);
}

#else /* DEBUG */

static void slab_alloc_check(struct slab_node *n, void *item) {;}
static void slab_free_check(struct slab_node *n, void *item) {;}

#endif /* DEBUG */

static struct page *__slab_node_get_page(struct slab_node *n)
{
	struct page *pg = list_pop(&n->partial_list, struct page, link);
	if (!pg) {
		int pgsize = (n->flags & SLAB_FLAG_LGPAGE) ?
			     PGSIZE_2MB : PGSIZE_4KB;

		pg = page_alloc_on_node(pgsize, n->numa_node);
		if (likely(pg)) {
			pg->flags |= PAGE_FLAG_SLAB;
			if (n->flags & SLAB_FLAG_PAGES) {
				pg->flags |= PAGE_FLAG_SHATTERED;
				memset(page_to_addr(pg), 0, n->offset);
			}
			pg->snode = n;
			n->pg_off = n->offset;
			pg->item_count = n->nr_elems;
			pg->next = NULL;
		}
	}

	return pg;
}

static void *__slab_node_alloc(struct slab_node *n)
{
	struct slab_hdr *hdr;

	assert_spin_lock_held(&n->page_lock);

	if (!n->cur_pg || !n->cur_pg->item_count) {
		if (n->cur_pg)
			list_add(&n->full_list, &n->cur_pg->link);
		n->cur_pg = __slab_node_get_page(n);

		/* ran out of memory */
		if (unlikely(!n->cur_pg))
			return NULL;
		n->nr_pages++;
	}

	if (n->cur_pg->next) {
		hdr = (struct slab_hdr *)n->cur_pg->next;
		n->cur_pg->next = (void *)hdr->next_hdr;
	} else {
		assert(n->pg_off < ((n->flags & SLAB_FLAG_LGPAGE) ?
				    PGSIZE_2MB : PGSIZE_4KB));
		hdr = (struct slab_hdr *)
			((char *)page_to_addr(n->cur_pg) + n->pg_off);
		n->pg_off += n->size;
	}

	n->cur_pg->item_count--;
	return (void *)hdr;
}

/**
 * slab_alloc_on_node - allocates an item from a slab
 * @s: the slab
 * @numa_node: the numa node
 *
 * Returns an item, or NULL if out of memory.
 */
void *slab_alloc_on_node(struct slab *s, int numa_node)
{
	struct slab_node *n = s->nodes[numa_node];
	void *item;

	spin_lock(&n->page_lock);
	item = __slab_node_alloc(n);
	spin_unlock(&n->page_lock);

	slab_alloc_check(n, item);

	return item;
}

static void slab_node_free(struct slab_node *n, void *item)
{
	struct page *pg;
	struct slab_hdr *hdr = (struct slab_hdr *)item;
	bool free = false;

	if (n->flags & SLAB_FLAG_LGPAGE)
		pg = addr_to_lgpage(item);
	else
		pg = addr_to_smpage(item);

	spin_lock(&n->page_lock);
	hdr->next_hdr = pg->next;
	pg->next = hdr;
	pg->item_count++;

	if (pg == n->cur_pg) {
		spin_unlock(&n->page_lock);
		return;
	}

	if (pg->item_count == SLAB_PARTIAL_THRESH) {
		list_del(&pg->link);
		list_add(&n->partial_list, &pg->link);
	} else if (pg->item_count == n->nr_elems) {
		list_del(&pg->link);
		free = true;
	}
	spin_unlock(&n->page_lock);

	if (free) {
		page_put(pg);
		n->nr_pages--;
	}
}

/**
 * slab_free frees an item to a slab
 * @s: the slab
 * @item: the item
 */
void slab_free(struct slab *s, void *item)
{
	struct slab_node *n = s->nodes[addr_to_numa_node(item)];
	slab_free_check(n, item);
	slab_node_free(n, item);
}

static int slab_tcache_alloc(struct tcache *tc, int nr, void **items)
{
	struct slab *s = (struct slab *)tc->data;
	struct slab_node *n = s->nodes[this_numa_node()];
	int i;

	spin_lock(&n->page_lock);
	for (i = 0; i < nr; i++) {
		items[i] = __slab_node_alloc(n);
		if (unlikely(!items[i])) {
			spin_unlock(&n->page_lock);
			goto fail;
		}
	}
	spin_unlock(&n->page_lock);

	return 0;

fail:
	for (i--; i >= 0; i--)
		slab_node_free(n, items[i]);
	return -ENOMEM;
}

static void slab_tcache_free(struct tcache *tc, int nr, void **items)
{
	struct slab *s = (struct slab *)tc->data;
	struct slab_node *n = s->nodes[this_numa_node()];
	int i;

	for (i = 0; i < nr; i++)
		slab_node_free(n, items[i]);

}

static const struct tcache_ops slab_tcache_ops = {
	.alloc	= slab_tcache_alloc,
	.free	= slab_tcache_free,
}; 

/**
 * slab_create_tcache - creates a thread-local cache of slab items
 * @s: the backing slab
 * @mag_size: the number of items in a magazine
 *
 * Returns a thread-local cache, or NULL if out of memory.
 */
struct tcache *
slab_create_tcache(struct slab *s, unsigned int mag_size)
{
	struct tcache *tc;

	tc = tcache_create(s->name, &slab_tcache_ops, mag_size, s->size);
	tc->data = (unsigned long)s;
	return tc;
}

/**
 * slab_print_usage - prints the amount of memory used in each slab
 */
void slab_print_usage(void)
{
	struct slab *s;
	size_t total = 0;
	int i;

	log_info("slab: usage statistics...");

	spin_lock(&slab_lock);
	list_for_each(&slab_list, s, link) {
		size_t usage = 0;

		for (i = 0; i < numa_count; i++) {
			struct slab_node *n = s->nodes[i];

			if (n->flags & SLAB_FLAG_LGPAGE) {
				usage += n->nr_pages * PGSIZE_2MB;
				total += n->nr_pages * PGSIZE_2MB;
			} else {
				usage += n->nr_pages * PGSIZE_4KB;
			}
		}

		log_info("%8ld KB\t%s", usage / 1024, s->name);
	}
	spin_unlock(&slab_lock);

	log_info("total: %ld KB", total / 1024);
}

/**
 * slab_init - initializes the slab subsystem
 *
 * NOTE: assumes that pages have already been initialized.
 */
int slab_init(void)
{
	int ret;

	/*
	 * The node and smpage slabs depend on each other so
	 * we bootstrap them here.
	 */
	__slab_early_create(&node_slab, early_slab_nodes, "slab_node",
			    align_up(sizeof(struct slab_node),
				     TCACHE_MIN_ITEM_SIZE),
			    0, 0, PGSIZE_4KB / sizeof(struct slab_node));

	__slab_early_create(&smpage_slab, early_smpage_nodes, "smpage",
			    PGSIZE_4KB, SMPAGE_META_LEN,
			    (SLAB_FLAG_LGPAGE | SLAB_FLAG_PAGES),
			    (PGSIZE_2MB - SMPAGE_META_LEN) / PGSIZE_4KB);

	/*
	 * And now we migrate them to data structures with
	 * the proper numa affinity.
	 */
	ret = __slab_early_migrate(&node_slab);
	if (ret)
		return ret;

	ret = __slab_early_migrate(&smpage_slab);
	if (ret)
		return ret;

	/*
	 * And then finally, create the thread-local cache
	 * for small pages.
	 */
	smpage_tcache = slab_create_tcache(&smpage_slab, SMPAGE_MAG_SIZE);
	if (!smpage_tcache)
		return -ENOMEM; 

	return 0;
}
