/*
 * ksched.c - an interface to the ksched kernel module
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "ksched.h"

/* a file descriptor handle to the ksched kernel module */
int ksched_fd;
/* the shared memory region with the kernel module */
struct ksched_shm_cpu *ksched_shm;
/* the set of pending cores to send interrupts to */
cpu_set_t ksched_set;
/* the generation number for each core */
unsigned int ksched_gens[NCPU];

/**
 * ksched_init - initializes the ksched kernel module interface
 *
 * Returns 0 if successful.
 */
int ksched_init(void)
{
	char *addr;
	int i;

	/* first open the file descriptor */
	ksched_fd = open("/dev/ksched", O_RDWR);
	if (ksched_fd < 0)
		return -errno;

	/* then map the shared memory region with the kernel */
	addr = mmap(NULL, sizeof(struct ksched_shm_cpu) * NCPU,
		    PROT_READ | PROT_WRITE, MAP_PRIVATE, ksched_fd, 0);
	if (addr == MAP_FAILED)
		return -errno;

	/* then initialize the generation numbers */
	ksched_shm = (struct ksched_shm_cpu *)addr;
	for (i = 0; i < NCPU; i++)
		ksched_gens[i] = load_acquire(&ksched_shm[i].last_gen);

	return 0;
}
