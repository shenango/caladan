
.file "uintr.S"
.section        .note.GNU-stack,"",@progbits
.text

.align 16
.globl uintr_asm_entry
.type uintr_asm_entry, @function
uintr_asm_entry:
	// skip pad, rsp, rip
	subq    $24, %rsp

	// fill rest of trapframe
	pushq   %rax
	pushq   %r15
	pushq   %r14
	pushq   %r13
	pushq   %r12
	pushq   %rbp
	pushq   %rbx
	pushq   %r11
	pushq   %r10
	pushq   %r9
	pushq   %r8
	pushq   %rcx
	pushq   %rdx
	pushq   %rsi
	pushq   %rdi

	// set first argument to beginning of trapframe
	movq    %rsp, %rdi

	call     uintr_entry

.align 16
.globl uintr_asm_return
uintr_asm_return:

	// restore trap frame
	popq   %rdi;
	popq   %rsi;
	popq   %rdx;
	popq   %rcx;
	popq   %r8;
	popq   %r9;
	popq   %r10;
	popq   %r11
	popq   %rbx;
	popq   %rbp;
	popq   %r12;
	popq   %r13;
	popq   %r14;
	popq   %r15;
	popq   %rax;

	// remove rip, rsp, pad, uirrv
	addq    $32, %rsp

	uiret
	nop