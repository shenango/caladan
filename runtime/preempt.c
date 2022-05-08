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
/* is a request to cede pending? */
volatile __thread bool preempt_cede_pending;
/* is a request to yield pending? */
volatile __thread bool preempt_yield_pending;

/* set a flag to indicate a preemption request is pending */
static void set_preempt_needed(void)
{
	preempt_cnt &= ~PREEMPT_NOT_PENDING;
}

/* handles preemptive cede signals from the iokernel */
static void handle_sigusr1(int s, siginfo_t *si, void *c)
{
	STAT(PREEMPTIONS)++;
	/* can only be delivered once, then must wait for cede to finish */
	BUG_ON(preempt_cede_pending);

	/* resume execution if preemption is disabled */
	if (!preempt_enabled() || preempt_needed()) {
		preempt_cede_pending = true;
		set_preempt_needed();
		return;
	}

	thread_cede();
}

/* handles preemptive yield signals from the iokernel */
static void handle_sigusr2(int s, siginfo_t *si, void *c)
{
	STAT(PREEMPTIONS)++;
	/* cannot be delivered while cede is pending */
	BUG_ON(preempt_cede_pending);

	/* check if the last yield request is still pending */
	if (unlikely(preempt_yield_pending))
		return;

	/* resume execution if preemption is disabled */
	if (!preempt_enabled() || preempt_needed()) {
		preempt_yield_pending = true;
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
	volatile bool *cede_pending = &preempt_cede_pending;
	volatile bool *yield_pending = &preempt_yield_pending;

	assert(preempt_needed());
	clear_preempt_needed();

	/* after this point, we might end up running on a different core */

	if (*cede_pending) {
		*cede_pending = false;
		thread_cede();
	}

	if (*yield_pending) {
		*yield_pending = false;
		thread_yield();
	}
}

/**
 * preempt_init - global initializer for preemption support
 *
 * Returns 0 if successful. otherwise fail.
 */
int preempt_init(void)
{
	struct sigaction act;

	act.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;

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
