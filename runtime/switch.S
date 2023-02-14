/*
 * switch.S - assembly routines for switching trap frames
 */

/*
 * Trap Frame Format
 * WARNING: These values reflect the layout of struct thread_tf. Don't change
 * these values without also updating defs.h.
 */

.file "switch.S"
.section        .note.GNU-stack,"",@progbits
.text

/* arguments registers (can be clobbered) */
#define RDI	(0)
#define RSI	(8)
#define RDX	(16)
#define RCX	(24)
#define R8	(32)
#define R9	(40)

/* temporary registers (can be clobbered) */
#define R10	(48)
#define R11	(56)

/* callee-saved registers (can not be clobbered) */
#define RBX	(64)
#define RBP	(72)
#define R12	(80)
#define R13	(88)
#define R14	(96)
#define R15	(104)

/* special-purpose registers */
#define RAX	(112)	/* return code */
#define RIP	(120)	/* instruction pointer */
#define RSP	(128)	/* stack pointer */

/**
 * __jmp_thread - executes a thread from the runtime
 * @tf: the trap frame to restore (%rdi)
 *
 * This low-level variant isn't intended to be called directly.
 * Re-enables preemption, parking the kthread if necessary.
 * Does not return.
 */
.align 16
.globl __jmp_thread
.type __jmp_thread, @function
__jmp_thread:
	/* restore ip and stack */
	movq    RSP(%rdi), %rsp
	movq    RIP(%rdi), %rsi

	/* restore callee regs */
	movq    RBX(%rdi), %rbx
	movq    RBP(%rdi), %rbp
	movq    R12(%rdi), %r12
	movq    R13(%rdi), %r13
	movq    R14(%rdi), %r14
	movq    R15(%rdi), %r15

	/* set first argument (in case new thread) */
	movq    RDI(%rdi), %rdi /* ARG0 */

	/* re-enable preemption */
	subl	$1, %gs:__perthread_preempt_cnt(%rip)
	jz	1f

	/* jump into trap frame */
	jmpq	*%rsi
	nop

1:	/* cold-path, save RIP and park the kthread */
	pushq	%rsi
	pushq	%rdi
	pushq	%r15
	movq	%rsp, %r15
	andq	$0xfffffffffffffff0, %rsp /* ensure correct stack alignment */
	call	preempt
	movq	%r15, %rsp /* restore SP */
	popq	%r15
	popq	%rdi
	popq	%rsi
	jmpq	*%rsi

/**
 * __jmp_thread_direct - directly switches from one thread to the next
 * @oldtf: the trap frame to save (%rdi)
 * @newtf: the trap frame to restore (%rsi)
 * @thread_running: a pointer to whether the thread is still running (%rdx)
 *
 * This low-level variant isn't intended to be called directly.
 * Re-enables preemption, parking the kthread if necessary.
 * Does return.
 */
.align 16
.globl __jmp_thread_direct
.type __jmp_thread_direct, @function
__jmp_thread_direct:
	/* save ip and stack */
	movq    (%rsp), %r8
	movq    %r8, RIP(%rdi)
	leaq    8(%rsp), %r8
	movq    %r8, RSP(%rdi)

	/* save callee regs */
	movq    %rbx, RBX(%rdi)
	movq    %rbp, RBP(%rdi)
	movq    %r12, R12(%rdi)
	movq    %r13, R13(%rdi)
	movq    %r14, R14(%rdi)
	movq    %r15, R15(%rdi)

	/* restore ip and stack */
	movq    RSP(%rsi), %rsp
	movq    RIP(%rsi), %r8

	/* clear the stack busy flag */
	movl	$0, (%rdx)

	/* restore callee regs */
	movq    RBX(%rsi), %rbx
	movq    RBP(%rsi), %rbp
	movq    R12(%rsi), %r12
	movq    R13(%rsi), %r13
	movq    R14(%rsi), %r14
	movq    R15(%rsi), %r15

	/* set first argument (in case new thread) */
	movq    RDI(%rsi), %rdi /* ARG0 */

	/* re-enable preemption */
	subl	$1, %gs:__perthread_preempt_cnt(%rip)
	jz	1f

	/* jump into trap frame */
	jmpq	*%r8
	nop

1:	/* cold-path, save RIP and park the kthread */
	pushq	%r8
	pushq	%rdi
	pushq	%r15
	movq	%rsp, %r15
	andq	$0xfffffffffffffff0, %rsp /* ensure correct stack alignment */
	call	preempt
	movq	%r15, %rsp /* restore SP */
	popq	%r15
	popq	%rdi
	popq	%r8
	jmpq	*%r8

/**
 * __jmp_runtime - saves the current trap frame and jumps to a function in the
 *                 runtime
 * @tf: the struct thread_tf to save state (%rdi)
 * @fn: the function pointer to call (%rsi)
 * @stack: the start of the runtime stack (%rdx)
 *
 * This low-level variant isn't intended to be called directly.
 * Must be called with preemption disabled.
 * No return value.
 */
.align 16
.globl __jmp_runtime
.type __jmp_runtime, @function
__jmp_runtime:
	/* save callee regs */
	movq    %rbx, RBX(%rdi)
	movq    %rbp, RBP(%rdi)
	movq    %r12, R12(%rdi)
	movq    %r13, R13(%rdi)
	movq    %r14, R14(%rdi)
	movq    %r15, R15(%rdi)

	/* save ip and stack */
	movq    (%rsp), %r8
	movq    %r8, RIP(%rdi)
	leaq    8(%rsp), %r8
	movq    %r8, RSP(%rdi)

	/* jump into runtime function */
	movq    %rdx, %rsp

	/* jump into runtime code */
	jmpq    *%rsi

/**
 * __jmp_runtime_nosave - jumps to a function in the runtime without saving the
 *			  current stack frame
 * @fn: the function pointer to call (%rdi)
 * @stack: the start of the runtime stack (%rsi)
 *
 * This low-level variant isn't intended to be called directly.
 * Must be called with preemption disabled.
 * No return value.
 */
.align 16
.globl __jmp_runtime_nosave
.type __jmp_runtime_nosave, @function
__jmp_runtime_nosave:

	/* jump into runtime function */
	movq    %rsi, %rsp
	movq	%rdi, %rsi

	/* jump into runtime code */
	jmpq    *%rsi
