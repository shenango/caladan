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

#define PROBE_MEM_LAT_SAMPLE_NUM (5)

extern int ksched_fd, ksched_count;
extern struct ksched_shm_cpu *ksched_shm;
extern cpu_set_t ksched_set;
extern unsigned int ksched_gens[NCPU];
extern volatile char *ucmem;

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

/* A high precision timer that gives the cycle-level result. */
struct hp_timer {
        unsigned        start_cycles_low;
        unsigned        start_cycles_high;
        unsigned        end_cycles_low;
        unsigned        end_cycles_high;
};

static void start_hp_timer(struct hp_timer *hp_timer)
{
        asm volatile(
		"xorl %%eax, %%eax\n\t"
                "CPUID\n\t"
                "RDTSC\n\t"
                "mov %%edx, %0\n\t"
                "mov %%eax, %1\n\t"
                : "=r"(hp_timer->start_cycles_high),
                 "=r"(hp_timer->start_cycles_low)::"%rax", "%rbx", "%rcx", "%rdx");
}

static void end_hp_timer(struct hp_timer *hp_timer)
{
        asm volatile(
		"RDTSCP\n\t"
                "mov %%edx, %0\n\t"
                "mov %%eax, %1\n\t"
                "xorl %%eax, %%eax\n\t"
	        "CPUID\n\t"
                : "=r"(hp_timer->end_cycles_high),
                  "=r"(hp_timer->end_cycles_low)::"%rax", "%rbx", "%rcx", "%rdx");
}

static unsigned get_hp_timer_elapse(struct hp_timer *hp_timer) {
        uint64_t start, end;
	start =
	  (((uint64_t)hp_timer->start_cycles_high << 32) |
	   hp_timer->start_cycles_low);
	end =
	  (((uint64_t)hp_timer->end_cycles_high << 32) |
	   hp_timer->end_cycles_low);
	return (unsigned int)(end - start);
}

static inline void __sort(unsigned a[], int n)
{
        int i, j;

	for (i = 0; i < n - 1; i++) {
	  for (j = 0; j < n - i - 1; j++) {
	    if (a[j] > a[j + 1]) {
	      unsigned tmp = a[j];
	      a[j] = a[j + 1];
	      a[j + 1] = tmp;
	    }
	  }
	}
}

static inline unsigned probe_mem_lat(void)
{
        struct hp_timer hp_timer;
	unsigned lats[PROBE_MEM_LAT_SAMPLE_NUM];
	for (int i = 0; i < PROBE_MEM_LAT_SAMPLE_NUM; i++) {
		start_hp_timer(&hp_timer);
		*((volatile char *)(ucmem));
		end_hp_timer(&hp_timer);
		lats[i] = get_hp_timer_elapse(&hp_timer);
	}
	__sort(lats, PROBE_MEM_LAT_SAMPLE_NUM);
	return lats[PROBE_MEM_LAT_SAMPLE_NUM / 2];
}
