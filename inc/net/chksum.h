/*
 * chksum.h - network checksum routines
 */

#pragma once

#include <base/stddef.h>
#include <net/ip.h>

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 1982, 1986, 1990, 1993
 *      The Regents of the University of California.
 * Copyright(c) 2010-2014 Intel Corporation.
 * Copyright(c) 2014 6WIND S.A.
 * All rights reserved.
 *
 * These checksum routines were originally from DPDK.
 */

/**
 * @internal Calculate a sum of all words in the buffer.
 * Helper routine for the rte_raw_cksum().
 *
 * @param buf
 *   Pointer to the buffer.
 * @param len
 *   Length of the buffer.
 * @param sum
 *   Initial value of the sum.
 * @return
 *   sum += Sum of all words in the buffer.
 */
static inline uint32_t
__raw_cksum(const void *buf, size_t len, uint32_t sum)
{
	/* workaround gcc strict-aliasing warning */
	uintptr_t ptr = (uintptr_t)buf;
	typedef uint16_t __attribute__((__may_alias__)) u16_p;
	const u16_p *u16 = (const u16_p *)ptr;

	while (len >= (sizeof(*u16) * 4)) {
		sum += u16[0];
		sum += u16[1];
		sum += u16[2];
		sum += u16[3];
		len -= sizeof(*u16) * 4;
		u16 += 4;
	}
	while (len >= sizeof(*u16)) {
		sum += *u16;
		len -= sizeof(*u16);
		u16 += 1;
	}

	/* if length is in odd bytes */
	if (len == 1)
		sum += *((const uint8_t *)u16);

	return sum;
}

/**
 * @internal Reduce a sum to the non-complemented checksum.
 * Helper routine for the rte_raw_cksum().
 *
 * @param sum
 *   Value of the sum.
 * @return
 *   The non-complemented checksum.
 */
static inline uint16_t
__raw_cksum_reduce(uint32_t sum)
{
	sum = ((sum & 0xffff0000) >> 16) + (sum & 0xffff);
	sum = ((sum & 0xffff0000) >> 16) + (sum & 0xffff);
	return (uint16_t)sum;
}

/**
 * Process the non-complemented checksum of a buffer.
 *
 * @param buf
 *   Pointer to the buffer.
 * @param len
 *   Length of the buffer.
 * @return
 *   The non-complemented checksum.
 */
static inline uint16_t
raw_cksum(const void *buf, size_t len)
{
	uint32_t sum;

	sum = __raw_cksum(buf, len, 0);
	return __raw_cksum_reduce(sum);
}

/**
 * Process the pseudo-header checksum of an IPv4 header.
 *
 * The checksum field must be set to 0 by the caller.
 *
 * @param ipv4_hdr
 *   The pointer to the contiguous IPv4 header.
 * @return
 *   The non-complemented checksum to set in the L4 header.
 */
static inline uint16_t
ipv4_phdr_cksum(uint8_t proto, uint32_t saddr, uint32_t daddr, uint16_t l4len)
{
	struct ipv4_psd_header {
		uint32_t saddr;    /* IP address of source host. */
		uint32_t daddr;    /* IP address of destination host. */
		uint8_t  zero;     /* zero. */
		uint8_t  proto;    /* L4 protocol type. */
		uint16_t len;      /* L4 length. */
	} psd_hdr;

	psd_hdr.saddr = hton32(saddr);
	psd_hdr.daddr = hton32(daddr);
	psd_hdr.zero = 0;
	psd_hdr.proto = proto;
	psd_hdr.len = hton16(l4len);
	return raw_cksum(&psd_hdr, sizeof(psd_hdr));
}

static inline uint16_t
ipv4_udptcp_cksum(uint8_t proto, uint32_t saddr, uint32_t daddr,
		  uint16_t l4len, const void *l4hdr)
{
	uint32_t cksum;

	cksum = raw_cksum(l4hdr, l4len);
	cksum += ipv4_phdr_cksum(proto, saddr, daddr, l4len);
	cksum = ((cksum & 0xffff0000) >> 16) + (cksum & 0xffff);
	cksum = (~cksum) & 0xffff;
	if (cksum == 0)
		cksum = 0xffff;

	return (uint16_t)cksum;
}

static inline uint16_t ipv4_cksum(const struct ip_hdr *iphdr)
{
	uint16_t cksum;
	cksum = raw_cksum(iphdr, sizeof(*iphdr));
	return (uint16_t)~cksum;
}
