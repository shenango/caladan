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

#include <breakwater/seda.h>

#include "util.h"
#include "sd_proto.h"
#include "sd_config.h"

#define CSD_TRACK_FLOW			false
#define CSD_TRACK_FLOW_ID		1

static void tb_refill_token(struct csd_session *s) {
	double new_token;
	uint64_t now = microtime();

	assert_mutex_held(&s->lock);

	if (s->tb_last_refresh == 0) {
		s->tb_last_refresh = now;
		s->tb_token = 0;
		return;
	}

	new_token = (now - s->tb_last_refresh) * s->tb_refresh_rate / 1000000.0;
	s->tb_token += new_token;
	s->tb_last_refresh = now;

	s->tb_token = MIN(s->tb_token, CSD_TB_MAX_TOKEN);
}

static void tb_set_rate(struct csd_session *s, double new_rate)
{
	assert_mutex_held(&s->lock);
	tb_refill_token(s);
	s->tb_refresh_rate = MAX(new_rate, CSD_TB_MIN_RATE);
}

static void tb_sleep_until_next_token(struct csd_session *s)
{
	uint64_t sleep_until;

	assert_mutex_held(&s->lock);

	sleep_until = s->tb_last_refresh +
		(1.0 - s->tb_token) * 1000000 / s->tb_refresh_rate;

	mutex_unlock(&s->lock);
	timer_sleep_until(sleep_until + 1);
	mutex_lock(&s->lock);
	tb_refill_token(s);
}

