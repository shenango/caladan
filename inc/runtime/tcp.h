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

extern int tcp_dial(struct netaddr laddr, struct netaddr raddr,
		    tcpconn_t **c_out);
extern int tcp_dial_nonblocking(struct netaddr laddr, struct netaddr raddr,
	                            tcpconn_t **c_out);
extern int tcp_dial_affinity(uint32_t affinity, struct netaddr raddr,
		    tcpconn_t **c_out);
extern int tcp_dial_conn_affinity(tcpconn_t *in, struct netaddr raddr,
		    tcpconn_t **c_out);

extern void tcp_set_nonblocking(tcpconn_t *c, bool nonblocking);

extern int tcp_listen(struct netaddr laddr, int backlog, tcpqueue_t **q_out);
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


extern ssize_t tcp_read2(tcpconn_t *c, void *buf, size_t len, bool peek, bool nonblocking);
extern ssize_t tcp_write2(tcpconn_t *c, const void *buf, size_t len, bool nonblocking);
extern ssize_t tcp_readv2(tcpconn_t *c, const struct iovec *iov, int iovcnt, bool peek, bool nonblocking);
extern ssize_t tcp_writev2(tcpconn_t *c, const struct iovec *iov, int iovcnt, bool nonblocking);

static inline ssize_t tcp_read(tcpconn_t *c, void *buf, size_t len)
{
	return tcp_read2(c, buf, len, false, false);
}

static inline ssize_t tcp_write(tcpconn_t *c, const void *buf, size_t len)
{
	return tcp_write2(c, buf, len, false);
}

static inline ssize_t tcp_readv(tcpconn_t *c, const struct iovec *iov, int iovcnt)
{
	return tcp_readv2(c, iov, iovcnt, false, false);
}

static inline ssize_t tcp_writev(tcpconn_t *c, const struct iovec *iov, int iovcnt)
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

