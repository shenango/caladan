/*
 * proto.h - RPC protocol definitions
 */

#pragma once

#include <base/types.h>

#define RPC_REQ_MAGIC	0x63727063 /* 'crpc' */
#define RPC_RESP_MAGIC	0x73727063 /* 'srpc' */

enum {
	RPC_OP_CALL = 0,  /* performs a procedure call */
	RPC_OP_PROBE,	  /* offer load to the server */
	RPC_OP_MAX,	  /* maximum number of opcodes */
};

/* header used for CLIENT -> SERVER */
struct crpc_hdr {
	uint32_t	magic; /* must be set to RPC_REQ_MAGIC */
	uint32_t	op;    /* the opcode */
	size_t		len;   /* length of request in bytes */
	uint64_t	demand;/* the demanded window size */
};

/* header used for SERVER -> CLIENT */
struct srpc_hdr {
	uint32_t	magic;	 /* must be set to RPC_RESP_MAGIC */
	uint32_t	op;	 /* the opcode */
	size_t		len;	 /* length of response in bytes */
	uint64_t	delay_us;/* the server's queuing delay */
	uint64_t	probe_us;/* the probe backoff time */
	bool		accepted;/* was the request accepted by the server? */
};
