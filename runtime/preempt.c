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
volatile __thread bool preempt_cede;

/* set a flag to indicate a preemption request is pending */
static void set_preempt_needed(void)
{
	preempt_cnt &= ~PREEMPT_NOT_PENDING;
}

/* handles preemption cede signals from the iokernel */
static void handle_sigusr1(int s, siginfo_t *si, void *c)
{
	STAT(PREEMPTIONS)++;
	set_preempt_needed();
	preempt_cede = true;

	/* resume execution if preemption is disabled */
	if (!preempt_enabled())
		return;

	thread_cede();
}

/* handles preemption yield signals from the iokernel */
static void handle_sigusr2(int s, siginfo_t *si, void *c)
{
	STAT(PREEMPTIONS)++;

	/*
	 * handle the case when SIGUSR1 is delivered while preemption is
	 * disabled, preemption is reenabled, and a SIGUSR2 is delivered
	 * before/during a call to preempt
	 */
	if (preempt_cede)
		return;

	/* resume execution if preemption is disabled */
	if (!preempt_enabled()) {
		set_preempt_needed();
		return;
	}

	thread_yield();
}


/**
 * preempt - entry point for preemption
 */
void preempt(void)
{
	assert(preempt_needed());
	clear_preempt_needed();
	if (preempt_cede)
		thread_cede();
	else
		thread_yield();
}



/**
 * preempt_init - global initializer for preemption support
 *
 * Returns 0 if successful. otherwise fail.
 */
int preempt_init(void)
{
	struct sigaction act;

	act.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESTART;

	if (sigemptyset(&act.sa_mask) != 0) {
		log_err("couldn't empty the signal handler mask");
		return -errno;
	}

	act.sa_sigaction = handle_sigusr2;
	if (sigaction(SIGUSR2, &act, NULL) == -1) {
		log_err("couldn't register signal handler");
		return -errno;
	}

	act.sa_sigaction = handle_sigusr1;

	/* block signals during SIGUSR1 */
	if (sigaddset(&act.sa_mask, SIGUSR2) != 0) {
		log_err("couldn't set signal handler mask");
		return -errno;
	}

	if (sigaddset(&act.sa_mask, SIGUSR1) != 0) {
		log_err("couldn't set signal handler mask");
		return -errno;
	}

	if (sigaction(SIGUSR1, &act, NULL) == -1) {
		log_err("couldn't register signal handler");
		return -errno;
	}

	return 0;
}
