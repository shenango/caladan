
/*
 * trapframe.h - trap frame support
 */

#pragma once

/*
 * Trap Frame Format
 * WARNING: These values reflect the layout of struct thread_tf. Don't change
 * these values without also updating trapframe.h.
 */

/* arguments registers (can be clobbered) */
#define RDI     (0)
#define RSI     (8)
#define RDX     (16)
#define RCX     (24)
#define R8      (32)
#define R9      (40)

/* temporary registers (can be clobbered) */
#define R10     (48)
#define R11     (56)

/* callee-saved registers (can not be clobbered) */
#define RBX     (64)
#define RBP     (72)
#define R12     (80)
#define R13     (88)
#define R14     (96)
#define R15     (104)

/* special-purpose registers */
#define RAX     (112)   /* return code */
#define RIP     (144)   /* instruction pointer */
#define RSP     (160)   /* stack pointer */
#define ORIG_RAX (128)

#ifndef __ASSEMBLER__

#include <base/stddef.h>

/*
 * See the "System V Application Binary Interface" for a full explation of
 * calling and argument passing conventions.
 */

struct thread_tf {
	/* argument registers, can be clobbered by callee */
	uint64_t rdi; /* first argument */
	uint64_t rsi;
	uint64_t rdx;
	uint64_t rcx;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;

	/* callee-saved registers */
	uint64_t rbx;
	uint64_t rbp;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;

	/* special-purpose registers */
	uint64_t rax;   /* holds return value */
	unsigned char *xsave_area;
	uint64_t orig_rax;
	uint64_t uirrv;
	uint64_t rip;
	uint64_t rflags;
	uint64_t rsp;
};


BUILD_ASSERT(RDI == offsetof(struct thread_tf, rdi));
BUILD_ASSERT(RSI == offsetof(struct thread_tf, rsi));
BUILD_ASSERT(RDX == offsetof(struct thread_tf, rdx));
BUILD_ASSERT(RCX == offsetof(struct thread_tf, rcx));
BUILD_ASSERT(R8 == offsetof(struct thread_tf, r8));
BUILD_ASSERT(R9 == offsetof(struct thread_tf, r9));
BUILD_ASSERT(R10 == offsetof(struct thread_tf, r10));
BUILD_ASSERT(R11 == offsetof(struct thread_tf, r11));

BUILD_ASSERT(RBX == offsetof(struct thread_tf, rbx));
BUILD_ASSERT(RBP == offsetof(struct thread_tf, rbp));
BUILD_ASSERT(R12 == offsetof(struct thread_tf, r12));
BUILD_ASSERT(R13 == offsetof(struct thread_tf, r13));
BUILD_ASSERT(R14 == offsetof(struct thread_tf, r14));
BUILD_ASSERT(R15 == offsetof(struct thread_tf, r15));

BUILD_ASSERT(RAX == offsetof(struct thread_tf, rax));
BUILD_ASSERT(ORIG_RAX == offsetof(struct thread_tf, orig_rax));
BUILD_ASSERT(RIP == offsetof(struct thread_tf, rip));
BUILD_ASSERT(RSP == offsetof(struct thread_tf, rsp));


#define ARG0(tf)        ((tf)->rdi)
#define ARG1(tf)        ((tf)->rsi)
#define ARG2(tf)        ((tf)->rdx)
#define ARG3(tf)        ((tf)->rcx)
#define ARG4(tf)        ((tf)->r8)
#define ARG5(tf)        ((tf)->r9)

#endif
