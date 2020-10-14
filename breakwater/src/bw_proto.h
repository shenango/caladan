/*
 * bw_proto.h - RPC protocol definitions for BreakWater
 */

#pragma once

#include <base/types.h>

#define BW_REQ_MAGIC	0x63727063 /* 'crpc' */
#define BW_RESP_MAGIC	0x73727063 /* 'srpc' */

enum {
	BW_OP_CALL = 0,  /* performs a procedure call */
	BW_OP_WINUPDATE, /* just updates the window (no call) */
	BW_OP_MAX,	  /* maximum number of opcodes */
};

#define BW_CFLAG_DSYNC	0x01

#define BW_SFLAG_DROP	0x01

/* header used for CLIENT -> SERVER */
struct cbw_hdr {
	uint32_t	magic; /* must be set to RPC_REQ_MAGIC */
	uint32_t	op;    /* the opcode */
	size_t		len;   /* length of request in bytes */
	uint64_t	id;    /* Request / Response ID */
	uint64_t	demand;/* the demanded window size */
	uint64_t	ts_sent;
	uint8_t		flags;
};

/* header used for SERVER -> CLIENT */
struct sbw_hdr {
	uint32_t	magic; /* must be set to RPC_RESP_MAGIC */
	uint32_t	op;    /* the opcode */
	size_t		len;   /* length of response in bytes */
	uint64_t	id;    /* Request / Response ID */
	uint64_t	win;   /* the offered window size */
	uint64_t	ts_sent;
	uint8_t		flags;
};
