/*
 * tcache.c - a generic thread-local item cache
 *
 * Based heavily on Magazines and Vmem: Extending the Slab Allocator to Many
 * CPUs and Arbitrary Resources. Jeff Bonwick and Johnathan Adams.
 *
 * TODO: Improve NUMA awareness.
 * TODO: Provide an interface to tear-down thread caches.
 * TODO: Remove dependence on libc malloc().
 * TODO: Use RCU for tcache list so printing stats doesn't block creating
 * new tcaches.
 */

#include <stdlib.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/lock.h>
#include <base/tcache.h>
#include <base/thread.h>

static DEFINE_SPINLOCK(tcache_lock);
static LIST_HEAD(tcache_list);

DEFINE_PERTHREAD(uint64_t, mag_alloc);
DEFINE_PERTHREAD(uint64_t, mag_free);
DEFINE_PERTHREAD(uint64_t, pool_alloc);
DEFINE_PERTHREAD(uint64_t, pool_free);

static struct tcache_hdr *tcache_alloc_mag(struct tcache *tc)
{
	void *items[TCACHE_MAX_MAG_SIZE];
	struct tcache_hdr *head, **pos;
	int err, i;

	perthread_incr(mag_alloc);

	err = tc->ops->alloc(tc, tc->mag_size, items);
	if (err)
		return NULL;

	head = (struct tcache_hdr *)items[0];
	pos = &head->next_item;
	for (i = 1; i < tc->mag_size; i++) {
		*pos = (struct tcache_hdr *)items[i];
		pos = &(*pos)->next_item;
	}

	*pos = NULL;
	atomic64_inc(&tc->mags_allocated);
	return head;
}

static void tcache_free_mag(struct tcache *tc, struct tcache_hdr *hdr)
{
	void *items[TCACHE_MAX_MAG_SIZE];
	int nr = 0;

	perthread_incr(mag_free);

	do {
		items[nr++] = hdr;
		hdr = hdr->next_item;
	} while (hdr);

	assert(nr == tc->mag_size);
	tc->ops->free(tc, nr, items);
	atomic64_dec(&tc->mags_allocated);
}

/* The thread-local cache allocation slow path. */
void *__tcache_alloc(struct tcache_perthread *ltc)
{
	struct tcache *tc = ltc->tc;
	void *item;

	/* must be out of rounds */
	assert(ltc->rounds == 0);
	assert(ltc->loaded == NULL);

	/* CASE 1: exchange empty loaded mag with full previous mag */
	if (ltc->previous) {
		ltc->loaded = ltc->previous;
		ltc->previous = NULL;
		goto alloc;
	}

	perthread_incr(pool_alloc);

	/* CASE 2: grab a magazine from the shared pool */
	spin_lock(&tc->lock);
	ltc->loaded = tc->shared_mags;
	if (tc->shared_mags)
		tc->shared_mags = tc->shared_mags->next_mag;
	spin_unlock(&tc->lock);
	if (ltc->loaded)
		goto alloc;

	/* CASE 3: allocate a new magazine */
	ltc->loaded = tcache_alloc_mag(tc);
	if (unlikely(!ltc->loaded))
		return NULL;

alloc:
	/* reload the magazine and allocate an item */
	ltc->rounds = ltc->capacity - 1;
	item = (void *)ltc->loaded;
	ltc->loaded = ltc->loaded->next_item;
	return item;
}

/* The thread-local cache free slow path. */
void __tcache_free(struct tcache_perthread *ltc, void *item)
{
	struct tcache *tc = ltc->tc;
	struct tcache_hdr *hdr = (struct tcache_hdr *)item;

	/* magazine must be full */
	assert(ltc->rounds == ltc->capacity);
	assert(ltc->loaded != NULL);

	/* CASE 1: exchange empty previous mag with full loaded mag */
	if (!ltc->previous) {
		ltc->previous = ltc->loaded;
		goto free;
	}

	perthread_incr(pool_free);

	/* CASE 2: return a magazine to the shared pool */
	spin_lock(&tc->lock);
	ltc->previous->next_mag = tc->shared_mags;
	tc->shared_mags = ltc->previous;
	spin_unlock(&tc->lock);
	ltc->previous = ltc->loaded;

free:
	/* start a new magazine and free the item */
	ltc->rounds = 1;
	ltc->loaded = hdr;
	hdr->next_item = NULL;
}

/**
 * tcache_create - creates a new thread-local cache
 * @name: a human-readable name to identify the cache
 * @ops: operations for allocating and freeing items that back the cache
 * @mag_size: the number of items in a magazine
 * @item_size: the size of each item
 *
 * Returns a thread cache or NULL of out of memory.
 *
 * After creating a thread-local cache, you'll want to attach one or more
 * thread-local handles using tcache_init_perthread().
 */
struct tcache *tcache_create(const char *name, const struct tcache_ops *ops,
			     unsigned int mag_size, size_t item_size)
{
	struct tcache *tc;

	/* we assume the caller is aware of the tcache size limits */
	assert(item_size >= TCACHE_MIN_ITEM_SIZE);
	assert(mag_size <= TCACHE_MAX_MAG_SIZE);

	tc = malloc(sizeof(*tc));
	if (!tc)
		return NULL;

	tc->name = name;
	tc->ops = ops;
	tc->item_size = item_size;
	atomic64_write(&tc->mags_allocated, 0);
	tc->mag_size = mag_size;
	spin_lock_init(&tc->lock);
	tc->shared_mags = NULL;

	spin_lock(&tcache_lock);
	list_add_tail(&tcache_list, &tc->link);
	spin_unlock(&tcache_lock);

	return tc;
}

/**
 * tcache_init_perthread - intializes a per-thread handle for a thread-local
 *                         cache
 * @tc: the thread-local cache
 * @ltc: the per-thread handle
 */
void tcache_init_perthread(struct tcache *tc, struct tcache_perthread *ltc)
{
	ltc->tc = tc;
	ltc->loaded = ltc->previous = NULL;
	ltc->rounds = 0;
	ltc->capacity = tc->mag_size; 
}

/**
 * tcache_reclaim - reclaims unused memory from a thread-local cache
 * @tc: the thread-local cache
 */
void tcache_reclaim(struct tcache *tc)
{
	struct tcache_hdr *hdr, *next;

	spin_lock(&tc->lock);
	hdr = tc->shared_mags;
	tc->shared_mags = NULL;
	spin_unlock(&tc->lock);

	while (hdr) {
		next = hdr->next_mag;
		tcache_free_mag(tc, hdr);
		hdr = next;
	}
}

/**
 * tcache_print_stats - dumps usage statistics about all thread-local caches
 */
void tcache_print_usage(void)
{
	struct tcache *tc;
	size_t total = 0;

	log_info("tcache: dumping usage statistics...");

	spin_lock(&tcache_lock);
	list_for_each(&tcache_list, tc, link) {
		long mags = atomic64_read(&tc->mags_allocated);
		size_t usage = tc->mag_size * tc->item_size * mags;
		log_info("%8ld KB\t%s", usage / 1024, tc->name);
		total += usage;
	}
	spin_unlock(&tcache_lock);

	log_info("total: %8ld KB", total / 1024);
}
