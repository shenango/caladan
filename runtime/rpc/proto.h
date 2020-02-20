/*
 * proto.h - RPC protocol definitions
 */

#pragma once

#include <base/types.h>

#define RPC_REQ_MAGIC	0x63727063 /* 'crpc' */
#define RPC_RESP_MAGIC	0x73727063 /* 'srpc' */

enum {
	RPC_OP_CALL = 0,  /* performs a remote procedure call */
	RPC_OP_DROPCALL,  /* performs a droppable remote procedure call */
	RPC_OP_OFFER,	  /* offers more tokens to a client */
	RPC_OP_MAX,	  /* maximum number of opcodes */
};

/* header used for CLIENT -> SERVER */
struct crpc_hdr {
	uint32_t	magic;	/* must be set to RPC_REQ_MAGIC */
	uint32_t	op;	/* the opcode */
	uint64_t	ident;	/* the request identifier */
	size_t		len;	/* length of request in bytes */
	uint64_t	demand;	/* the demanded window size */
};

/* header used for SERVER -> CLIENT */
struct srpc_hdr {
	uint32_t	magic;	 /* must be set to RPC_RESP_MAGIC */
	uint32_t	op;	 /* the opcode */
	uint64_t	ident;	 /* the request identifier */
	size_t		len;	 /* length of response in bytes */
	uint64_t	delay_us;/* the server's queuing delay */
	float		tokens;	 /* the number of new tokens given to client */
	bool		accepted;/* was the request accepted by the server? */
};
