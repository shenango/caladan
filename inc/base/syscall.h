/*
 * syscall.h - support for common syscalls in the base library
 */

#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <sys/uio.h>

extern const char base_syscall_start[];
extern const char base_syscall_end[];

extern void *syscall_mmap(void *addr, size_t length, int prot, int flags,
	                      int fd, off_t offset);
extern long syscall_mbind(void *start, size_t len, int mode,
	                      const unsigned long *nmask, unsigned long maxnode,
	                      unsigned flags);
extern void syscall_rt_sigreturn(void);
extern int syscall_ioctl(int fd, unsigned long int request, void *arg);
extern int syscall_madvise(void *addr, size_t length, int advice);
extern int syscall_mprotect(void *addr, size_t len, int prot);
extern ssize_t syscall_pwritev2(int fd, const struct iovec *iov, int iovcnt,
	                            off_t offset_lo, off_t offset_hi, int flags);

extern ssize_t syscall_writev(int fd, const struct iovec *iov, int iovcnt);

static inline ssize_t syscall_pwrite(int fd, const void *buf, size_t count,
	                                 off_t offset)
{
	const struct iovec iov = {
		.iov_base = (void *)buf,
		.iov_len = count
	};

	return syscall_pwritev2(fd, &iov, 1, offset, 0, 0);
}

static inline ssize_t syscall_write(int fd, const void *buf, size_t count)
{
	const struct iovec iov = {
		.iov_base = (void *)buf,
		.iov_len = count
	};

	return syscall_writev(fd, &iov, 1);
}
