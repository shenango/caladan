/*
 * RPC client-side support
 */

#include <base/time.h>
#include <base/stddef.h>
#include <base/list.h>
#include <base/log.h>
#include <base/atomic.h>
#include <runtime/smalloc.h>
#include <runtime/sync.h>
#include <runtime/timer.h>

#include <breakwater/breakwater.h>

#include "util.h"
#include "bw_proto.h"
#include "bw_config.h"


#define CBW_TRACK_FLOW			false
#define CBW_TRACK_FLOW_ID		1

/**
 * crpc_send_winupdate - send WINUPDATE message to update window size
 * @s: the RPC session to update the window
 *
 * On success, returns 0. On failure returns standard socket errors (< 0)
 */
ssize_t crpc_send_winupdate(struct cbw_session *s)
{
        struct cbw_hdr chdr;
        ssize_t ret;

	assert_mutex_held(&s->lock);

	/* construct the client header */
	chdr.magic = BW_REQ_MAGIC;
	chdr.op = BW_OP_WINUPDATE;
	chdr.id = 0;
	chdr.len = 0;
	chdr.demand = s->head - s->tail;
	chdr.flags = 0;
	if (s->demand_sync)
		chdr.flags |= BW_CFLAG_DSYNC;

	/* send the request */
	ret = tcp_write_full(s->cmn.c, &chdr, sizeof(chdr));
	if (unlikely(ret < 0))
		return ret;

	assert(ret == sizeof(chdr));
	s->winu_tx_++;

#if CBW_TRACK_FLOW
	if (s->id == CBW_TRACK_FLOW_ID) {
		printf("[%lu] <=== winupdate: demand = %lu, win = %u/%u\n",
		       microtime(), chdr.demand, s->win_used, s->win_avail);
	}
#endif
	return 0;
}

static ssize_t crpc_send_request_vector(struct cbw_session *s)
{
	struct cbw_hdr chdr[CRPC_QLEN];
	struct iovec v[CRPC_QLEN * 2];
	int nriov = 0;
	int nrhdr = 0;
	ssize_t ret;
	uint64_t now = microtime();

	assert_mutex_held(&s->lock);

	if (s->head == s->tail || s->win_used >= s->win_avail)
		return 0;

	while (s->head != s->tail && s->win_used < s->win_avail) {
		struct crpc_ctx *c = s->qreq[s->tail++ % CRPC_QLEN];

		chdr[nrhdr].magic = BW_REQ_MAGIC;
		chdr[nrhdr].op = BW_OP_CALL;
		chdr[nrhdr].id = c->id;
		chdr[nrhdr].len = c->len;
		chdr[nrhdr].demand = s->head - s->tail;
		chdr[nrhdr].ts_sent = now;
		chdr[nrhdr].flags = 0;
		if (s->demand_sync)
			chdr[nrhdr].flags |= BW_CFLAG_DSYNC;

		v[nriov].iov_base = &chdr[nrhdr];
		v[nriov].iov_len = sizeof(struct cbw_hdr);
		nrhdr++;
		nriov++;

		if (c->len > 0) {
			v[nriov].iov_base = c->buf;
			v[nriov++].iov_len = c->len;
		}

		s->win_used++;
	}

	if (s->head == s->tail) {
		s->head = 0;
		s->tail = 0;
	}

	ret = tcp_writev_full(s->cmn.c, v, nriov);

	s->req_tx_ += nrhdr;

#if CBW_TRACK_FLOW
	if (s->id == CBW_TRACK_FLOW_ID) {
		printf("[%lu] <=== request (%d): qlen=%d win=%d/%d\n",
		       microtime(), nrhdr, s->head-s->tail, s->win_used, s->win_avail);
	}
#endif

	if (unlikely(ret < 0))
		return ret;
	return 0;
}

