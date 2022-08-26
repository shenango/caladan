/*
 * preempt.c - support for kthread preemption
 */

#include <signal.h>
#include <string.h>

#include "base/log.h"
#include "runtime/thread.h"
#include "runtime/preempt.h"

#include "defs.h"

/* the current preemption count */
volatile __thread unsigned int preempt_cnt = PREEMPT_NOT_PENDING;

/* set a flag to indicate a preemption request is pending */
static void set_preempt_needed(void)
{
	preempt_cnt &= ~PREEMPT_NOT_PENDING;
}

/* handles preemptive cede signals from the iokernel */
static void handle_sigusr1(int s, siginfo_t *si, void *c)
{
	STAT(PREEMPTIONS)++;

	/* resume execution if preemption is disabled */
	if (!preempt_enabled()) {
		set_preempt_needed();
		return;
	}

	WARN_ON_ONCE(!preempt_cede_needed(myk()));

	preempt_disable();
	thread_cede();
}

/* handles preemptive yield signals from the iokernel */
static void handle_sigusr2(int s, siginfo_t *si, void *c)
{
	STAT(PREEMPTIONS)++;

	/* resume execution if preemption is disabled */
	if (!preempt_enabled()) {
		set_preempt_needed();
		return;
	}

	/* check if yield request is still relevant */
	if (!preempt_yield_needed(myk()))
		return;

	thread_yield();
}

/**
 * preempt - entry point for preemption
 */
void preempt(void)
{
	struct kthread *k = getk();

	if (!preempt_needed()) {
		putk();
		return;
	}

	clear_preempt_needed();

	/*
         * preemption signals may be delivered after kthreads/uthreads
         * voluntarily park/yield, so the preempt_needed flag may be
         * set even when there is nothing to do
         */

	if (preempt_cede_needed(k)) {
		thread_cede();
		return;
	}

	if (preempt_yield_needed(k)) {
		putk();
		thread_yield();
		return;
	}

	putk();
}

/**
 * preempt_init - global initializer for preemption support
 *
 * Returns 0 if successful. otherwise fail.
 */
int preempt_init(void)
{
	struct sigaction act;

	act.sa_flags = SA_SIGINFO | SA_NODEFER;

	if (sigemptyset(&act.sa_mask) != 0) {
		log_err("couldn't empty the signal handler mask");
		return -errno;
	}

	act.sa_sigaction = handle_sigusr1;
	if (sigaction(SIGUSR1, &act, NULL) == -1) {
		log_err("couldn't register signal handler");
		return -errno;
	}

	act.sa_sigaction = handle_sigusr2;
	if (sigaction(SIGUSR2, &act, NULL) == -1) {
		log_err("couldn't register signal handler");
		return -errno;
	}

	return 0;
}
