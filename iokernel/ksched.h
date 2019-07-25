/*
 * ksched.h - an interface to the ksched kernel module
 */

#pragma once

#include <sched.h>
#include <sys/ioctl.h>
#include <signal.h>

#include <base/stddef.h>
#include <base/atomic.h>
#include <base/limits.h>

#define __user
#include "../ksched/ksched.h"

// TODO: should not be hard-coded.
#define SOCKET0_IMC_BUS_NO (0x7F)
#define SOCKET0_IMC_DEVICE_NO (0x14)
#define SOCKET0_CHANNEL0_IMC_FUNC_NO (0)
#define SOCKET0_CHANNEL1_IMC_FUNC_NO (1)

#define MC_CHy_PCI_PMON_CTL0 (0xD8)
#define MC_CHy_PCI_PMON_CTL1 (0xDC)
#define MC_CHy_PCI_PMON_CTL2 (0xE0)
#define MC_CHy_PCI_PMON_CTL3 (0xE4)
#define MC_CHy_PCI_PMON_CTR0_LOW (0xA0)
#define MC_CHy_PCI_PMON_CTR0_HIGH (0xA4)
#define MC_CHy_PCI_PMON_CTR1_LOW (0xA8)
#define MC_CHy_PCI_PMON_CTR1_HIGH (0xAC)
#define MC_CHy_PCI_PMON_CTR2_LOW (0xB0)
#define MC_CHy_PCI_PMON_CTR2_HIGH (0xB4)
#define MC_CHy_PCI_PMON_CTR3_LOW (0xB8)
#define MC_CHy_PCI_PMON_CTR3_HIGH (0xBC)

#define MC_CHy_PCI_PMON_CTL_ENV_SEL_SHIFT (0)
#define MC_CHy_PCI_PMON_CTL_UMASK_SHIFT (8)
#define MC_CHy_PCI_PMON_CTL_ENABLE_SHIFT (22)

#define EVENT_CODE_CAS_COUNT (0x04)
#define	UMASK_CAS_COUNT_ALL (0xF)

#define PROBE_MEM_LAT_SAMPLE_NUM (5)

extern int ksched_fd, ksched_count;
extern struct ksched_shm_cpu *ksched_shm;
extern cpu_set_t ksched_set;
extern unsigned int ksched_gens[NCPU];
extern volatile char *ucmem;
extern unsigned int *low_cas_count_all_ptr;

/**
 * ksched_run - runs a kthread on a specific core
 * @core: the core to run a kthread on
 * @tid: the kthread's TID (or zero to idle the core)
 */
static inline void ksched_run(unsigned int core, pid_t tid)
{
	unsigned int gen = ++ksched_gens[core];

	ksched_shm[core].tid = tid;
	store_release(&ksched_shm[core].gen, gen);
}

/**
 * ksched_poll_run_done - determines if the last ksched_run() call finished
 * @core: the core on which kthread_run() was called
 *
 * Returns true if finished.
 */
static inline bool ksched_poll_run_done(unsigned int core)
{
	return load_acquire(&ksched_shm[core].last_gen) == ksched_gens[core];
}

/**
 * ksched_poll_idle - determines if a core is currently idle
 * @core: the core to check if it is idle
 *
 * Returns true if idle.
 */
static inline bool ksched_poll_idle(unsigned int core)
{
	return !load_acquire(&ksched_shm[core].busy);
}

static inline void ksched_idle_hint(unsigned int core, unsigned int hint)
{
	ksched_shm[core].mwait_hint = hint;
}

enum {
	KSCHED_INTR_CEDE = 0,
	KSCHED_INTR_YIELD,
};

/**
 * ksched_enqueue_intr - enqueues an interrupt request on a core
 * @core: the core to interrupt
 * @type: the type of interrupt to enqueue
 *
 * The interrupt will not be sent until ksched_send_intrs(). This is done to
 * create an opportunity for batching interrupts. If ksched_run() is called on
 * the same core after ksched_enqueue_intr(), it may prevent interrupts
 * still pending for the last kthread from being delivered.
 */
static inline void ksched_enqueue_intr(unsigned int core, int type)
{
	unsigned int signum;

	switch (type) {
	case KSCHED_INTR_CEDE:
		signum = SIGUSR1;
		break;

	case KSCHED_INTR_YIELD:
		signum = SIGUSR2;
		break;

	default:
		WARN();
		return;
	}

	ksched_shm[core].signum = signum;
	store_release(&ksched_shm[core].sig, ksched_gens[core]);
	CPU_SET(core, &ksched_set);
	ksched_count++;
}

/**
 * ksched_enqueue_pmc - enqueues a performance counter request on a core
 * @core: the core to measure
 * @sel: the architecture-specific counter selector
 */
static inline void ksched_enqueue_pmc(unsigned int core, uint64_t sel)
{
	ksched_shm[core].pmcsel = sel;
	store_release(&ksched_shm[core].pmc, 1);
	CPU_SET(core, &ksched_set);
	ksched_count++;
}

/**
 * ksched_poll_pmc - polls for a performance counter result
 * @core: the core to poll
 * @val: a pointer to store the result
 *
 * Returns true if succesful, otherwise counter is still being measured.
 */
static inline bool ksched_poll_pmc(unsigned int core, uint64_t *val)
{
	if (load_acquire(&ksched_shm[core].pmc) != 0)
		return false;
	*val = ACCESS_ONCE(ksched_shm[core].pmcval);
	return true;
}

/**
 * ksched_send_intrs - sends any pending interrupts
 */
static inline void ksched_send_intrs(void)
{
	struct ksched_intr_req req;
	int ret;

	if (ksched_count == 0)
		return;

	ksched_count = 0;
	req.len = sizeof(ksched_set); 
	req.mask = &ksched_set;
	ret = ioctl(ksched_fd, KSCHED_IOC_INTR, &req);
	BUG_ON(ret);

	CPU_ZERO(&ksched_set);
}

static inline unsigned int pci_cfg_index(unsigned int bus, unsigned int device,
                                  unsigned int function, unsigned int offset) {
	unsigned int byte_address;
        assert(device >= 0);
        assert(device < (1 << 5));
        assert(function >= 0);
        assert(function < (1 << 3));
        assert(offset >= 0);
        assert(offset < (1 << 12));
        byte_address = (bus << 20) | (device << 15) | (function << 12) | offset;
        return byte_address;
}

/* Get the number of DRAM read/write happens since the last call. This function
 * should be called every small time interval otherwise the counter will
 * overflow. */
static inline uint32_t get_cas_count_all(void) {
	return ACCESS_ONCE(*low_cas_count_all_ptr);
}
