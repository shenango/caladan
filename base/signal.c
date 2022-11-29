/*
 * signal.c - support for setting up signal handlers without using glibc
 */

#include <base/signal.h>
#include <base/syscall.h>

#include <asm/unistd_64.h>
#include <errno.h>
#include <string.h>

#define SA_RESTORER 0x04000000

/* copied from glibc sysdeps/unix/sysv/linux/kernel_sigaction.h */
struct kernel_sigaction {
	__sighandler_t k_sa_handler;
	unsigned long sa_flags;
	void (*sa_restorer) (void);
	sigset_t sa_mask;
};

/* allow user to specify sa_restorer */
int base_sigaction_full(int sig, const struct sigaction *act,
                        struct sigaction *oact)
{
	long ret;
	struct kernel_sigaction kact, okact;

	if (act) {
		kact.k_sa_handler = act->sa_handler;
		memcpy(&kact.sa_mask, &act->sa_mask, sizeof(sigset_t));
		kact.sa_flags = act->sa_flags | SA_RESTORER;
		kact.sa_restorer = act->sa_restorer;
	}

	ret = syscall(__NR_rt_sigaction, sig, act ? &kact : NULL, oact ? &okact : NULL, 8);

	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	if (oact) {
		oact->sa_handler = okact.k_sa_handler;
		memcpy(&oact->sa_mask, &okact.sa_mask, sizeof(sigset_t));
		oact->sa_flags = okact.sa_flags;
		oact->sa_restorer = okact.sa_restorer;
	}

	return 0;
}


/* use our own sa_restorer instead of glibc's */
int base_sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
	long ret;
	struct kernel_sigaction kact, okact;

	if (act) {
		kact.k_sa_handler = act->sa_handler;
		memcpy(&kact.sa_mask, &act->sa_mask, sizeof(sigset_t));
		kact.sa_flags = act->sa_flags | SA_RESTORER;
		kact.sa_restorer = &syscall_rt_sigreturn;
	}

	ret = syscall(__NR_rt_sigaction, sig, act ? &kact : NULL, oact ? &okact : NULL, 8);

	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	if (oact) {
		oact->sa_handler = okact.k_sa_handler;
		memcpy(&oact->sa_mask, &okact.sa_mask, sizeof(sigset_t));
		oact->sa_flags = okact.sa_flags;
		oact->sa_restorer = okact.sa_restorer;
	}

	return 0;
}
