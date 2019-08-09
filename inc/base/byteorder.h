/*
 * byteorder.h - utilties for swapping bytes and converting endianness
 */

#pragma once
#include <stdint.h>
#include <asm/cpu.h>
#include <base/compiler.h>

static inline uint16_t __bswap16(uint16_t val)
{
#ifdef HAS_BUILTIN_BSWAP
	return __builtin_bswap16(val);
#else
	return (((val & 0x00ffU) << 8) |
		((val & 0xff00U) >> 8));
#endif
}

static inline uint32_t __bswap32(uint32_t val)
{
#ifdef HAS_BUILTIN_BSWAP
	return __builtin_bswap32(val);
#else
	return (((val & 0x000000ffUL) << 24) |
		((val & 0x0000ff00UL) << 8) |
		((val & 0x00ff0000UL) >> 8) |
		((val & 0xff000000UL) >> 24));
#endif
}

static inline uint64_t __bswap64(uint64_t val)
{
#ifdef HAS_BUILTIN_BSWAP
	return __builtin_bswap64(val);
#else
	return (((val & 0x00000000000000ffULL) << 56) |
		((val & 0x000000000000ff00ULL) << 40) |
		((val & 0x0000000000ff0000ULL) << 24) |
		((val & 0x00000000ff000000ULL) << 8) |
		((val & 0x000000ff00000000ULL) >> 8) |
		((val & 0x0000ff0000000000ULL) >> 24) |
		((val & 0x00ff000000000000ULL) >> 40) |
		((val & 0xff00000000000000ULL) >> 56));
#endif
}

#ifndef __BYTE_ORDER
#error __BYTE_ORDER is undefined
#endif

#if __BYTE_ORDER == __LITTLE_ENDIAN

#define cpu_to_le16(x)	(x)
#define cpu_to_le32(x)	(x)
#define cpu_to_le64(x)	(x)
#define cpu_to_be16(x)	(__bswap16(x))
#define cpu_to_be32(x)	(__bswap32(x))
#define cpu_to_be64(x)	(__bswap64(x))

#define le16_to_cpu(x)	(x)
#define le32_to_cpu(x)	(x)
#define le64_to_cpu(x)	(x)
#define be16_to_cpu(x)	(__bswap16(x))
#define be32_to_cpu(x)	(__bswap32(x))
#define be64_to_cpu(x)	(__bswap64(x))

#else /* __BYTE_ORDER == __LITLE_ENDIAN */

#define cpu_to_le16(x)	(__bswap16(x))
#define cpu_to_le32(x)	(__bswap32(x))
#define cpu_to_le64(x)	(__bswap64(x))
#define cpu_to_be16(x)	(x)
#define cpu_to_be32(x)	(x)
#define cpu_to_be64(x)	(x)

#define le16_to_cpu(x)	(__bswap16(x))
#define le32_to_cpu(x)	(__bswap32(x))
#define le64_to_cpu(x)	(__bswap64(x))
#define be16_to_cpu(x)	(x)
#define be32_to_cpu(x)	(x)
#define be64_to_cpu(x)	(x)

#endif /* __BYTE_ORDER == __LITTLE_ENDIAN */

#define ntoh16(x)	(be16_to_cpu(x))
#define ntoh32(x)	(be32_to_cpu(x))
#define ntoh64(x)	(be64_to_cpu(x))

#define hton16(x)	(cpu_to_be16(x))
#define hton32(x)	(cpu_to_be32(x))
#define hton64(x)	(cpu_to_be64(x))
