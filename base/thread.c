/*
 * thread.c - support for thread-local storage and initialization
 */

#include <unistd.h>
#include <limits.h>
#include <asm/prctl.h>
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
DEFINE_PERTHREAD(void *, perthread_ptr);

DEFINE_PERTHREAD(unsigned int, thread_numa_node);
DEFINE_PERTHREAD(unsigned int, thread_id);
DEFINE_PERTHREAD(bool, thread_init_done);

extern const char __perthread_start[];
extern const char __perthread_end[];

static int thread_alloc_perthread(unsigned int thread_id, unsigned int node)
{
	int ret;
	void *addr;
	size_t len = __perthread_end - __perthread_start;
	unsigned long gsbase;

	/* no perthread data */
	if (!len)
		return 0;

	addr = mem_map_anom(NULL, len, PGSIZE_4KB, node);
	if (addr == MAP_FAILED)
		return -ENOMEM;

	memset(addr, 0, len);

	/**
	 * perthread addresses will be relative to the perthread section,
	 * subtracting __perthread_start allows us to directly use the
	 * variable pointers as offsets from gs
	 **/
	gsbase = (unsigned long)addr - (unsigned long)__perthread_start;

	/* store a copy of gsbase so we can compute TLS addresses */
	*(unsigned long *)(gsbase + (uintptr_t)&__perthread_perthread_ptr) = gsbase;

	ret = syscall(SYS_arch_prctl, ARCH_SET_GS, gsbase);
	if (ret) {
		log_err("thread_alloc_perthread: failed to set gs (errno %d)", errno);
		return ret;
	}

	perthread_offsets[thread_id] = (void *)gsbase;
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
	unsigned int thread_id;

	/*
	 * this function may be called multiple times since
	 * base_init() must initialize perthread internally
	 */
	static __thread bool internal_thread_init_done;
	if (internal_thread_init_done)
		return 0;
	internal_thread_init_done = true;

	spin_lock(&thread_lock);
	if (thread_count >= NTHREAD) {
		spin_unlock(&thread_lock);
		log_err("thread: hit thread limit of %d\n", NTHREAD);
		return -ENOSPC;
	}
	thread_id = thread_count++;
	spin_unlock(&thread_lock);

	ret = thread_alloc_perthread(thread_id, 0);
	if (ret)
		return ret;

	/* TODO: figure out how to support NUMA */
	perthread_store(thread_numa_node, 0);

	perthread_store(thread_id, thread_id);

	log_info("thread: created thread %d", thread_id);
	return 0;
}
