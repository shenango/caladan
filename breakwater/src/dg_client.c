/*
 * RPC client-side support
 */

#include <time.h>
#include <stdio.h>
#include <base/time.h>
#include <base/stddef.h>
#include <base/list.h>
#include <base/log.h>
#include <base/atomic.h>
#include <runtime/smalloc.h>
#include <runtime/sync.h>
#include <runtime/timer.h>

#include <breakwater/dagor.h>

#include "util.h"
#include "dg_proto.h"
#include "dg_config.h"

#define CDG_TRACK_FLOW			false
#define CDG_TRACK_FLOW_ID		1

static ssize_t crpc_send_request_vector(struct cdg_session *s)
{
	assert_mutex_held(&s->lock);
	struct cdg_hdr chdr[CRPC_QLEN];
	struct iovec v[CRPC_QLEN * 2];
	int nriov = 0;
	int nrhdr = 0;
	ssize_t ret;
	uint64_t now = microtime();

	while (s->head != s->tail) {
		struct cdg_ctx *c = s->qreq[s->tail++ % CRPC_QLEN];

		chdr[nrhdr].magic = DG_REQ_MAGIC;
		chdr[nrhdr].op = DG_OP_CALL;
		chdr[nrhdr].id = c->cmn.id;
		chdr[nrhdr].len = c->cmn.len;
		chdr[nrhdr].prio = c->prio;
		chdr[nrhdr].ts_sent = now;

		v[nriov].iov_base = &chdr[nrhdr];
		v[nriov].iov_len = sizeof(struct cdg_hdr);
		nrhdr++;
		nriov++;

		if (c->cmn.len > 0) {
			v[nriov].iov_base = c->cmn.buf;
			v[nriov++].iov_len = c->cmn.len;
		}
	}

	if (nriov == 0)
		return 0;
	ret = tcp_writev_full(s->cmn.c, v, nriov);

	s->req_tx_ += nrhdr;

	s->head = 0;
	s->tail = 0;

	if (unlikely(ret < 0))
		return ret;
	return 0;
}

static bool crpc_enqueue_one(struct cdg_session *s,
			     const void *buf, size_t len, int hash)
{
	int pos;
	struct cdg_ctx *c;
	uint64_t now = microtime();
	int prio = hash % DG_MAX_PRIO;

	assert_mutex_held(&s->lock);

	if (s->head - s->tail >= CRPC_QLEN || prio > s->local_prio) {
		s->req_dropped_++;
		//printf("[WARNING] Request dropped due to full queue\n");
		return false;
	}

	pos = s->head++ % CRPC_QLEN;
	c = s->qreq[pos];
	memcpy(c->cmn.buf, buf, len);
	c->cmn.id = s->req_id++;
	c->cmn.len = len;
	c->cmn.ts = now;
	c->prio = prio;

	if (s->head - s->tail == 1)
		condvar_signal(&s->sender_cv);

	return true;
}

ssize_t cdg_send_one(struct crpc_session *s_,
		      const void *buf, size_t len, int hash)
{
	struct cdg_session *s = (struct cdg_session *)s_;

	/* implementation is currently limited to a maximum payload size */
	if (unlikely(len > SRPC_BUF_SIZE))
		return -E2BIG;

	mutex_lock(&s->lock);

	/* hot path, just send */
	crpc_enqueue_one(s, buf, len, hash);
	mutex_unlock(&s->lock);

	return len;
}