static ssize_t crpc_send_raw(struct cbw_session *s,
			     const void *buf, size_t len,
			     uint64_t id)
{
	struct iovec vec[2];
	struct cbw_hdr chdr;
	ssize_t ret;
	uint64_t now = microtime();

	/* initialize the header */
	chdr.magic = BW_REQ_MAGIC;
	chdr.op = BW_OP_CALL;
	chdr.id = id;
	chdr.len = len;
	chdr.demand = s->head - s->tail;
	chdr.ts_sent = now;
	chdr.flags = 0;
	if (s->demand_sync)
		chdr.flags |= BW_CFLAG_DSYNC;

	/* initialize the SG vector */
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

#if CBW_TRACK_FLOW
	if (s->id == CBW_TRACK_FLOW_ID) {
		printf("[%lu] <=== request: id=%lu, demand = %lu, win = %u/%u\n",
		       now, chdr.id, chdr.demand, s->win_used, s->win_avail);
	}
#endif
	return len;
}

static void crpc_drain_queue(struct cbw_session *s)
{
	int pos;
	struct crpc_ctx *c;
	uint64_t now = microtime();

	assert_mutex_held(&s->lock);

	if (s->head == s->tail || s->waiting_winupdate)
		return;

	if (s->win_avail == 0 && s->demand_sync) {
		s->waiting_winupdate = true;
		crpc_send_winupdate(s);
		return;
	}

	while (s->head != s->tail) {
		pos = s->tail % CRPC_QLEN;
		c = s->qreq[pos];
		if (now - c->ts <= CBW_MAX_CLIENT_DELAY_US)
			break;

		s->tail++;
		s->req_dropped_++;
#if CBW_TRACK_FLOW
		if (s->id == CBW_TRACK_FLOW_ID) {
			printf("[%lu] request dropped: id=%lu, qlen = %d\n",
			       now, c->id, s->head - s->tail);
		}
#endif
	}

	crpc_send_request_vector(s);
}

static bool crpc_enqueue_one(struct cbw_session *s,
			     const void *buf, size_t len)
{
	int pos;
	struct crpc_ctx *c;
	uint64_t now = microtime();

	assert_mutex_held(&s->lock);

	/* if the queue is full, drop tail */
	if (s->head - s->tail >= CRPC_QLEN) {
		s->tail++;
		s->req_dropped_++;
#if CBW_TRACK_FLOW
		if (s->id == CBW_TRACK_FLOW_ID) {
			printf("[%lu] queue full. drop the request\n",
			       now);
		}
#endif
	}

	pos = s->head++ % CRPC_QLEN;
	c = s->qreq[pos];
	memcpy(c->buf, buf, len);
	c->id = s->req_id++;
	c->ts = now;
	c->len = len;

#if CBW_TRACK_FLOW
	if (s->id == CBW_TRACK_FLOW_ID) {
		printf("[%lu] request enqueued: id=%lu, qlen = %d, waiting_winupdate=%d\n",
		       now, c->id, s->head - s->tail, s->waiting_winupdate);
	}
#endif

	// very first message
	if (!s->init) {
		crpc_send_winupdate(s);
		s->waiting_winupdate = true;
		s->init = true;
	}

	// if queue become non-empty, start expiration loop
	if (s->head - s->tail == 1)
		condvar_signal(&s->timer_cv);

	return true;
}

ssize_t cbw_send_one(struct crpc_session *s_,
		      const void *buf, size_t len, int hash)
{
	struct cbw_session *s = (struct cbw_session *)s_;
	ssize_t ret;

	/* implementation is currently limited to a maximum payload size */
	if (unlikely(len > SRPC_BUF_SIZE))
		return -E2BIG;

	mutex_lock(&s->lock);

	/* hot path, just send */
	if (s->win_used < s->win_avail && s->head == s->tail) {
		s->win_used++;
		ret = crpc_send_raw(s, buf, len, s->req_id++);
		mutex_unlock(&s->lock);
		return ret;
	}

	/* cold path, enqueue request and drain the queue */
	if (!crpc_enqueue_one(s, buf, len)) {
		crpc_drain_queue(s);
		mutex_unlock(&s->lock);
		return -ENOBUFS;
	}
	crpc_drain_queue(s);
	mutex_unlock(&s->lock);

	return len;
}

