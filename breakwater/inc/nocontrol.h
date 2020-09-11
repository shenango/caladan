/*
 * nocontrol.h - No server overload implementation
 * for RPC layer
 */

#pragma once

#include <base/types.h>
#include <runtime/sync.h>

#include "rpc.h"

/* for RPC server */

struct snc_ctx {
	struct srpc_ctx		cmn;
	uint64_t		ts;
};

/* for RPC client */
struct cnc_session {
	struct crpc_session	cmn;
	uint64_t		id;
	uint64_t		req_id;
	mutex_t			lock;
	bool			running;
	condvar_t		sender_cv;
	waitgroup_t		sender_waiter;

	/* a queue of pending RPC requests */
	uint32_t		head;
	uint32_t		tail;
	struct crpc_ctx		*qreq[CRPC_QLEN];

	/* client-side stats */
	uint64_t		winu_rx_;
	uint64_t		winu_tx_;
	uint64_t		resp_rx_;
	uint64_t		req_tx_;
	uint64_t		win_expired_;
	uint64_t		req_dropped_;
};
