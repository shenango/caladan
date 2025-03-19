
/*
 * trapframe.h - trap frame support
 */

#pragma once

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
    uint64_t uirrv;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
};

#define ARG0(tf)        ((tf)->rdi)
#define ARG1(tf)        ((tf)->rsi)
#define ARG2(tf)        ((tf)->rdx)
#define ARG3(tf)        ((tf)->rcx)
#define ARG4(tf)        ((tf)->r8)
#define ARG5(tf)        ((tf)->r9)