ssize_t cbw_recv_one(struct crpc_session *s_, void *buf, size_t len,
		     uint64_t *latency)
{
	struct cbw_session *s = (struct cbw_session *)s_;
	struct sbw_hdr shdr;
	ssize_t ret;
	uint64_t now;

again:
	/* read the server header */
	ret = tcp_read_full(s->cmn.c, &shdr, sizeof(shdr));
	if (unlikely(ret <= 0))
		return ret;
	assert(ret == sizeof(shdr));

	/* parse the server header */
	if (unlikely(shdr.magic != BW_RESP_MAGIC)) {
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
	case BW_OP_CALL:
		/* read the payload */
		if (shdr.len > 0) {
			ret = tcp_read_full(s->cmn.c, buf, shdr.len);
			if (unlikely(ret <= 0))
				return ret;
			assert(ret == shdr.len);
			s->resp_rx_++;
		}

		/* update the window */
		mutex_lock(&s->lock);
		assert(s->win_used > 0);
		s->win_used--;
		s->win_avail = shdr.win;
		s->waiting_winupdate = false;

#if CBW_TRACK_FLOW
		if (s->id == CBW_TRACK_FLOW_ID) {
			printf("[%lu] ===> response: id=%lu, shdr.win=%lu, win=%u/%u\n",
			       now, shdr.id, shdr.win, s->win_used, s->win_avail);
		}
#endif

		if (s->win_avail > 0) {
			crpc_drain_queue(s);
		}

		if ((shdr.flags & BW_SFLAG_DROP) && latency) {
			*latency = now - shdr.ts_sent;
		}

		mutex_unlock(&s->lock);

		break;
	case BW_OP_WINUPDATE:
		if (unlikely(shdr.len != 0)) {
			log_warn("crpc: winupdate has nonzero len");
			return -EINVAL;
		}
		assert(shdr.len == 0);

		/* update the window */
		mutex_lock(&s->lock);
		s->win_avail = shdr.win;
		s->waiting_winupdate = false;

#if CBW_TRACK_FLOW
		if (s->id == CBW_TRACK_FLOW_ID) {
			printf("[%lu] ===> Winupdate: shdr.win=%lu, win=%u/%u\n",
			       microtime(), shdr.win, s->win_used, s->win_avail);
		}
#endif

		if (s->win_avail > 0) {
			crpc_drain_queue(s);
		}
		mutex_unlock(&s->lock);
		s->winu_rx_++;

		goto again;
	default:
		log_warn("crpc: got invalid op %d", shdr.op);
		return -EINVAL;
	}

	return shdr.len;
}

static void crpc_timer(void *arg)
{
	struct cbw_session *s = (struct cbw_session *)arg;
	uint64_t now;
	int pos;
	struct crpc_ctx *c;
	int num_drops;

	mutex_lock(&s->lock);
	while(true) {
		while (s->running && s->head == s->tail)
			condvar_wait(&s->timer_cv, &s->lock);

		if (!s->running)
			goto done;

		num_drops = 0;
		now = microtime();

		// Drop requests if expired
		while (s->head != s->tail) {
			pos = s->tail % CRPC_QLEN;
			c = s->qreq[pos];
			if (now - c->ts <= CBW_MAX_CLIENT_DELAY_US)
				break;

			s->tail++;
			s->req_dropped_++;
			num_drops++;
#if CBW_TRACK_FLOW
			if (s->id == CBW_TRACK_FLOW_ID) {
				printf("[%lu] request dropped: id=%lu, qlen = %d\n",
				       now, c->id, s->head - s->tail);
			}
#endif
		}

		// If queue becomes empty
		if (s->head == s->tail) {
			if (num_drops > 0 && s->demand_sync) {
				s->waiting_winupdate = false;
				crpc_send_winupdate(s);
			}
			continue;
		}

		// caculate next wake up time
		pos = (s->head - 1) % CRPC_QLEN;
		c = s->qreq[pos];
		mutex_unlock(&s->lock);
		timer_sleep_until(c->ts + CBW_MAX_CLIENT_DELAY_US);
		mutex_lock(&s->lock);
	}
done:
	mutex_unlock(&s->lock);
	waitgroup_done(&s->timer_waiter);
}

