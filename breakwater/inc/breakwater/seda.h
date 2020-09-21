/*
 * seda.h - SEDA implementation for RPC layer
 */

#pragma once

#include <base/types.h>
#include <base/atomic.h>
#include <runtime/sync.h>

#include "rpc.h"

/* for RPC server */

struct ssd_ctx {
	struct srpc_ctx		cmn;
	uint64_t		ts;
};

/* for RPC client */
#define SEDA_NREQ 100

struct csd_session {
	struct crpc_session	cmn;
	uint64_t		id;
	uint64_t		req_id;
	mutex_t			lock;
	condvar_t		timer_cv;
	waitgroup_t		timer_waiter;
	bool			running;
	condvar_t		sender_cv;
	waitgroup_t		sender_waiter;

	/* token bucket for rate limiting */
	double			tb_token;
	double			tb_refresh_rate;
	uint64_t		tb_last_refresh;

	int32_t			res_ts[SEDA_NREQ];
	int			res_idx;
	double			cur;
	uint64_t		seda_last_update;

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