ssize_t cdg_recv_one(struct crpc_session *s_, void *buf, size_t len,
		     uint64_t *latency)
{
	struct cdg_session *s = (struct cdg_session *)s_;
	struct sdg_hdr shdr;
	ssize_t ret;
	uint64_t now;

	/* read the server header */
	ret = tcp_read_full(s->cmn.c, &shdr, sizeof(shdr));
	if (unlikely(ret <= 0))
		return ret;
	assert(ret == sizeof(shdr));

	/* parse the server header */
	if (unlikely(shdr.magic != DG_RESP_MAGIC)) {
		log_warn("crpc: got invalid magic %x", shdr.magic);
		return -EINVAL;
	}
	if (unlikely(shdr.len > MIN(SRPC_BUF_SIZE, len))) {
		log_warn("crpc: request len %ld too large (limit %ld)",
			 shdr.len, MIN(SRPC_BUF_SIZE, len));
		return -EINVAL;
	}

	now = microtime();
	switch (shdr.op) {
	case DG_OP_CALL:
		/* read the payload */
		if (shdr.len > 0) {
			ret = tcp_read_full(s->cmn.c, buf, shdr.len);
			if (unlikely(ret <= 0))
				return ret;
			assert(ret == shdr.len);
			s->resp_rx_++;
		}

		mutex_lock(&s->lock);
		s->local_prio = shdr.prio;

		if ((shdr.flags & DG_SFLAG_DROP) && latency)
			*latency = now - shdr.ts_sent;

		mutex_unlock(&s->lock);

#if CDG_TRACK_FLOW
		if (s->id == CDG_TRACK_FLOW_ID) {
			printf("[%lu] ===> response: id=%lu, prio=%d\n",
			       now, shdr.id, shdr.prio);
		}
#endif

		break;
	case DG_OP_WINUPDATE:
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
	struct cdg_session *s = (struct cdg_session *)arg;

	mutex_lock(&s->lock);
	while(true) {
		while (s->running && s->head == s->tail)
			condvar_wait(&s->sender_cv, &s->lock);

		if (!s->running)
			goto done;

		// Wait for batching
		if (s->head - s->tail < CRPC_QLEN) {
			mutex_unlock(&s->lock);
			timer_sleep(CDG_BATCH_WAIT_US);
			mutex_lock(&s->lock);
		}

		// Batch sending
		crpc_send_request_vector(s);
	}

done:
	mutex_unlock(&s->lock);
	waitgroup_done(&s->sender_waiter);
}

int cdg_open(struct netaddr raddr, struct crpc_session **sout, int id)
{
	struct netaddr laddr;
	struct cdg_session *s;
	tcpconn_t *c;
	int i, ret;

	srand(time(NULL));

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
		s->qreq[i] = smalloc(sizeof(struct cdg_ctx));
		if (!s->qreq[i])
			goto fail;
	}

	s->cmn.c = c;
	s->running = true;
	s->local_prio = rand() % 128;
	mutex_init(&s->lock);
	condvar_init(&s->sender_cv);
	waitgroup_init(&s->sender_waiter);
	waitgroup_add(&s->sender_waiter, 1);
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

void cdg_close(struct crpc_session *s_)
{
	struct cdg_session *s = (struct cdg_session *)s_;
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
uint32_t cdg_win_avail(struct crpc_session *s_)
{
	return 0;
}

void cdg_stat_clear(struct crpc_session *s_)
{
	return;
}

uint64_t cdg_stat_win_expired(struct crpc_session *s_)
{
	return 0;
}

uint64_t cdg_stat_winu_rx(struct crpc_session *s_)
{
	return 0;
}

uint64_t cdg_stat_winu_tx(struct crpc_session *s_)
{
	return 0;
}

uint64_t cdg_stat_resp_rx(struct crpc_session *s_)
{
	struct cdg_session *s = (struct cdg_session *)s_;
	return s->resp_rx_;
}

uint64_t cdg_stat_req_tx(struct crpc_session *s_)
{
	struct cdg_session *s = (struct cdg_session *)s_;
	return s->req_tx_;
}

uint64_t cdg_stat_req_dropped(struct crpc_session *s_)
{
	struct cdg_session *s = (struct cdg_session *)s_;
	return s->req_dropped_;
}

struct crpc_ops cdg_ops = {
	.crpc_send_one		= cdg_send_one,
	.crpc_recv_one		= cdg_recv_one,
	.crpc_open		= cdg_open,
	.crpc_close		= cdg_close,
	.crpc_win_avail		= cdg_win_avail,
	.crpc_stat_clear	= cdg_stat_clear,
	.crpc_stat_winu_rx	= cdg_stat_winu_rx,
	.crpc_stat_win_expired	= cdg_stat_win_expired,
	.crpc_stat_winu_tx	= cdg_stat_winu_tx,
	.crpc_stat_resp_rx	= cdg_stat_resp_rx,
	.crpc_stat_req_tx	= cdg_stat_req_tx,
	.crpc_stat_req_dropped	= cdg_stat_req_dropped,
};
