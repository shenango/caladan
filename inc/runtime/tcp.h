/*
 * tcp.h - TCP sockets
 */

#pragma once

#include <runtime/net.h>
#include <runtime/poll.h>
#include <sys/uio.h>
#include <sys/socket.h>

struct tcpqueue;
typedef struct tcpqueue tcpqueue_t;
struct tcpconn;
typedef struct tcpconn tcpconn_t;

/**
 * __tcp_dial - opens a TCP connection, creating a new socket. If token is not
 * NULL, it's associated laddr will be considered instead of @laddr. If this
 * call succeeds (e,g, returns 0 or -EINPROGRESS), then the token has been
 * consumed and no longer belongs to the caller.
 * @laddr: the local address
 * @raddr: the remote address
 * @c_out: a pointer to store the new connection
 * @nonblocking: whether to make the connection nonblocking
 * @token: the token to use for the connection (may be NULL)
 */
extern int __tcp_dial(struct netaddr laddr, struct netaddr raddr,
                      tcpconn_t **c_out, bool nonblocking,
	                  bind_token_t *token);

/**
 * tcp_dial - opens a TCP connection, creating a new socket
 * @laddr: the local address
 * @raddr: the remote address
 * @c_out: a pointer to store the new connection
 *
 * Returns 0 if successful, otherwise fail.
 */
static inline int tcp_dial(struct netaddr laddr, struct netaddr raddr,
		    tcpconn_t **c_out)
{
	return __tcp_dial(laddr, raddr, c_out, false, NULL);
}

/**
 * tcp_dial_nonblocking - opens a nonblocking TCP connection, creating a new
 * socket
 * @laddr: the local address
 * @raddr: the remote address
 * @c_out: a pointer to store the new connection
 *
 * Returns 0 if successful, otherwise fail.
 */
static inline int tcp_dial_nonblocking(struct netaddr laddr,
                                       struct netaddr raddr,
                                       tcpconn_t **c_out)
{
	return __tcp_dial(laddr, raddr, c_out, true, NULL);
}

extern int tcp_dial_affinity(uint32_t affinity, struct netaddr raddr,
		    tcpconn_t **c_out);
extern int tcp_dial_conn_affinity(tcpconn_t *in, struct netaddr raddr,
		    tcpconn_t **c_out);

/**
 * tcp_set_default_window - sets the default TCP receive window for new conns
 * @win: the window size (bytes); if 0, reset to default
 */
extern void tcp_set_default_window(uint32_t win);

extern void tcp_set_nonblocking(tcpconn_t *c, bool nonblocking);

/**
 * __tcp_listen - creates a TCP listening queue for a local address. If token is
 * not NULL, it's associated laddr will be considered instead of @laddr. If this
 * call succeeds (e,g, returns 0), then the token has been consumed and no
 * longer belongs to the caller.
 * @laddr: the local address
 * @backlog: the maximum number of unaccepted sockets to queue
 * @q_out: a pointer to store the newly created listening queue
 * @token: the token to use for the listening queue (may be NULL)
 */
extern int __tcp_listen(struct netaddr laddr, int backlog, tcpqueue_t **q_out,
	                    bind_token_t *token);

/**
 * tcp_listen - creates a TCP listening queue for a local address
 * @laddr: the local address to listen on
 * @backlog: the maximum number of unaccepted sockets to queue
 * @q_out: a pointer to store the newly created listening queue
 *
 * Returns 0 if successful, otherwise fails.
 */
static inline int tcp_listen(struct netaddr laddr, int backlog,
                             tcpqueue_t **q_out)
{
	return __tcp_listen(laddr, backlog, q_out, NULL);
}
extern int tcp_accept(tcpqueue_t *q, tcpconn_t **c_out);
extern void tcp_qshutdown(tcpqueue_t *q);
extern void tcp_qclose(tcpqueue_t *q);
extern void tcpq_set_nonblocking(tcpqueue_t *q, bool nonblocking);
extern struct netaddr tcpq_local_addr(tcpqueue_t *q);
extern int tcpq_backlog(tcpqueue_t *q);
extern struct netaddr tcp_local_addr(tcpconn_t *c);
extern struct netaddr tcp_remote_addr(tcpconn_t *c);
extern int tcp_get_status(tcpconn_t *c);
extern uint32_t tcp_get_input_bytes(tcpconn_t *c);

extern ssize_t tcp_read2(tcpconn_t *c, void *buf, size_t len, bool peek,
                         bool nonblocking);
extern ssize_t tcp_write3(tcpconn_t *c, const void *buf, size_t len,
                          bool nonblocking, bool no_partial_write);
extern ssize_t tcp_readv2(tcpconn_t *c, const struct iovec *iov, int iovcnt,
                          bool peek, bool nonblocking);
extern ssize_t tcp_writev2(tcpconn_t *c, const struct iovec *iov, int iovcnt,
                           bool nonblocking);

static inline ssize_t tcp_write2(tcpconn_t *c, const void *buf, size_t len,
                                 bool nonblocking)
{
	return tcp_write3(c, buf, len, nonblocking, false);
}

static inline ssize_t tcp_read(tcpconn_t *c, void *buf, size_t len)
{
	return tcp_read2(c, buf, len, false, false);
}

static inline ssize_t tcp_write(tcpconn_t *c, const void *buf, size_t len)
{
	return tcp_write2(c, buf, len, false);
}

static inline ssize_t tcp_readv(tcpconn_t *c, const struct iovec *iov,
                                int iovcnt)
{
	return tcp_readv2(c, iov, iovcnt, false, false);
}

static inline ssize_t tcp_writev(tcpconn_t *c, const struct iovec *iov,
                                 int iovcnt)
{
	return tcp_writev2(c, iov, iovcnt, false);
}

extern int tcp_shutdown(tcpconn_t *c, int how);
extern void tcp_abort(tcpconn_t *c);
extern void tcp_close(tcpconn_t *c);

extern void tcp_poll_install_cb(tcpconn_t *c, poll_notif_fn_t setfn,
			                    poll_notif_fn_t clearfn, unsigned long data);
extern void tcpq_poll_install_cb(tcpqueue_t *q, poll_notif_fn_t setfn,
			                    poll_notif_fn_t clearfn, unsigned long data);

