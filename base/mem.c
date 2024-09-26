/*
 * mem.c - memory management
 */

#include <asm/mman.h>
#include <linux/mman.h>
#include <linux/shm.h>

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <numaif.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#include <base/stddef.h>
#include <base/mem.h>
#include <base/log.h>
#include <base/limits.h>

#if !defined(MAP_HUGE_2MB) || !defined(MAP_HUGE_1GB)
#warning "Your system does not support specifying MAP_HUGETLB page sizes"
#endif

#if !defined(SHM_HUGE_2MB) || !defined(SHM_HUGE_1GB)
#warning "Your system does not support specifying SHM_HUGETLB page sizes"
#endif

bool cfg_transparent_hugepages_enabled;

/* libc conflicts with linux/shm.h, so define these ourselves */
void* shmat(int shm_id, const void *addr, int flags);
int shmctl(int shm_id, int cmd, struct shmid_ds* buf);
int shmdt(const void *addr);
int shmget(key_t key, size_t size, int flags);

long mbind(void *start, size_t len, int mode,
	   const unsigned long *nmask, unsigned long maxnode,
	   unsigned flags)
{
	return syscall(__NR_mbind, start, len, mode, nmask, maxnode, flags);
}

static void sigbus_error(int sig)
{
	panic("couldn't map pages");
}

static void touch_mapping(void *base, size_t len, size_t pgsize)
{
	__sighandler_t s;
	char *pos;

	/*
	 * Unfortunately mmap() provides no error message if MAP_POPULATE fails
	 * because of insufficient memory. Therefore, we manually force a write
	 * on each page to make sure the mapping was successful.
	 */
	s = signal(SIGBUS, sigbus_error);
	for (pos = (char *)base; pos < (char *)base + len; pos += pgsize)
		ACCESS_ONCE(*pos);
	signal(SIGBUS, s);
} 

static void *
__mem_map_anom(void *base, size_t len, size_t pgsize,
	       unsigned long *mask, int numa_policy)
{
	void *addr;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;

	len = align_up(len, pgsize);

	if (base)
		flags |= MAP_FIXED;

	switch (pgsize) {
	case PGSIZE_4KB:
	    break;
	case PGSIZE_2MB:
	    if (!cfg_transparent_hugepages_enabled) {
	        flags |= MAP_HUGETLB;
#ifdef MAP_HUGE_2MB
	        flags |= MAP_HUGE_2MB;
#endif
	    }
	    break;
	case PGSIZE_1GB:
#ifdef MAP_HUGE_1GB
	    if (!cfg_transparent_hugepages_enabled)
	        flags |= MAP_HUGETLB | MAP_HUGE_1GB;
#else
	    return MAP_FAILED;
#endif
	    break;
	default: /* fail on other sizes */
	  return MAP_FAILED;
	}

	addr = mmap(base, len, PROT_READ | PROT_WRITE, flags, -1, 0);
	if (addr == MAP_FAILED)
		return MAP_FAILED;

	BUILD_ASSERT(sizeof(unsigned long) * 8 >= NNUMA);
	if (mbind(addr, len, numa_policy, mask ? mask : NULL,
		  mask ? NNUMA + 1 : 0, MPOL_MF_STRICT | MPOL_MF_MOVE))
		goto fail;

	if (cfg_transparent_hugepages_enabled && (pgsize > PGSIZE_4KB)) {
	  if (madvise(addr, len, MADV_HUGEPAGE))
	    goto fail;
	}

	touch_mapping(addr, len, pgsize);
	return addr;

fail:
	munmap(addr, len);
	return MAP_FAILED;
}

/**
 * mem_map_anom - map anonymous memory pages
 * @base: the base address (or NULL for automatic)
 * @len: the length of the mapping
 * @pgsize: the page size
 * @node: the NUMA node
 *
 * Returns the base address, or MAP_FAILED if out of memory
 */
void *mem_map_anom(void *base, size_t len, size_t pgsize, int node)
{
	unsigned long mask = (1 << node);
	return __mem_map_anom(base, len, pgsize, &mask, MPOL_BIND);
}

/**
 * mem_map_file - maps a file into memory
 * @base: the address (or automatic if NULL)
 * @len: the length in bytes
 * @fd: the file descriptor
 * @offset: the offset inside the file
 *
 * Returns the address of the mapping or MAP_FAILED if failure.
 */
void *mem_map_file(void *base, size_t len, int fd, off_t offset)
{
	return mmap(base, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, offset);
}