static ssize_t crpc_send_request_vector(struct csd_session *s)
{
	struct csd_hdr chdr[CRPC_QLEN];
	struct iovec v[CRPC_QLEN * 2];
	int nriov = 0;
	int nrhdr = 0;
	ssize_t ret;
	uint64_t now = microtime();

	assert_mutex_held(&s->lock);

	if (s->head == s->tail || s->tb_token < 1.0)
		return 0;

	while (s->head != s->tail && s->tb_token >= 1.0) {
		struct crpc_ctx *c = s->qreq[s->tail++ % CRPC_QLEN];

		chdr[nrhdr].magic = SD_REQ_MAGIC;
		chdr[nrhdr].op = SD_OP_CALL;
		chdr[nrhdr].id = c->id;
		chdr[nrhdr].len = c->len;
		chdr[nrhdr].ts = now;

		v[nriov].iov_base = &chdr[nrhdr];
		v[nriov].iov_len = sizeof(struct csd_hdr);
		nrhdr++;
		nriov++;

		if (c->len > 0) {
			v[nriov].iov_base = c->buf;
			v[nriov++].iov_len = c->len;
		}

		s->tb_token -= 1.0;
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

static ssize_t crpc_send_raw(struct csd_session *s,
			     const void *buf, size_t len,
			     uint64_t id)
{
	struct iovec vec[2];
	struct csd_hdr chdr;
	ssize_t ret;

	/* header */
	chdr.magic = SD_REQ_MAGIC;
	chdr.op = SD_OP_CALL;
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

static bool crpc_enqueue_one(struct csd_session *s,
			     const void *buf, size_t len)
{
	int pos;
	struct crpc_ctx *c;
	uint64_t now = microtime();

	assert_mutex_held(&s->lock);

	if (s->head - s->tail >= CRPC_QLEN) {
		//printf("[WARNING] Request dropped due to full queue\n");
		return false;
	}

	pos = s->head++ % CRPC_QLEN;
	c = s->qreq[pos];
	memcpy(c->buf, buf, len);
	c->id = s->req_id++;
	c->ts = now;
	c->len = len;

	if (s->head - s->tail == 1) {
		condvar_signal(&s->timer_cv);
		condvar_signal(&s->sender_cv);
	}

	return true;
}

ssize_t csd_send_one(struct crpc_session *s_,
		      const void *buf, size_t len, int hash)
{
	struct csd_session *s = (struct csd_session *)s_;
	ssize_t ret;

	/* implementation is currently limited to a maximum payload size */
	if (unlikely(len > SRPC_BUF_SIZE))
		return -E2BIG;

	mutex_lock(&s->lock);

	tb_refill_token(s);
	/* hot path, just send */
	if (s->head == s->tail && s->tb_token >= 1.0) {
		s->tb_token -= 1.0;
		ret = crpc_send_raw(s, buf, len, s->req_id++);
		mutex_unlock(&s->lock);
		return ret;
	}

	/* cold path, enqueue request and drain the queue */
	crpc_enqueue_one(s, buf, len);
	mutex_unlock(&s->lock);

	return len;
}

static int cmpfunc(const void *a, const void *b) {
	return (*(uint32_t*)a - *(uint32_t*)b);
}

static void crpc_update_tb_rate(struct csd_session *s, uint64_t us)
{
	double rate = s->tb_refresh_rate;
	uint32_t samp;
	double err;
	uint64_t now = microtime();
	int idx;
	int len;

	assert_mutex_held(&s->lock);

	s->res_ts[s->res_idx++ % SEDA_NREQ] = (uint32_t)us;

	if (now - s->seda_last_update > SEDA_TIMEOUT) {
		len = s->res_idx % SEDA_NREQ;
		qsort(s->res_ts, len, sizeof(uint32_t), cmpfunc);
		idx = (int)((len - 1) * 0.99);
		samp = s->res_ts[idx];
		s->cur = SEDA_ALPHA * s->cur + (1 - SEDA_ALPHA) * samp;

		err = (s->cur - SEDA_TARGET) / (double)SEDA_TARGET;

		if (err > SEDA_ERR_D)
			rate = rate / SEDA_ADJ_D;
		else if (err < SEDA_ERR_I)
			rate += -(err - SEDA_CI) * SEDA_ADJ_I;

		tb_set_rate(s, rate);
		s->seda_last_update = microtime();
		s->res_idx = 0;
		return;
	}

	if (s->res_idx % SEDA_NREQ > 0)
		return;

	// sort res_ts;
	qsort(s->res_ts, SEDA_NREQ, sizeof(uint32_t), cmpfunc);
	samp = s->res_ts[(int)(SEDA_NREQ * 0.99)];
	s->cur = SEDA_ALPHA * s->cur + (1 - SEDA_ALPHA) * samp;

	err = (s->cur - SEDA_TARGET) / (double)SEDA_TARGET;

	if (err > SEDA_ERR_D) {
		rate = rate / SEDA_ADJ_D;
	} else if (err < SEDA_ERR_I) {
		rate += -(err - SEDA_CI) * SEDA_ADJ_I;
	}

	tb_set_rate(s, rate);
	s->seda_last_update = now;
}

ssize_t csd_recv_one(struct crpc_session *s_, void *buf, size_t len,
		     uint64_t *latency)
{
	struct csd_session *s = (struct csd_session *)s_;
	struct ssd_hdr shdr;
	ssize_t ret;
	uint64_t now;
	uint64_t us;

	/* read the server header */
	ret = tcp_read_full(s->cmn.c, &shdr, sizeof(shdr));
	if (unlikely(ret <= 0))
		return ret;
	assert(ret == sizeof(shdr));

	/* parse the server header */
	if (unlikely(shdr.magic != SD_RESP_MAGIC)) {
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
	case SD_OP_CALL:
		/* read the payload */
		if (shdr.len > 0) {
			ret = tcp_read_full(s->cmn.c, buf, shdr.len);
			if (unlikely(ret <= 0))
				return ret;
			assert(ret == shdr.len);
			s->resp_rx_++;
		}

		us = now - shdr.ts;

		mutex_lock(&s->lock);
		crpc_update_tb_rate(s, us);
		mutex_unlock(&s->lock);

#if CSD_TRACK_FLOW
		if (s->id == CSD_TRACK_FLOW_ID) {
			printf("[%lu] ===> response: id=%lu\n",
			       now, shdr.id, shdr.win);
		}
#endif

		break;
	case SD_OP_WINUPDATE:
		printf("Oops!\n");
		break;
	default:
		log_warn("crpc: got invalid op %d", shdr.op);
		return -EINVAL;
	}

	return shdr.len;
}

static void crpc_timer(void *arg)
{
	struct csd_session *s = (struct csd_session *)arg;
	uint64_t now;
	int pos;
	struct crpc_ctx *c;

	mutex_lock(&s->lock);
	while(true) {
		while (s->running && s->head == s->tail)
			condvar_wait(&s->timer_cv, &s->lock);

		if (!s->running)
			goto done;

		now = microtime();

		while (s->head != s->tail) {
			pos = s->tail % CRPC_QLEN;
			c = s->qreq[pos];
			if (now - c->ts <= CSD_MAX_CLIENT_DELAY_US)
				break;

			s->tail++;
			s->req_dropped_++;
		}

		if (s->head == s->tail) {
			s->head = 0;
			s->tail = 0;
			continue;
		}

		// calculate next wake up time
		pos = (s->head - 1) % CRPC_QLEN;
		c = s->qreq[pos];
		mutex_unlock(&s->lock);
		timer_sleep_until(c->ts + CSD_MAX_CLIENT_DELAY_US);
		mutex_lock(&s->lock);
	}
done:
	mutex_unlock(&s->lock);
	waitgroup_done(&s->timer_waiter);
}

static void crpc_sender(void *arg)
{
	struct csd_session *s = (struct csd_session *)arg;
	int pos;
	struct crpc_ctx *c;
	uint64_t now;

	mutex_lock(&s->lock);
	while (true) {
		while(s->running && s->head == s->tail)
			condvar_wait(&s->sender_cv, &s->lock);

		if (!s->running)
			goto done;

		while (s->tb_token < 1.0)
			tb_sleep_until_next_token(s);

		now = microtime();
		while (s->head != s->tail) {
			pos = s->tail % CRPC_QLEN;
			c = s->qreq[pos];
			if (now - c->ts <= CSD_MAX_CLIENT_DELAY_US)
				break;

			s->tail++;
			s->req_dropped_++;
		}

		crpc_send_request_vector(s);
	}

done:
	mutex_unlock(&s->lock);
	waitgroup_done(&s->sender_waiter);
}

int csd_open(struct netaddr raddr, struct crpc_session **sout, int id)
{
	struct netaddr laddr;
	struct csd_session *s;
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
	s->tb_token = 0.0;
	s->tb_refresh_rate = (rand() / (double)RAND_MAX) *
		(CSD_TB_INIT_RATE - CSD_TB_MIN_RATE) + CSD_TB_MIN_RATE;
	s->tb_last_refresh = 0;
	s->res_idx = 0;

	mutex_init(&s->lock);
	condvar_init(&s->timer_cv);
	condvar_init(&s->sender_cv);
	waitgroup_init(&s->timer_waiter);
	waitgroup_add(&s->timer_waiter, 1);
	waitgroup_init(&s->sender_waiter);
	waitgroup_add(&s->sender_waiter, 1);
	s->running = true;

	if (id != -1)
		s->id = id;
	s->req_id = 1;
	*sout = (struct crpc_session *)s;

	ret = thread_spawn(crpc_timer, s);
	BUG_ON(ret);

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

void csd_close(struct crpc_session *s_)
{
	struct csd_session *s = (struct csd_session *)s_;
	int i;

	mutex_lock(&s->lock);
	s->running = false;
	condvar_signal(&s->timer_cv);
	condvar_signal(&s->sender_cv);
	mutex_unlock(&s->lock);

	waitgroup_wait(&s->timer_waiter);
	waitgroup_wait(&s->sender_waiter);

	tcp_close(s->cmn.c);
	for (i = 0; i < CRPC_QLEN; ++i)
		sfree(s->qreq[i]);
	sfree(s);
}

/* client-side stats */
uint32_t csd_win_avail(struct crpc_session *s_)
{
	return 0;
}

void csd_stat_clear(struct crpc_session *s_)
{
	return;
}

uint64_t csd_stat_win_expired(struct crpc_session *s_)
{
	struct csd_session *s = (struct csd_session *)s_;
	return s->win_expired_;
}

uint64_t csd_stat_winu_rx(struct crpc_session *s_)
{
	struct csd_session *s = (struct csd_session *)s_;
	return s->winu_rx_;
}

uint64_t csd_stat_winu_tx(struct crpc_session *s_)
{
	struct csd_session *s = (struct csd_session *)s_;
	return s->winu_tx_;
}

uint64_t csd_stat_resp_rx(struct crpc_session *s_)
{
	struct csd_session *s = (struct csd_session *)s_;
	return s->resp_rx_;
}

uint64_t csd_stat_req_tx(struct crpc_session *s_)
{
	struct csd_session *s = (struct csd_session *)s_;
	return s->req_tx_;
}

uint64_t csd_stat_req_dropped(struct crpc_session *s_)
{
	struct csd_session *s = (struct csd_session *)s_;
	return s->req_dropped_;
}

struct crpc_ops csd_ops = {
	.crpc_send_one		= csd_send_one,
	.crpc_recv_one		= csd_recv_one,
	.crpc_open		= csd_open,
	.crpc_close		= csd_close,
	.crpc_win_avail		= csd_win_avail,
	.crpc_stat_clear	= csd_stat_clear,
	.crpc_stat_winu_rx	= csd_stat_winu_rx,
	.crpc_stat_win_expired	= csd_stat_win_expired,
	.crpc_stat_winu_tx	= csd_stat_winu_tx,
	.crpc_stat_resp_rx	= csd_stat_resp_rx,
	.crpc_stat_req_tx	= csd_stat_req_tx,
	.crpc_stat_req_dropped	= csd_stat_req_dropped,
};
