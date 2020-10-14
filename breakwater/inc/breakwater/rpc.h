/*
 * rpc.h - an interface for remote procedure calls
 */

#pragma once

#include <runtime/net.h>
#include <runtime/tcp.h>

/*
 * Stats data structures
 */

/*
 * Server API
 */

#define SRPC_PORT	8123
#define SRPC_BUF_SIZE	4096

struct srpc_session {
	tcpconn_t		*c;
};

struct srpc_ctx {
	struct srpc_session	*s;
	int			idx;
	uint64_t		id;
	size_t			req_len;
	size_t			resp_len;
	char			req_buf[SRPC_BUF_SIZE];
	char			resp_buf[SRPC_BUF_SIZE];
};

typedef void (*srpc_fn_t)(struct srpc_ctx *ctx);

struct srpc_ops {
	/**
	 * srpc_enable - starts the RPC server
	 * @handler: the handler function to call for each RPC.
	 *
	 * Returns 0 if successful.
	 */
	int (*srpc_enable)(srpc_fn_t handler);

	uint64_t (*srpc_stat_winu_rx)();
	uint64_t (*srpc_stat_winu_tx)();
	uint64_t (*srpc_stat_win_tx)();
	uint64_t (*srpc_stat_req_rx)();
	uint64_t (*srpc_stat_req_dropped)();
	uint64_t (*srpc_stat_resp_tx)();
};

/*
 * Client API
 */

#define CRPC_QLEN		16

struct crpc_session {
	tcpconn_t		*c;
};

struct crpc_ctx {
	size_t			len;
	uint64_t		id;
	uint64_t		ts;
	char			buf[SRPC_BUF_SIZE];
};

struct crpc_ops {
	/**
	 * crpc_send_one - sends one RPC request
	 * @s: the RPC session to send to
	 * @ident: the unique identifier associated with the request
	 * @buf: the payload buffer to send
	 * @len: the length of @buf (up to SRPC_BUF_SIZE)
	 *
	 * WARNING: This function could block.
	 *
	 * On success, returns the length sent in bytes (i.e. @len). On failure,
	 * returns -ENOBUFS if the window is full. Otherwise, returns standard socket
	 * errors (< 0).
	 */
	ssize_t (*crpc_send_one)(struct crpc_session *s,
				 const void *buf, size_t len, int hash);
	/**
	 * crpc_recv_one - receive one RPC request
	 * @s: the RPC session to receive from
	 * @buf: a buffer to store the received payload
	 * @len: the length of @buf (up to SRPC_BUF_SIZE)
	 *
	 * WARNING: This function could block.
	 *
	 * On success, returns the length received in bytes. On failure returns standard
	 * socket errors (<= 0).
	 */
	ssize_t (*crpc_recv_one)(struct crpc_session *s,
				 void *buf, size_t len, uint64_t *latency);

	/**
	 * crpc_open - creates an RPC session
	 * @raddr: the remote address to connect to (port must be SRPC_PORT)
	 * @sout: the connection session that was created
	 *
	 * WARNING: This function could block.
	 *
	 * Returns 0 if successful.
	 */
	int (*crpc_open)(struct netaddr raddr, struct crpc_session **sout,
			 int id);

	/**
	 * crpc_close - closes an RPC session
	 * @s: the session to close
	 *
	 * WARNING: This function could block.
	 */
	void (*crpc_close)(struct crpc_session *s);

	uint32_t (*crpc_win_avail)(struct crpc_session *s);
	void (*crpc_stat_clear)(struct crpc_session *s);
	uint64_t (*crpc_stat_winu_rx)(struct crpc_session *s);
	uint64_t (*crpc_stat_win_expired)(struct crpc_session *s);
	uint64_t (*crpc_stat_winu_tx)(struct crpc_session *s);
	uint64_t (*crpc_stat_resp_rx)(struct crpc_session *s);
	uint64_t (*crpc_stat_req_tx)(struct crpc_session *s);
	uint64_t (*crpc_stat_req_dropped)(struct crpc_session *s);
};

/*
 * RPC implementations
 */

extern const struct srpc_ops *srpc_ops;
extern const struct crpc_ops *crpc_ops;
extern struct srpc_ops sbw_ops;
extern struct crpc_ops cbw_ops;
extern struct srpc_ops ssd_ops;
extern struct crpc_ops csd_ops;
extern struct srpc_ops sdg_ops;
extern struct crpc_ops cdg_ops;
extern struct srpc_ops snc_ops;
extern struct crpc_ops cnc_ops;
