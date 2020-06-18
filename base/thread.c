/*
 * thread.c - support for thread-local storage and initialization
 */

#include <unistd.h>
#include <limits.h>
#include <sys/syscall.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/cpu.h>
#include <base/thread.h>
#include <base/mem.h>
#include <base/init.h>
#include <base/lock.h>

/* protects thread_count */
static DEFINE_SPINLOCK(thread_lock);

unsigned int thread_count;
void *perthread_offsets[NTHREAD];
__thread void *perthread_ptr;

__thread unsigned int thread_numa_node;
__thread unsigned int thread_id;
__thread bool thread_init_done;

extern const char __perthread_start[];
extern const char __perthread_end[];

static int thread_alloc_perthread(void)
{
	void *addr;
	size_t len = __perthread_end - __perthread_start;

	/* no perthread data */
	if (!len)
		return 0;

	addr = mem_map_anom(NULL, len, PGSIZE_4KB, thread_numa_node);
	if (addr == MAP_FAILED)
		return -ENOMEM;

	memset(addr, 0, len);
	perthread_ptr = addr;
	perthread_offsets[thread_id] = addr;
	return 0;
}

/**
 * thread_gettid - gets the tid of the current kernel thread
 */
pid_t thread_gettid(void)
{
#ifndef SYS_gettid
	#error "SYS_gettid unavailable on this system"
#endif
	return syscall(SYS_gettid);
}

/**
 * thread_init_perthread - initializes a thread
 *
 * Returns 0 if successful, otherwise fail.
 */
int thread_init_perthread(void)
{
	int ret;

	spin_lock(&thread_lock);
	if (thread_count >= NTHREAD) {
		spin_unlock(&thread_lock);
		log_err("thread: hit thread limit of %d\n", NTHREAD);
		return -ENOSPC;
	}
	thread_id = thread_count++;
	spin_unlock(&thread_lock);

	/* TODO: figure out how to support NUMA */
	thread_numa_node = 0;

	ret = thread_alloc_perthread();
	if (ret)
		return ret;

	log_info("thread: created thread %d", thread_id);
	return 0;
}
