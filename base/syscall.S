
#include <asm/unistd_64.h>

.section        .note.GNU-stack,"",@progbits
.text
.globl base_syscall_start
base_syscall_start:

.align 16
.globl syscall_mmap
syscall_mmap:
	movl    $__NR_mmap, %eax
	movq    %rcx, %r10
	syscall
	ret

.align 16
.globl syscall_mbind
syscall_mbind:
	movl    $__NR_mbind, %eax
	movq    %rcx, %r10
	syscall
	ret

.align 16
.globl syscall_ioctl
syscall_ioctl:
	movl    $__NR_ioctl, %eax
	syscall
	ret

.align 16
.globl syscall_rt_sigreturn
syscall_rt_sigreturn:
	movl    $__NR_rt_sigreturn, %eax
        // Invalidate the alternate signal stack entry in this frame
        // so the kernel doesn't change it.
	movl    $4, 24(%rsp)
	syscall
	ret

.align 16
.globl syscall_madvise
syscall_madvise:
	movl    $__NR_madvise, %eax
	syscall
	ret

.align 16
.globl syscall_mprotect
syscall_mprotect:
	movl    $__NR_mprotect, %eax
	syscall
	ret

.globl base_syscall_end
base_syscall_end:
