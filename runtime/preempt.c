/*
 * preempt.c - support for kthread preemption
 */

#include <signal.h>
#include <string.h>

#include <asm/prctl.h>
#include <immintrin.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include <base/log.h>
#include <runtime/thread.h>
#include <runtime/preempt.h>

#include "defs.h"

#define __user
#include "../ksched/ksched.h"

#define REDZONE_SIZE   128

/* the current preemption count */
DEFINE_PERTHREAD(unsigned int, preempt_cnt);
/* perthread stack to use supply for UIPIs */
DEFINE_PERTHREAD(void *, uintr_stack);
/* maximum size in bytes needed for xsave */
size_t xsave_max_size;
/* extended processor features to save */
size_t xsave_features;

/* set a flag to indicate a preemption request is pending */
static __nofp inline void set_preempt_needed(void)
{
	BUILD_ASSERT(~PREEMPT_NOT_PENDING == 0x7fffffff);
	perthread_andi(preempt_cnt, 0x7fffffff);
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

/*
 * WARNING: any functions called from this function before xsavec is called
 * must be marked with __nofp.
 */
__weak __nofp void uintr_entry(struct uintr_frame *uintr_frame)
{
	struct kthread *k;
	unsigned char *xsave_buf;

	STAT(PREEMPTIONS)++;

	/* resume execution if preemption is disabled */
	if (!preempt_enabled()) {
		set_preempt_needed();
		return;
	}

	k = getk();

	bool do_cede = preempt_cede_needed(k);
	if (!do_cede && !preempt_yield_needed(k)) {
		putk();
		return;
	}

	/* allocate buffer for xsave area on stack */
	xsave_buf = alloca(xsave_max_size + 64);
	xsave_buf = (unsigned char *)align_up((uintptr_t)xsave_buf, 64);

	/* zero xsave header */
	__builtin_memset(xsave_buf + 512, 0, 64);

	/* save state */
	__builtin_ia32_xsavec64(xsave_buf, xsave_features);

	if (do_cede) {
		thread_cede();
	} else {
		/* re-enable interrupts */
		__builtin_ia32_stui();
		putk();
		thread_yield();
	}

	/* restore state */
	__builtin_ia32_xrstor64(xsave_buf, xsave_features);
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

int preempt_init_thread(void)
{
	perthread_store(preempt_cnt, PREEMPT_NOT_PENDING);
	perthread_store(uintr_stack, (void *)REDZONE_SIZE);
	return 0;
}

/**
 * preempt_init - global initializer for preemption support
 *
 * Returns 0 if successful. otherwise fail.
 */
int preempt_init(void)
{
	int ret;
	struct sigaction act;
	struct cpuid_info regs;

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

	ret = ioctl(ksched_fd, KSCHED_IOC_UINTR_SETUP_USER, uintr_asm_entry);
	if (ret) {
		log_err("uintr: unavailable");
		return 0;
	}

	log_info("uintr: enabled");

	ret = syscall(SYS_arch_prctl, ARCH_GET_XCOMP_SUPP, &xsave_features);
	if (unlikely(ret)) {
		log_err("failed to get XSAVE features");
		return -1;
	}

	cpuid(0xd, 0, &regs);
	xsave_max_size = regs.ecx;

	return 0;
}
