/*
 * breakwater.h - breakwater implementation for RPC layer
 */

#pragma once

#include <base/types.h>
#include <base/atomic.h>
#include <runtime/sync.h>

#include "rpc.h"

/* for RPC server */

struct sbw_ctx {
	struct srpc_ctx		cmn;
	uint64_t		ts_sent;
	bool			drop;
};

/* for RPC client */
struct cbw_session {
	struct crpc_session	cmn;
	uint64_t		id;
	uint64_t		req_id;
	mutex_t			lock;
	waitgroup_t		timer_waiter;
	bool			waiting_winupdate;
	uint32_t		win_avail;
	uint32_t		win_used;
	bool			running;
	bool			demand_sync;
	condvar_t		timer_cv;
	bool			init;

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
