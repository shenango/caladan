/*
 * RPC client-side support
 */

#include <stdio.h>
#include <base/time.h>
#include <base/stddef.h>
#include <base/list.h>
#include <base/log.h>
#include <base/atomic.h>
#include <runtime/smalloc.h>
#include <runtime/sync.h>
#include <runtime/timer.h>

#include <breakwater/nocontrol.h>

#include "util.h"
#include "nc_proto.h"

#define CNC_TRACK_FLOW			false
#define CNC_TRACK_FLOW_ID		1

static ssize_t crpc_send_request_vector(struct cnc_session *s)
{
	struct cnc_hdr chdr[CRPC_QLEN];
	struct iovec v[CRPC_QLEN * 2];
	int nriov = 0;
	int nrhdr = 0;
	ssize_t ret;
	uint64_t now = microtime();

	assert_mutex_held(&s->lock);

	if (s->head == s->tail)
		return 0;

	while (s->head != s->tail) {
		struct crpc_ctx *c = s->qreq[s->tail++ % CRPC_QLEN];

		chdr[nrhdr].magic = NC_REQ_MAGIC;
		chdr[nrhdr].op = NC_OP_CALL;
		chdr[nrhdr].id = c->id;
		chdr[nrhdr].len = c->len;
		chdr[nrhdr].ts = now;

		v[nriov].iov_base = &chdr[nrhdr];
		v[nriov].iov_len = sizeof(struct cnc_hdr);
		nrhdr++;
		nriov++;

		if (c->len > 0) {
			v[nriov].iov_base = c->buf;
			v[nriov++].iov_len = c->len;
		}
	}

	if (s->head == s->tail) {
		s->head = 0;
		s->tail = 0;
	}

	if (nriov == 0)
		return 0;
	ret = tcp_writev_full(s->cmn.c, v, nriov);

	s->req_tx_ += nrhdr;

	if (unlikely(ret < 0))
		return ret;
	return 0;
}

static ssize_t crpc_send_raw(struct cnc_session *s,
			     const void *buf, size_t len,
			     uint64_t id)
{
	struct iovec vec[2];
	struct cnc_hdr chdr;
	ssize_t ret;

	/* header */
	chdr.magic = NC_REQ_MAGIC;
	chdr.op = NC_OP_CALL;
	chdr.id = id;
	chdr.len = len;
	chdr.ts = microtime();

	/* SG vector */
	vec[0].iov_base = &chdr;
	vec[0].iov_len = sizeof(chdr);
	vec[1].iov_base = (void *)buf;
	vec[1].iov_len = len;

	/* send the request */
	ret = tcp_writev_full(s->cmn.c, vec, 2);
	if (unlikely(ret < 0))
		return ret;
	assert(ret == sizeof(chdr) + len);
	s->req_tx_++;

	return len;
}

ssize_t cnc_send_one(struct crpc_session *s_,
		      const void *buf, size_t len, int hash)
{
	struct cnc_session *s = (struct cnc_session *)s_;
	ssize_t ret;

	/* implementation is currently limited to a maximum payload size */
	if (unlikely(len > SRPC_BUF_SIZE))
		return -E2BIG;

	mutex_lock(&s->lock);

	/* hot path, just send */
	ret = crpc_send_raw(s, buf, len, s->req_id++);
	mutex_unlock(&s->lock);
	return ret;
}

ssize_t cnc_recv_one(struct crpc_session *s_, void *buf, size_t len,
		     uint64_t *latency)
{
	struct cnc_session *s = (struct cnc_session *)s_;
	struct snc_hdr shdr;
	ssize_t ret;

again:
	/* read the server header */
	ret = tcp_read_full(s->cmn.c, &shdr, sizeof(shdr));
	if (unlikely(ret <= 0))
		return ret;
	assert(ret == sizeof(shdr));

	/* parse the server header */
	if (unlikely(shdr.magic != NC_RESP_MAGIC)) {
		log_warn("crpc: got invalid magic %x", shdr.magic);
		return -EINVAL;
	}
	if (unlikely(shdr.len > MIN(SRPC_BUF_SIZE, len))) {
		log_warn("crpc: request len %ld too large (limit %ld)",
			 shdr.len, MIN(SRPC_BUF_SIZE, len));
		return -EINVAL;
	}

	switch (shdr.op) {
	case NC_OP_CALL:
		if (shdr.len == 0)
			goto again;

		/* read the payload */
		ret = tcp_read_full(s->cmn.c, buf, shdr.len);
		if (unlikely(ret <= 0))
			return ret;
		assert(ret == shdr.len);
		s->resp_rx_++;

#if CNC_TRACK_FLOW
		if (s->id == CNC_TRACK_FLOW_ID) {
			printf("[%lu] ===> response: id=%lu\n",
			       microtime(), shdr.id);
		}
#endif

		break;
	case NC_OP_WINUPDATE:
		printf("Oops!\n");
		break;
	default:
		log_warn("crpc: got invalid op %d", shdr.op);
		return -EINVAL;
	}

	return shdr.len;
}

