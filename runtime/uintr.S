
#ifdef CONFIG_UINTR

.file "uintr.S"
.section        .note.GNU-stack,"",@progbits
.text

.align 16
.globl uintr_asm_entry
.type uintr_asm_entry, @function
uintr_asm_entry:

	.cfi_startproc
	.cfi_signal_frame

	.cfi_def_cfa rsp, 32
	.cfi_offset rsp, -8
	.cfi_offset rip, -24

	// skip pad
	subq  $8, %rsp
	.cfi_adjust_cfa_offset 8

	// set xsave_area to NULL
	pushq $0
	.cfi_adjust_cfa_offset 8

	// fill rest of trapframe
	pushq   %rax
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset rax, 0

	pushq   %r15
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset r15, 0

	pushq   %r14
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset r14, 0

	pushq   %r13
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset r13, 0

	pushq   %r12
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset r12, 0

	pushq   %rbp
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset rbp, 0

	pushq   %rbx
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset rbx, 0

	pushq   %r11
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset r11, 0

	pushq   %r10
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset r10, 0

	pushq   %r9
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset r9, 0

	pushq   %r8
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset r8, 0

	pushq   %rcx
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset rcx, 0

	pushq   %rdx
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset rdx, 0

	pushq   %rsi
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset rsi, 0

	pushq   %rdi
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset rdi, 0

	// set first argument to beginning of trapframe
	movq    %rsp, %rdi

	// Align the stack
	subq    $8, %rsp
	.cfi_adjust_cfa_offset 8

	call     uintr_entry

	addq	$8, %rsp
	.cfi_adjust_cfa_offset -8

.align 16
.globl uintr_asm_return
uintr_asm_return:

	// restore trap frame
	popq   %rdi;
	.cfi_adjust_cfa_offset -8

	popq   %rsi;
	.cfi_adjust_cfa_offset -8

	popq   %rdx;
	.cfi_adjust_cfa_offset -8

	popq   %rcx;
	.cfi_adjust_cfa_offset -8

	popq   %r8;
	.cfi_adjust_cfa_offset -8

	popq   %r9;
	.cfi_adjust_cfa_offset -8

	popq   %r10;
	.cfi_adjust_cfa_offset -8

	popq   %r11
	.cfi_adjust_cfa_offset -8

	popq   %rbx;
	.cfi_adjust_cfa_offset -8

	popq   %rbp;
	.cfi_adjust_cfa_offset -8

	popq   %r12;
	.cfi_adjust_cfa_offset -8

	popq   %r13;
	.cfi_adjust_cfa_offset -8

	popq   %r14;
	.cfi_adjust_cfa_offset -8

	popq   %r15;
	.cfi_adjust_cfa_offset -8

	popq   %rax;
	.cfi_adjust_cfa_offset -8

	// remove pad, uirrv, xsave_area
	addq    $24, %rsp
	.cfi_adjust_cfa_offset -24

	uiret
	nop
	.cfi_endproc

#endif
