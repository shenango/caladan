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
#include "proto.h"

/* the maximum supported window size */
#define CRPC_MAX_WINDOW		64
/* the minimum runtime queuing delay */
#define CRPC_MIN_DELAY_US	20
/* the maximum runtime queuing delay */
#define CRPC_MAX_DELAY_US	60
/* the credit expiration time */
#define CRPC_CREDIT_EXPIRE_US	20

static ssize_t crpc_send_raw(struct crpc_session *s, bool probe,
			     const void *buf, size_t len)
{
	struct iovec vec[2];
	struct crpc_hdr chdr;
	ssize_t ret;

	/* initialize the header */
	chdr.magic = RPC_REQ_MAGIC;
	chdr.op = probe ? RPC_OP_PROBE : RPC_OP_CALL;
	chdr.len = len;
	chdr.demand = s->head - s->tail;

	/* initialize the SG vector */
	vec[0].iov_base = &chdr;
	vec[0].iov_len = sizeof(chdr);
	vec[1].iov_base = (void *)buf;
	vec[1].iov_len = len;

	/* send the request */
	ret = tcp_writev_full(s->c, vec, 2);
	if (unlikely(ret < 0))
		return ret;
	assert(ret == sizeof(chdr) + len);

	return len;
}

static void crpc_drain_queue(struct crpc_session *s)
{
	bool can_probe = !s->probe_sent && s->win_avail == 0 &&
			 s->win_used == 0 && s->probe_wait_ts <= microtime();
	bool do_probe = false;
	ssize_t ret;
	int pos;

	assert_mutex_held(&s->lock);

	/* try to drain queued requests */
	while (s->head != s->tail) {
		/* is the window empty? */
		if (s->win_used >= s->win_avail) {
			if (!can_probe)
				break;
			do_probe = true;
		}

		pos = s->tail++ % CRPC_QLEN;
		ret = crpc_send_raw(s, do_probe, s->bufs[pos], s->lens[pos]);
		if (ret < 0)
			break;
		assert(ret == s->lens[pos]);
		s->win_used++;
		if (do_probe)
			break;
	}
}

static bool crpc_enqueue_one(struct crpc_session *s,
			     const void *buf, size_t len)
{
	int pos;

	assert_mutex_held(&s->lock);

	/* is the queue full? */
	if (s->head - s->tail >= CRPC_QLEN)
		return false;

	/* if can't probe, drop the request */
	if (s->probe_wait_ts > microtime())
		return false;

	pos = s->head++ % CRPC_QLEN;
	memcpy(s->bufs[pos], buf, len);
	s->lens[pos] = len;
	return true;
}

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
	ssize_t ret;

	/* implementation is currently limited to a maximum payload size */
	if (unlikely(len > SRPC_BUF_SIZE))
		return -E2BIG;

	mutex_lock(&s->lock);

	/* expire stale credits */
	if (microtime() >= s->win_update_ts + CRPC_CREDIT_EXPIRE_US)
		s->win_avail = s->win_used;

	/* hot path, just send */
	if (s->win_used < s->win_avail && s->head == s->tail) {
		s->win_used++;
		ret = crpc_send_raw(s, false, buf, len);
		mutex_unlock(&s->lock);
		return ret;
	}

	/* cold path, enqueue request and drain queue */
	if (!crpc_enqueue_one(s, buf, len)) {
		mutex_unlock(&s->lock);
		return -ENOBUFS;
	}
	crpc_drain_queue(s);
	mutex_unlock(&s->lock);

	return len;
}

static void crpc_update_window(struct crpc_session *s, uint64_t us)
{
	float alpha;

	/* update window (currently AIMD w/ DCTCP tweak) */
	if (us >= CRPC_MIN_DELAY_US) {
		us = MIN(CRPC_MAX_DELAY_US, us);
		alpha = (float)(us - CRPC_MIN_DELAY_US) /
			(float)(CRPC_MAX_DELAY_US - CRPC_MIN_DELAY_US);
		s->win_avail = (float)s->win_avail * (1.0 - alpha / 2.0);
	} else {
		s->win_avail++;
	}

	/* clamp to supported values */
	s->win_avail = MIN(s->win_avail, CRPC_MAX_WINDOW - 1);
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

again:
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
	if (unlikely(shdr.op >= RPC_OP_MAX)) {
		log_warn("crpc: got invalid op %d", shdr.op);
		return -EINVAL;
	}
	if (unlikely(shdr.len > MIN(SRPC_BUF_SIZE, len))) {
		log_warn("crpc: request len %ld too large (limit %ld)",
			 shdr.len, MIN(SRPC_BUF_SIZE, len));
		return -EINVAL;
	}

	/* receive the payload */
	if (shdr.len) {
		ret = tcp_read_full(s->c, buf, shdr.len);
		if (unlikely(ret <= 0))
			return ret;
		assert(ret == shdr.len);
	}

	mutex_lock(&s->lock);
	crpc_update_window(s, shdr.delay_us);
	assert(s->win_used > 0);
	s->win_used--;
	s->probe_sent = false;

	/* drain the queue and send another probe if allowed/needed */
	if (shdr.delay_us > CRPC_MIN_DELAY_US || !shdr.accepted)
		s->probe_wait_ts = microtime() + shdr.probe_us;
	else
		s->probe_wait_ts = 0;
	crpc_drain_queue(s);

	/* if the delay was high, drop the remaining queue */
	if (shdr.delay_us > CRPC_MIN_DELAY_US || !shdr.accepted)
		s->head = s->tail = 0;

	s->win_update_ts = microtime();
	mutex_unlock(&s->lock);

	if (shdr.op != RPC_OP_CALL)
		goto again;
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
	int i, ret;

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
	memset(s, 0, sizeof(*s));

	for (i = 0; i < CRPC_QLEN; i++) {
		s->bufs[i] = smalloc(SRPC_BUF_SIZE);
		if (!s->bufs[i])
			goto fail;
	}

	s->c = c;
	mutex_init(&s->lock);
	*sout = s;
	return 0;

fail:
	tcp_close(c);
	for (i = i - 1; i >= 0; i--)
		sfree(s->bufs[i]);
	sfree(s);
	return -ENOMEM;
}

/**
 * crpc_close - closes an RPC session
 * @s: the session to close
 *
 * WARNING: This function could block.
 */
void crpc_close(struct crpc_session *s)
{
	int i;

	tcp_close(s->c);
	for (i = 0; i < CRPC_QLEN; i++)
		sfree(s->bufs[i]);
	sfree(s);
}
