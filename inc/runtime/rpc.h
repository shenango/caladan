/*
 * rpc.h - an interface for remote procedure calls
 */

#pragma once

#include <base/types.h>
#include <base/atomic.h>
#include <runtime/net.h>
#include <runtime/tcp.h>
#include <runtime/sync.h>


/*
 * Server API
 */

struct srpc_session;

#define SRPC_PORT	8123
#define SRPC_BUF_SIZE	4096

struct srpc_ctx {
	struct srpc_session	*s;
	bool			drop;
	int			idx;
	uint64_t		ident;
	size_t			req_len;
	size_t			resp_len;
	char			req_buf[SRPC_BUF_SIZE];
	char			resp_buf[SRPC_BUF_SIZE];
};

typedef void (*srpc_fn_t)(struct srpc_ctx *ctx);

extern int srpc_enable(srpc_fn_t handler);
extern uint64_t srpc_stat_req_dropped();
extern uint64_t srpc_stat_req_rx();
extern uint64_t srpc_stat_dreq_rx();
extern uint64_t srpc_stat_resp_tx();
extern uint64_t srpc_stat_offer_tx();

/*
 * Client API
 */

#define CRPC_QLEN		16

struct crpc_session {
	tcpconn_t		*c;
	spinlock_t		lock;
	uint32_t		win_used;
	float			tokens;

	/* client-side stats */
	uint64_t		resp_rx_;
	uint64_t		offer_rx_;
	float			token_rx_;
	uint64_t		req_tx_;
	uint64_t		req_dropped_;
};

extern ssize_t crpc_send_one(struct crpc_session *s,
			     const void *buf, size_t len);
extern ssize_t crpc_recv_one(struct crpc_session *s,
			     void *buf, size_t len);
extern int crpc_open(struct netaddr raddr, struct crpc_session **sout);
extern void crpc_close(struct crpc_session *s);

/**
 * crpc_is_busy - is the session busy (unable to accept requests right now)
 * @s: the session to check
 */
static inline bool crpc_is_busy(struct crpc_session *s)
{
	return ACCESS_ONCE(s->tokens) < 1.0;
}

/* client-side stats */
static inline uint64_t crpc_stat_resp_rx(struct crpc_session *s)
{
	return ACCESS_ONCE(s->resp_rx_);
}

static inline uint64_t crpc_stat_offer_rx(struct crpc_session *s)
{
	return ACCESS_ONCE(s->offer_rx_);
}

static inline float crpc_stat_token_rx(struct crpc_session *s)
{
	return ACCESS_ONCE(s->token_rx_);
}

static inline uint64_t crpc_stat_req_tx(struct crpc_session *s)
{
	return ACCESS_ONCE(s->req_tx_);
}

static inline uint64_t crpc_stat_req_dropped(struct crpc_session *s)
{
	return ACCESS_ONCE(s->req_dropped_);
}
