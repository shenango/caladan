/*
 * chksum.h - utilities for calculating checksums
 */

#pragma once

#include <stdint.h>

/**
 * chksum_internet - performs an internet checksum on a buffer
 * @buf: the buffer
 * @len: the length in bytes
 *
 * An internet checksum is a 16-bit one's complement sum. Details
 * are described in RFC 1071.
 *
 * Returns a 16-bit checksum value.
 */
static inline uint16_t chksum_internet(const void *buf, int len)
{
        uint64_t sum;

        asm volatile("xorq %0, %0\n"

             /* process 8 byte chunks */
             "movl %2, %%edx\n"
             "shrl $3, %%edx\n"
             "cmp $0, %%edx\n"
             "jz 2f\n"
             "1: adcq (%1), %0\n"
             "leaq 8(%1), %1\n"
             "decl %%edx\n"
             "jne 1b\n"
             "adcq $0, %0\n"

             /* process 4 byte (if left) */
             "2: test $4, %2\n"
             "je 3f\n"
             "movl (%1), %%edx\n"
             "addq %%rdx, %0\n"
             "adcq $0, %0\n"
             "leaq 4(%1), %1\n"

             /* process 2 byte (if left) */
             "3: test $2, %2\n"
             "je 4f\n"
             "movzwq (%1), %%rdx\n"
             "addq %%rdx, %0\n"
             "adcq $0, %0\n"
             "leaq 2(%1), %1\n"

             /* process 1 byte (if left) */
             "4: test $1, %2\n"
             "je 5f\n"
             "movzbq (%1), %%rdx\n"
             "addq %%rdx, %0\n"
             "adcq $0, %0\n"

             /* fold into 16-bit answer */
             "5: movq %0, %1\n"
             "shrq $32, %0\n"
             "addl %k1, %k0\n"
             "adcl $0, %k0\n"
             "movq %0, %1\n"
             "shrl $16, %k0\n"
             "addw %w1, %w0\n"
             "adcw $0, %w0\n"
	     "not %0\n"

            : "=&r"(sum), "=r"(buf)
            : "r"(len), "1"(buf) : "%rdx", "cc", "memory");

        return (uint16_t)sum;
}

