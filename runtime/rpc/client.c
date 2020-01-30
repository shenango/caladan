/*
 * RPC client-side support
 */

#include <base/stddef.h>
#include <base/list.h>
#include <base/log.h>
#include <base/atomic.h>
#include <runtime/rpc.h>
#include <runtime/smalloc.h>

#include "util.h"

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
ssize_t crpc_send_one(struct crpc_session *s,
		      const void *buf, size_t len)
{
	struct crpc_hdr *chdr;
	ssize_t ret;
  ssize_t pkt_len = sizeof(struct crpc_hdr) + len;

  if (pkt_len > SRPC_BUF_SIZE)
    return -ENOBUFS;

	/* adjust the window */
	if (atomic_read(&s->win_used) >= s->win_avail)
		return -ENOBUFS;
	atomic_inc(&s->win_used);

	/* send the client header */
  chdr = (struct crpc_hdr *)(s->buf);
	chdr->magic = RPC_REQ_MAGIC;
	chdr->op = RPC_OP_CALL;
	chdr->len = len;

  /* copy the payload */
  memcpy(s->buf + sizeof(struct crpc_hdr), buf, len);

	/* send the request */
	ret = tcp_write_full(s->c, s->buf, pkt_len);
	if (unlikely(ret < 0))
		return ret;
	assert(ret == pkt_len);

	return len;
}

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
ssize_t crpc_recv_one(struct crpc_session *s, void *buf, size_t len)
{
	struct srpc_hdr shdr;
	ssize_t ret;

	/* read the server header */
	ret = tcp_read_full(s->c, &shdr, sizeof(shdr));
	if (unlikely(ret <= 0))
		return ret;
	assert(ret == sizeof(shdr));

	/* parse the server header */
	if (unlikely(shdr.magic != RPC_RESP_MAGIC)) {
		log_warn("crpc: got invalid magic %x", shdr.magic);
		return -EINVAL;
	}
	if (unlikely(shdr.len > MIN(SRPC_BUF_SIZE, len))) {
		log_warn("srpc: request len %ld too large (limit %ld)",
			 shdr.len, MIN(SRPC_BUF_SIZE, len));
		return -EINVAL;
	}
	if (unlikely(shdr.op != RPC_OP_CALL)) {
		log_warn("srpc: got invalid op %d", shdr.op);
		return -EINVAL;
	}

	/* receive the payload */
	ret = tcp_read_full(s->c, buf, shdr.len);
 	if (unlikely(ret <= 0))
		return ret;
	assert(ret == shdr.len);

	/* adjust the window */
	assert(atomic_read(&s->win_used) > 0);
	atomic_dec(&s->win_used);
	ACCESS_ONCE(s->win_avail) = shdr.win;

	return shdr.len;
}

/**
 * crpc_open - creates an RPC session
 * @raddr: the remote address to connect to (port must be SRPC_PORT)
 * @sout: the connection session that was created
 *
 * WARNING: This function could block.
 *
 * Returns 0 if successful.
 */
int crpc_open(struct netaddr raddr, struct crpc_session **sout)
{
	struct netaddr laddr;
	struct crpc_session *s;
	tcpconn_t *c;
	int ret;

	/* set up ephemeral IP and port */
	laddr.ip = 0;
	laddr.port = 0;

	if (raddr.port != SRPC_PORT)
		return -EINVAL;

	ret = tcp_dial(laddr, raddr, &c);
	if (ret)
		return ret;

	s = smalloc(sizeof(*s));
	if (!s) {
		tcp_close(c);
		return -ENOMEM;
	}

	s->c = c;
	s->win_avail = 1;
	atomic_write(&s->win_used, 0);
	*sout = s;
	return 0;
}

/**
 * crpc_close - closes an RPC session
 * @s: the session to close
 *
 * WARNING: This function could block.
 */
void crpc_close(struct crpc_session *s)
{
	tcp_close(s->c);
	sfree(s);
}
