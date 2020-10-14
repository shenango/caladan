/*
 * proto.h - RPC protocol definitions
 */

#pragma once

#include <base/types.h>

#define DG_REQ_MAGIC	0x63727063 /* 'crpc' */
#define DG_RESP_MAGIC	0x73727063 /* 'srpc' */
#define DG_MAX_PRIO	128

enum {
	DG_OP_CALL = 0,  /* performs a procedure call */
	DG_OP_WINUPDATE, /* just updates the window (no call) */
	DG_OP_MAX,	  /* maximum number of opcodes */
};

#define DG_SFLAG_DROP	0x01

/* header used for CLIENT -> SERVER */
struct cdg_hdr {
	uint32_t	magic; /* must be set to RPC_REQ_MAGIC */
	uint32_t	op;    /* the opcode */
	size_t		len;   /* length of request in bytes */
	uint64_t	id;    /* Request / Response ID */
	int		prio;  /* the demanded window size */
	uint64_t	ts_sent;
};

/* header used for SERVER -> CLIENT */
struct sdg_hdr {
	uint32_t	magic; /* must be set to RPC_RESP_MAGIC */
	uint32_t	op;    /* the opcode */
	size_t		len;   /* length of response in bytes */
	uint64_t	id;    /* Request / Response ID */
	int		prio;  /* the offered window size */
	uint64_t	ts_sent;
	uint8_t		flags;
};