int cbw_open(struct netaddr raddr, struct crpc_session **sout, int id)
{
	struct netaddr laddr;
	struct cbw_session *s;
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
	mutex_init(&s->lock);
	condvar_init(&s->timer_cv);
	waitgroup_init(&s->timer_waiter);
	waitgroup_add(&s->timer_waiter, 1);
	s->running = true;
	s->demand_sync = false;
	if (id != -1)
		s->id = id;
	s->req_id = 1;
	*sout = (struct crpc_session *)s;

	ret = thread_spawn(crpc_timer, s);
	BUG_ON(ret);

	return 0;

fail:
	tcp_close(c);
	for (i = i - 1; i >= 0; i--)
		sfree(s->qreq[i]);
	sfree(s);
	return -ENOMEM;
}

void cbw_close(struct crpc_session *s_)
{
	struct cbw_session *s = (struct cbw_session *)s_;
	int i;

	mutex_lock(&s->lock);
	s->running = false;
	condvar_signal(&s->timer_cv);
	mutex_unlock(&s->lock);

	waitgroup_wait(&s->timer_waiter);

	tcp_close(s->cmn.c);
	for(i = 0; i < CRPC_QLEN; ++i)
		sfree(s->qreq[i]);
	sfree(s);
}

/* client-side stats */
uint32_t cbw_win_avail(struct crpc_session *s_)
{
	struct cbw_session *s = (struct cbw_session *)s_;
	return s->win_avail;
}

void cbw_stat_clear(struct crpc_session *s_)
{
	return;
}

uint64_t cbw_stat_win_expired(struct crpc_session *s_)
{
	struct cbw_session *s = (struct cbw_session *)s_;
	return s->win_expired_;
}

uint64_t cbw_stat_winu_rx(struct crpc_session *s_)
{
	struct cbw_session *s = (struct cbw_session *)s_;
	return s->winu_rx_;
}

uint64_t cbw_stat_winu_tx(struct crpc_session *s_)
{
	struct cbw_session *s = (struct cbw_session *)s_;
	return s->winu_tx_;
}

uint64_t cbw_stat_resp_rx(struct crpc_session *s_)
{
	struct cbw_session *s = (struct cbw_session *)s_;
	return s->resp_rx_;
}

uint64_t cbw_stat_req_tx(struct crpc_session *s_)
{
	struct cbw_session *s = (struct cbw_session *)s_;
	return s->req_tx_;
}

uint64_t cbw_stat_req_dropped(struct crpc_session *s_)
{
	struct cbw_session *s = (struct cbw_session *)s_;
	return s->req_dropped_;
}

struct crpc_ops cbw_ops = {
	.crpc_send_one		= cbw_send_one,
	.crpc_recv_one		= cbw_recv_one,
	.crpc_open		= cbw_open,
	.crpc_close		= cbw_close,
	.crpc_win_avail		= cbw_win_avail,
	.crpc_stat_clear	= cbw_stat_clear,
	.crpc_stat_winu_rx	= cbw_stat_winu_rx,
	.crpc_stat_win_expired	= cbw_stat_win_expired,
	.crpc_stat_winu_tx	= cbw_stat_winu_tx,
	.crpc_stat_resp_rx	= cbw_stat_resp_rx,
	.crpc_stat_req_tx	= cbw_stat_req_tx,
	.crpc_stat_req_dropped	= cbw_stat_req_dropped,
};
