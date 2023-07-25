/*
 * tcache.h - a generic per-thread item cache based on magazines
 */

#pragma once

#include <base/stddef.h>
#include <base/lock.h>
#include <base/list.h>
#include <base/atomic.h>
#include <base/thread.h>

#define TCACHE_MAX_MAG_SIZE	64
#define TCACHE_DEFAULT_MAG_SIZE	32

struct tcache;

struct tcache_hdr {
	struct tcache_hdr	*next_item;
	struct tcache_hdr	*next_mag;
};

#define TCACHE_MIN_ITEM_SIZE	sizeof(struct tcache_hdr)

struct tcache_ops {
	int (*alloc)(struct tcache *tc, int nr, void **items);
	void (*free)(struct tcache *tc, int nr, void **items);
};

struct tcache_perthread {
	struct tcache		*tc;
	unsigned int		rounds;
	unsigned int		capacity;
	struct tcache_hdr	*loaded;
	struct tcache_hdr	*previous;
};

struct tcache {
	const char		*name;
	const struct tcache_ops	*ops;
	size_t			item_size;
	atomic64_t		mags_allocated;
	struct list_node	link;

	unsigned int		mag_size;
	spinlock_t		lock;
	struct tcache_hdr	*shared_mags;
	unsigned long		data;
};

extern void *__tcache_alloc(struct tcache_perthread *ltc);
extern void __tcache_free(struct tcache_perthread *ltc, void *item);

/*
 * stat counters
 */
DECLARE_PERTHREAD(uint64_t, mag_alloc);
DECLARE_PERTHREAD(uint64_t, mag_free);
DECLARE_PERTHREAD(uint64_t, pool_alloc);
DECLARE_PERTHREAD(uint64_t, pool_free);

/**
 * tcache_alloc - allocates an item from the thread cache
 * @ltc: the thread-local cache
 *
 * Returns an item, or NULL if out of memory.
 */
static inline void *tcache_alloc(struct tcache_perthread *ltc)
{
	void *item = (void *)ltc->loaded;

	if (ltc->rounds == 0)
		return __tcache_alloc(ltc);

	ltc->rounds--;
	ltc->loaded = ltc->loaded->next_item;
	return item;
}

/**
 * tcache_free - frees an item to the thread cache
 * @ltc: the thread-local cache
 * @item: the item to free
 */
static inline void tcache_free(struct tcache_perthread *ltc, void *item)
{
	struct tcache_hdr *hdr = (struct tcache_hdr *)item;

	if (ltc->rounds >= ltc->capacity)
		return __tcache_free(ltc, item);

	ltc->rounds++;
	hdr->next_item = ltc->loaded;
	ltc->loaded = hdr;
}

extern struct tcache *tcache_create(const char *name,
				    const struct tcache_ops *ops,
				    unsigned int mag_size, size_t item_size);
extern void tcache_init_perthread(struct tcache *tc,
				  struct tcache_perthread *ltc);
extern void tcache_reclaim(struct tcache *tc);
extern void tcache_print_usage(void);