static void crpc_sender(void *arg)
{
	struct cnc_session *s = (struct cnc_session *)arg;

	mutex_lock(&s->lock);
	while (true) {
		while(s->running && s->head == s->tail)
			condvar_wait(&s->sender_cv, &s->lock);

		if (!s->running)
			goto done;

		crpc_send_request_vector(s);
	}

done:
	mutex_unlock(&s->lock);
	waitgroup_done(&s->sender_waiter);
}

int cnc_open(struct netaddr raddr, struct crpc_session **sout, int id)
{
	struct netaddr laddr;
	struct cnc_session *s;
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

	for (i = 0; i < CRPC_QLEN; ++i) {
		s->qreq[i] = smalloc(sizeof(struct crpc_ctx));
		if (!s->qreq[i])
			goto fail;
	}

	s->cmn.c = c;
	s->running = true;

	mutex_init(&s->lock);
	condvar_init(&s->sender_cv);
	waitgroup_init(&s->sender_waiter);
	waitgroup_add(&s->sender_waiter, 1);
	s->running = true;

	if (id != -1)
		s->id = id;
	s->req_id = 1;
	*sout = (struct crpc_session *)s;

	ret = thread_spawn(crpc_sender, s);
	BUG_ON(ret);

	return 0;

fail:
	tcp_close(c);
	for (i = i - 1; i >= 0; i--)
		sfree(s->qreq[i]);
	sfree(s);
	return -ENOMEM;
}

void cnc_close(struct crpc_session *s_)
{
	struct cnc_session *s = (struct cnc_session *)s_;
	int i;

	mutex_lock(&s->lock);
	s->running = false;
	condvar_signal(&s->sender_cv);
	mutex_unlock(&s->lock);

	waitgroup_wait(&s->sender_waiter);

	tcp_close(s->cmn.c);
	for (i = 0; i < CRPC_QLEN; ++i)
		sfree(s->qreq[i]);
	sfree(s);
}

/* client-side stats */
uint32_t cnc_win_avail(struct crpc_session *s_)
{
	return 0;
}

void cnc_stat_clear(struct crpc_session *s_)
{
	return;
}

uint64_t cnc_stat_win_expired(struct crpc_session *s_)
{
	struct cnc_session *s = (struct cnc_session *)s_;
	return s->win_expired_;
}

uint64_t cnc_stat_winu_rx(struct crpc_session *s_)
{
	struct cnc_session *s = (struct cnc_session *)s_;
	return s->winu_rx_;
}

uint64_t cnc_stat_winu_tx(struct crpc_session *s_)
{
	struct cnc_session *s = (struct cnc_session *)s_;
	return s->winu_tx_;
}

uint64_t cnc_stat_resp_rx(struct crpc_session *s_)
{
	struct cnc_session *s = (struct cnc_session *)s_;
	return s->resp_rx_;
}

uint64_t cnc_stat_req_tx(struct crpc_session *s_)
{
	struct cnc_session *s = (struct cnc_session *)s_;
	return s->req_tx_;
}

uint64_t cnc_stat_req_dropped(struct crpc_session *s_)
{
	struct cnc_session *s = (struct cnc_session *)s_;
	return s->req_dropped_;
}

struct crpc_ops cnc_ops = {
	.crpc_send_one		= cnc_send_one,
	.crpc_recv_one		= cnc_recv_one,
	.crpc_open		= cnc_open,
	.crpc_close		= cnc_close,
	.crpc_win_avail		= cnc_win_avail,
	.crpc_stat_clear	= cnc_stat_clear,
	.crpc_stat_winu_rx	= cnc_stat_winu_rx,
	.crpc_stat_win_expired	= cnc_stat_win_expired,
	.crpc_stat_winu_tx	= cnc_stat_winu_tx,
	.crpc_stat_resp_rx	= cnc_stat_resp_rx,
	.crpc_stat_req_tx	= cnc_stat_req_tx,
	.crpc_stat_req_dropped	= cnc_stat_req_dropped,
};
