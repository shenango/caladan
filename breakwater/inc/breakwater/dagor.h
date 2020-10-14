/*
 * dagor.h - DAGOR implementation for RPC layer
 */

#pragma once

#include <base/types.h>
#include <base/atomic.h>
#include <runtime/sync.h>

#include "rpc.h"

/* for RPC server */

struct sdg_ctx {
	struct srpc_ctx		cmn;
	uint64_t		ts_sent;
	bool			drop;
};

/* for RPC client */

struct cdg_ctx {
	struct crpc_ctx		cmn;
	int			prio;
};

struct cdg_session {
	struct crpc_session	cmn;
	uint64_t		id;
	uint64_t		req_id;
	int			local_prio;
	mutex_t			lock;
	bool			running;
	condvar_t		sender_cv;
	waitgroup_t		sender_waiter;

	/* a queue of pending RPC requests */
	uint32_t		head;
	uint32_t		tail;
	struct cdg_ctx		*qreq[CRPC_QLEN];

	/* client-side stats */
	uint64_t		winu_rx_;
	uint64_t		winu_tx_;
	uint64_t		resp_rx_;
	uint64_t		req_tx_;
	uint64_t		win_expired_;
	uint64_t		req_dropped_;
};
