/*
 * page.h - page-level memory management
 */

#pragma once

#include <base/stddef.h>
#include <base/mem.h>
#include <base/list.h>
#include <base/limits.h>
#include <base/kref.h>

struct slab_node;

struct page {
	int 			flags;
	struct kref		ref;
	void			*next;
	struct list_node	link;
	struct slab_node 	*snode;
	off_t			offset;
	long			pad;
	long			item_count;
};

BUILD_ASSERT(PGSIZE_2MB % sizeof(struct page) == 0);

#define PAGE_FLAG_LARGE		0x01 /* page is large */
#define PAGE_FLAG_IN_USE	0x02 /* page is allocated */
#define PAGE_FLAG_SLAB		0x04 /* page is used by SLAB */
#define PAGE_FLAG_SHATTERED	0x08 /* page is 2MB shattered into 4KB */
#define PAGE_FLAG_PGDIR		0x10 /* page is being used as a PDE */

/* meta-data length for small pages */
#define SMPAGE_META_LEN		(PGSIZE_2MB / PGSIZE_4KB * sizeof(struct page))

/* meta-data large page count for large pages (per NUMA node) */
#define LGPAGE_META_NR_LGPAGES	1

/* meta-data length for large pages (per NUMA node) */
#define LGPAGE_META_LEN		(LGPAGE_META_NR_LGPAGES * PGSIZE_2MB)

/* the number of large page meta-data entries (per NUMA node) */
#define LGPAGE_META_ENTS	(LGPAGE_META_LEN / sizeof(struct page))

/* the size of the page map address space (per NUMA node) */
#define LGPAGE_NODE_ADDR_LEN	(LGPAGE_META_ENTS * PGSIZE_2MB)

/* per NUMA node page tables are stored in a contiguous array */
extern struct page *page_tbl;

#define PAGE_BASE_ADDR	0x100000000000UL  /* the start of page mappings */
#define PAGE_END_ADDR	(PAGE_BASE_ADDR + LGPAGE_META_ENTS * PGSIZE_2MB * NNUMA)

/**
 * is_page_addr - determines if an address is inside page memory
 * @addr: the address of the page
 *
 * Returns true if the address is inside page memory.
 */
static inline bool is_page_addr(void *addr)
{
	return ((uintptr_t)addr >= PAGE_BASE_ADDR &&
		(uintptr_t)addr < PAGE_END_ADDR);
}

/**
 * addr_to_numa_node - gets the NUMA node of a page's address
 * @addr: the page's address
 *
 * Returns a numa node
 */
static inline unsigned int addr_to_numa_node(void *addr)
{
	return ((uintptr_t)addr - PAGE_BASE_ADDR) / LGPAGE_NODE_ADDR_LEN;
}

#define LGPGN(addr) \
	PGN_2MB((uintptr_t)(addr) - (uintptr_t)PAGE_BASE_ADDR)
#define PGN(addr) \
	PGN_4KB((uintptr_t)(addr) - PGADDR_2MB(addr))

/**
 * pa_to_lgpage - gets the large page struct for an address
 * @addr: the address
 *
 * Returns a pointer to the large page struct
 */
static inline struct page *addr_to_lgpage(void *addr)
{
	return page_tbl + LGPGN(addr);
}

/**
 * lgpage_to_addr - gets the address for a large page struct
 * @pg: the large page struct
 *
 * Returns the address
 */
static inline void *lgpage_to_addr(struct page *pg)
{
	return (void *)(PAGE_BASE_ADDR + (pg - page_tbl) * PGSIZE_2MB);
}

/**
 * addr_to_smpage - gets the small page struct for an address
 * @addr: the address
 *
 * Returns a pointer to the page struct
 */
static inline struct page *addr_to_smpage(void *addr)
{
	struct page *frags = (struct page *)PGADDR_2MB(addr);
	return &frags[PGN(addr)];
}

/**
 * smpage_to_addr - gets the address for a small page struct
 * @pg: the small page struct
 *
 * Returns an address
 */
static inline void *smpage_to_addr(struct page *pg)
{
	struct page *frags = (struct page *)PGADDR_2MB(pg);
	return (void *)(PGADDR_2MB(pg) + (pg - frags) * PGSIZE_4KB);
}

/**
 * addr_to_page - gets the page struct for an address
 * @addr: the address
 *
 * Returns a pointer to the page struct
 */
static inline struct page *addr_to_page(void *addr)
{
	struct page *pg = addr_to_lgpage(addr);
	assert(pg->flags & PAGE_FLAG_LARGE);

	if (pg->flags & PAGE_FLAG_SHATTERED)
		return addr_to_smpage(addr);
	return pg;
}

/**
 * page_to_addr - gets the address for a page struct
 * @pg: the page struct
 *
 * Returns the address
 */
static inline void *page_to_addr(struct page *pg)
{
	if (pg->flags & PAGE_FLAG_LARGE)
		return lgpage_to_addr(pg);
	return smpage_to_addr(pg);
}

/**
 * smpage_to_lgpage - retrieves the large page struct for a 4kb page
 * @pg: the page
 *
 * Returns a pointer to the lgpage struct.
 */
static inline struct page *smpage_to_lgpage(struct page *pg)
{
	assert(!(pg->flags & PAGE_FLAG_LARGE));
	return addr_to_lgpage((void *)pg);
}

/**
 * page_to_size - gets the size of the page (in bytes)
 * @pg: the page
 *
 * Returns the size in bytes.
 */
static inline size_t page_to_size(struct page *pg)
{
	return (pg->flags & PAGE_FLAG_LARGE) ? PGSIZE_2MB : PGSIZE_4KB;
}


/*
 * Page allocation
 */

/* function attributes to optimize for malloc() behavior */
#define __page_malloc __malloc __assume_aligned(PGSIZE_4KB)

extern struct page *page_alloc_on_node(size_t pgsize, int numa_node);
extern struct page *page_alloc(size_t pgsize);
extern struct page *page_zalloc(size_t pgsize);
extern void *page_alloc_addr_on_node(size_t pgsize, int numa_node) __page_malloc;
extern void *page_alloc_addr(size_t pgsize) __page_malloc;
extern void *page_zalloc_addr_on_node(size_t pgsize, int numa_node) __page_malloc;
extern void *page_zalloc_addr(size_t pgsize) __page_malloc;
extern void page_put_addr(void *addr);
extern void page_release(struct kref *ref);

/**
 * page_get - increments the page reference count
 * @pg: the page to reference
 *
 * Returns the page.
 */
static inline struct page *page_get(struct page *pg)
{
	kref_get(&pg->ref);
	return pg;
}

/**
 * page_put - decrements the page reference count, freeing it at zero
 * @pg: the page to unreference
 */
static inline void page_put(struct page *pg)
{
	kref_put(&pg->ref, page_release);
}
