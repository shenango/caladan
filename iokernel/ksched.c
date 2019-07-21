/*
 * ksched.c - an interface to the ksched kernel module
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "ksched.h"

/* a file descriptor handle to the ksched kernel module */
int ksched_fd;
/* the number of pending interrupts */
int ksched_count;
/* the shared memory region with the kernel module */
struct ksched_shm_cpu *ksched_shm;
/* the set of pending cores to send interrupts to */
cpu_set_t ksched_set;
/* the generation number for each core */
unsigned int ksched_gens[NCPU];

int pci_cfg_fd;
char *pci_cfg_base_ptr;
unsigned int *low_cas_count_all_ptr;

/**
 * ksched_init - initializes the ksched kernel module interface
 *
 * Returns 0 if successful.
 */
int ksched_init(void)
{
	char *ksched_addr;
	int i;

	/* first open the file descriptor */
	ksched_fd = open("/dev/ksched", O_RDWR);
	if (ksched_fd < 0)
		return -errno;
	
	/* then map the shared memory region with the kernel */
	ksched_addr = mmap(NULL, sizeof(struct ksched_shm_cpu) * NCPU,
		    PROT_READ | PROT_WRITE, MAP_SHARED, ksched_fd, 0);
	if (ksched_addr == MAP_FAILED)
		return -errno;

	/* then initialize the generation numbers */
	ksched_shm = (struct ksched_shm_cpu *)ksched_addr;
	for (i = 0; i < NCPU; i++) {
		ksched_gens[i] = load_acquire(&ksched_shm[i].last_gen);
		ksched_idle_hint(i, 0);
	}

	/* mmap pci cfg space to userspace, this is found to be way more faster than 
           using perf or pread/pwrite to read IMC counters. */
        pci_cfg_fd = open("/dev/pcicfg", O_RDWR);
        if (pci_cfg_fd < 0)
                return -errno;
        pci_cfg_base_ptr = (char *)mmap(NULL, PCI_CFG_SIZE, PROT_READ | PROT_WRITE,
					MAP_SHARED, pci_cfg_fd, 0);
	
	/* program MC_CHy_PCI_PMON_CTL0, we only monitor one channel (out of 2 in zig/zag) 
           of socket 0. But all DRAM traffics (read + write) are monitored */
        unsigned int *ctrl_ptr =
                (unsigned int *)(pci_cfg_base_ptr +
                             pci_cfg_index(SOCKET0_IMC_BUS_NO,
                                           SOCKET0_IMC_DEVICE_NO,
                                           SOCKET0_CHANNEL0_IMC_FUNC_NO,
                                           MC_CHy_PCI_PMON_CTL0));
	*ctrl_ptr = (EVENT_CODE_CAS_COUNT << MC_CHy_PCI_PMON_CTL_ENV_SEL_SHIFT) |
		(UMASK_CAS_COUNT_ALL << MC_CHy_PCI_PMON_CTL_UMASK_SHIFT) |
		(1 /* enabled */ << MC_CHy_PCI_PMON_CTL_ENABLE_SHIFT);

	/* a small trick is that we only monitor the low 32-bit reg in order 
           to reduce the overhead. */
	low_cas_count_all_ptr =
		(unsigned int *)(pci_cfg_base_ptr +
                             pci_cfg_index(SOCKET0_IMC_BUS_NO,
                                           SOCKET0_IMC_DEVICE_NO,
                                           SOCKET0_CHANNEL0_IMC_FUNC_NO,
                                           MC_CHy_PCI_PMON_CTR0_LOW));

        return 0;
}

