
#include <asm/unistd_64.h>

.section        .note.GNU-stack,"",@progbits
.text
.globl base_syscall_start
base_syscall_start:

.align 16
.globl syscall_pwritev2
.type syscall_pwritev2, @function
syscall_pwritev2:
	movl	$__NR_pwritev2, %eax
	movq    %rcx, %r10
	syscall
	ret

.align 16
.globl syscall_writev
.type syscall_writev, @function
syscall_writev:
        movl    $__NR_writev, %eax
        syscall
        ret

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
	.cfi_startproc simple
	.cfi_signal_frame
	.cfi_def_cfa rsp, 40
	.cfi_offset rsp, 120
	.cfi_offset rip, 128

	.cfi_offset r8, 0
	.cfi_offset r9, 8
	.cfi_offset r10, 16
	.cfi_offset r11, 24
	.cfi_offset r12, 32
	.cfi_offset r13, 40
	.cfi_offset r14, 48
	.cfi_offset r15, 56
	.cfi_offset rdi, 64
	.cfi_offset rsi, 72
	.cfi_offset rbp, 80
	.cfi_offset rbx, 88
	.cfi_offset rdx, 96
	.cfi_offset rax, 104
	.cfi_offset rcx, 112

	movq    $__NR_rt_sigreturn, %rax
	// Invalidate the alternate signal stack entry in this frame
	// so the kernel doesn't change it.
	movl    $4, 24(%rsp)

	// Set mask to 0
	movq    $0, 296(%rsp)
	syscall
	ret
	.cfi_endproc

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
