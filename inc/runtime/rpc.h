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


/*
 * Client API
 */

#define CRPC_QLEN		16

struct crpc_session {
	tcpconn_t		*c;
	spinlock_t		lock;
	uint32_t		win_used;
	float			tokens;
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