static void *__mem_map_shm(mem_key_t key, void *base, size_t len,
		  size_t pgsize, bool exclusive, bool rdonly);

/**
 * mem_map_shm - maps a System V shared memory segment
 * @key: the unique key that identifies the shared region (e.g. use ftok())
 * @base: the base address to map the shared segment (or automatic if NULL)
 * @len: the length of the mapping
 * @pgsize: the size of each page
 * @exclusive: ensure this call creates the shared segment
 *
 * Returns a pointer to the mapping, or NULL if the mapping failed.
 */
void *mem_map_shm(mem_key_t key, void *base, size_t len, size_t pgsize,
		  bool exclusive)
{
	return __mem_map_shm(key, base, len, pgsize, exclusive, false);
}

void *mem_map_shm_rdonly(mem_key_t key, void *base, size_t len,
		  size_t pgsize)
{
	return __mem_map_shm(key, base, len, pgsize, false, true);
}

static void *__mem_map_shm(mem_key_t key, void *base, size_t len,
		  size_t pgsize, bool exclusive, bool rdonly)
{
	void *addr;
	int shmid, flags = rdonly ? 0 : (IPC_CREAT | 0744);

	BUILD_ASSERT(sizeof(mem_key_t) == sizeof(key_t));

	switch (pgsize) {
	case PGSIZE_4KB:
		break;
	case PGSIZE_2MB:
		flags |= SHM_HUGETLB;
#ifdef SHM_HUGE_2MB
		flags |= SHM_HUGE_2MB;
#endif
		break;
	case PGSIZE_1GB:
#ifdef SHM_HUGE_1GB
		flags |= SHM_HUGETLB | SHM_HUGE_1GB;
#else
		return MAP_FAILED;
#endif
		break;
	default: /* fail on other sizes */
		return MAP_FAILED;
	}

	if (exclusive)
		flags |= IPC_EXCL;

	shmid = shmget(key, len, flags);
	if (shmid == -1)
		return MAP_FAILED;

	flags = rdonly ? SHM_RDONLY : 0;
	addr = shmat(shmid, base, flags);
	if (addr == MAP_FAILED)
		return MAP_FAILED;

	touch_mapping(addr, len, pgsize);
	return addr;
}

/**
 * mem_unmap_shm - detach a shared memory mapping
 * @addr: the base address of the mapping
 *
 * Returns 0 if successful, otherwise fail.
 */
int mem_unmap_shm(void *addr)
{
	if (shmdt(addr) == -1)
		return -errno;
	return 0;
}

#define PAGEMAP_PGN_MASK	0x7fffffffffffffULL
#define PAGEMAP_FLAG_PRESENT	(1ULL << 63)
#define PAGEMAP_FLAG_SWAPPED	(1ULL << 62)
#define PAGEMAP_FLAG_FILE	(1ULL << 61)
#define PAGEMAP_FLAG_SOFTDIRTY	(1ULL << 55)

/**
 * mem_lookup_page_phys_addrs - determines the physical address of pages
 * @addr: a pointer to the start of the pages (must be @size aligned)
 * @len: the length of the mapping
 * @pgsize: the page size (4KB, 2MB, or 1GB)
 * @paddrs: a pointer store the physical addresses (of @nr elements)
 *
 * Returns 0 if successful, otherwise failure.
 */
int mem_lookup_page_phys_addrs(void *addr, size_t len,
			       size_t pgsize, physaddr_t *paddrs)
{
	uintptr_t pos;
	uint64_t tmp;
	int fd, i = 0, ret = 0;

	/*
	 * 4 KB pages could be swapped out by the kernel, so it is not
	 * safe to get a machine address. If we later decide to support
	 * 4KB pages, then we need to mlock() the page first.
	 */
	if (pgsize == PGSIZE_4KB)
		return -EINVAL;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0)
		return -EIO;

	for (pos = (uintptr_t)addr; pos < (uintptr_t)addr + len;
	     pos += pgsize) {
		if (lseek(fd, pos / PGSIZE_4KB * sizeof(uint64_t), SEEK_SET) ==
		    (off_t)-1) {
			ret = -EIO;
			goto out;
		}
		if (read(fd, &tmp, sizeof(uint64_t)) <= 0) {
			ret = -EIO;
			goto out;
		}
		if (!(tmp & PAGEMAP_FLAG_PRESENT)) {
			ret = -ENODEV;
			goto out;
		}

		paddrs[i++] = (tmp & PAGEMAP_PGN_MASK) * PGSIZE_4KB;
	}

out:
	close(fd);
	return ret;
}
