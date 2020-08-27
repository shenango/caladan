/*
 * proto.h - RPC protocol definitions for SEDA
 */

#pragma once

#include <base/types.h>

#define SD_REQ_MAGIC	0x63727063 /* 'crpc' */
#define SD_RESP_MAGIC	0x73727063 /* 'srpc' */

enum {
	SD_OP_CALL = 0,  /* performs a procedure call */
	SD_OP_WINUPDATE, /* just updates the window (no call) */
	SD_OP_MAX,	  /* maximum number of opcodes */
};

/* header used for CLIENT -> SERVER */
struct csd_hdr {
	uint32_t	magic; /* must be set to RPC_REQ_MAGIC */
	uint32_t	op;    /* the opcode */
	size_t		len;   /* length of request in bytes */
	uint64_t	id;    /* Request / Response ID */
	uint64_t	ts;
};

/* header used for SERVER -> CLIENT */
struct ssd_hdr {
	uint32_t	magic; /* must be set to RPC_RESP_MAGIC */
	uint32_t	op;    /* the opcode */
	size_t		len;   /* length of response in bytes */
	uint64_t	id;    /* Request / Response ID */
	uint64_t	ts;
};
