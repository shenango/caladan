/*
 * RPC server-side support
 */

#include <stdio.h>
#include <time.h>

#include <base/atomic.h>
#include <base/stddef.h>
#include <base/time.h>
#include <base/list.h>
#include <base/log.h>
#include <runtime/tcp.h>
#include <runtime/sync.h>
#include <runtime/smalloc.h>
#include <runtime/thread.h>
#include <runtime/timer.h>
#include <runtime/runtime.h>

#include <breakwater/dagor.h>

#include "util.h"
#include "dg_proto.h"
#include "dg_config.h"

/* the maximum supported window size */
#define SDG_MAX_WINDOW_EXP	6
#define SDG_MAX_WINDOW		64

#define SDG_TRACK_FLOW		false
#define SDG_TRACK_FLOW_ID	1

#define SDG_TS_OUT		false
#define TS_BUF_SIZE_EXP		15
#define TS_BUF_SIZE		(1 << TS_BUF_SIZE_EXP)
#define TS_BUF_MASK		(TS_BUF_SIZE - 1)

#define EWMA_WEIGHT		0.1f

BUILD_ASSERT((1 << SDG_MAX_WINDOW_EXP) == SDG_MAX_WINDOW);

#if SDG_TS_OUT
int nextIndex = 0;
FILE *signal_out = NULL;

struct Event {
	uint64_t timestamp;
	int win_avail;
	int win_used;
	int num_pending;
	int num_drained;
	int num_active;
	int num_sess;
	uint64_t delay;
	int num_cores;
};

static struct Event events[TS_BUF_SIZE];
#endif

/* the handler function for each RPC */
static srpc_fn_t srpc_handler;

/* total number of session */
atomic_t srpc_num_sess;

/* the number of active sessions */
atomic_t srpc_num_active;

/* the number of pending requests */
atomic_t srpc_num_pending;

/* average service time in us */
int srpc_avg_st;

/* dagor-related variables */
uint64_t last_prio_update;
double dagor_prio_;
int dagor_delay;
atomic_t dagor_prio_thresh;
atomic_t dagor_num_reqs;

struct sdg_session {
	struct srpc_session	cmn;
	int			id;
	/* drained_list's core number. -1 if not in the drained list */
	waitgroup_t		send_waiter;
	int			num_pending;

	/* shared state between receiver and sender */
	DEFINE_BITMAP(avail_slots, SDG_MAX_WINDOW);

	/* shared state between workers and sender */
	spinlock_t		lock;
	int			closed;
	thread_t		*sender_th;
	DEFINE_BITMAP(completed_slots, SDG_MAX_WINDOW);

	/* worker slots (one for each credit issued) */
	struct sdg_ctx		*slots[SDG_MAX_WINDOW];
};

/* credit-related stats */
atomic64_t srpc_stat_winu_rx_;
atomic64_t srpc_stat_winu_tx_;
atomic64_t srpc_stat_win_tx_;
atomic64_t srpc_stat_req_rx_;
atomic64_t srpc_stat_req_dropped_;
atomic64_t srpc_stat_resp_tx_;

#if SDG_TS_OUT
static void printRecord()
{
	int i;

	if (!signal_out)
		signal_out = fopen("signal.csv", "w");

	for (i = 0; i < TS_BUF_SIZE; ++i) {
		struct Event *event = &events[i];
		fprintf(signal_out, "%lu,%d,%d,%d,%d,%d,%d,%lu,%d\n",
			event->timestamp, event->win_avail,
			event->win_used, event->num_pending,
			event->num_drained, event->num_active,
			event->num_sess, event->delay,
			event->num_cores);
	}
	fflush(signal_out);
}

static void record(int win_avail, uint64_t delay)
{
	struct Event *event = &events[nextIndex];
	nextIndex = (nextIndex + 1) & TS_BUF_MASK;

	event->timestamp = microtime();
	event->win_avail = win_avail;
	event->win_used = 0;
	event->num_pending = atomic_read(&srpc_num_pending);
	event->num_drained = 0;
	event->num_active = atomic_read(&srpc_num_active);
	event->num_sess = atomic_read(&srpc_num_sess);
	event->delay = delay;
	event->num_cores = runtime_active_cores();

	if (nextIndex == 0)
		printRecord();
}
#endif

static int srpc_get_slot(struct sdg_session *s)
{
	int slot = __builtin_ffsl(s->avail_slots[0]) - 1;
	if (slot >= 0) {
		bitmap_atomic_clear(s->avail_slots, slot);
		s->slots[slot] = smalloc(sizeof(struct sdg_ctx));
		s->slots[slot]->cmn.s = (struct srpc_session *)s;
		s->slots[slot]->cmn.idx = slot;
	}
	return slot;
}

static void srpc_put_slot(struct sdg_session *s, int slot)
{
	sfree(s->slots[slot]);
	s->slots[slot] = NULL;
	bitmap_atomic_set(s->avail_slots, slot);
}

static int srpc_send_completion_vector(struct sdg_session *s,
				       unsigned long *slots)
{
	struct sdg_hdr shdr[SDG_MAX_WINDOW];
	struct iovec v[SDG_MAX_WINDOW * 2];
	int nriov = 0;
	int nrhdr = 0;
	int i;
	ssize_t ret = 0;

	bitmap_for_each_set(slots, SDG_MAX_WINDOW, i) {
		struct sdg_ctx *c = s->slots[i];
		size_t len;
		char *buf;
		uint8_t flags = 0;

		if (!c->drop) {
			len = c->cmn.resp_len;
			buf = c->cmn.resp_buf;
		} else {
			len = c->cmn.req_len;
			buf = c->cmn.req_buf;
			flags |= DG_SFLAG_DROP;
		}

		shdr[nrhdr].magic = DG_RESP_MAGIC;
		shdr[nrhdr].op = DG_OP_CALL;
		shdr[nrhdr].len = len;
		shdr[nrhdr].id = c->cmn.id;
		shdr[nrhdr].prio = atomic_read(&dagor_prio_thresh);
		shdr[nrhdr].ts_sent = c->ts_sent;
		shdr[nrhdr].flags = flags;

		v[nriov].iov_base = &shdr[nrhdr];
		v[nriov].iov_len = sizeof(struct sdg_hdr);
		nrhdr++;
		nriov++;

		if (len > 0) {
			v[nriov].iov_base = buf;
			v[nriov++].iov_len = len;
		}
	}

	/* send the completion(s) */
	if (nriov == 0)
		return 0;
	ret = tcp_writev_full(s->cmn.c, v, nriov);
	bitmap_for_each_set(slots, SDG_MAX_WINDOW, i)
		srpc_put_slot(s, i);

#if SDG_TRACK_FLOW
	if (s->id == SDG_TRACK_FLOW_ID) {
		printf("[%lu] <=== Response (%d)\n",
			microtime(), nrhdr);
	}
#endif
	atomic_sub_and_fetch(&srpc_num_pending, nrhdr);
	atomic64_fetch_and_add(&srpc_stat_resp_tx_, nrhdr);
	atomic64_fetch_and_add(&srpc_stat_win_tx_, 0);

	if (unlikely(ret < 0))
		return ret;
	return 0;
}

static void srpc_worker(void *arg)
{
	struct sdg_ctx *c = (struct sdg_ctx *)arg;
	struct sdg_session *s = (struct sdg_session *)c->cmn.s;
	uint64_t st = microtime();
	thread_t *th;

	c->drop = false;
	srpc_handler((struct srpc_ctx *)c);
	st = microtime() - st;

	srpc_avg_st = (int)((1 - EWMA_WEIGHT) * srpc_avg_st + EWMA_WEIGHT * st);

	spin_lock_np(&s->lock);
	bitmap_set(s->completed_slots, c->cmn.idx);
	th = s->sender_th;
	s->sender_th = NULL;
	spin_unlock_np(&s->lock);
	if (th)
		thread_ready(th);
}

static int srpc_recv_one(struct sdg_session *s)
{
	struct cdg_hdr chdr;
	int idx, ret;
	char buf_tmp[SRPC_BUF_SIZE];

again:
	/* read the client header */
	ret = tcp_read_full(s->cmn.c, &chdr, sizeof(chdr));
	if (unlikely(ret <= 0)) {
		if (ret == 0)
			return -EIO;
		return ret;
	}

	/* parse the client header */
	if (unlikely(chdr.magic != DG_REQ_MAGIC)) {
		log_warn("srpc: got invalid magic %x", chdr.magic);
		return -EINVAL;
	}
	if (unlikely(chdr.len > SRPC_BUF_SIZE)) {
		log_warn("srpc: request len %ld too large (limit %d)",
			 chdr.len, SRPC_BUF_SIZE);
		return -EINVAL;
	}

	switch (chdr.op) {
	case DG_OP_CALL:
		atomic64_inc(&srpc_stat_req_rx_);
		atomic_inc(&dagor_num_reqs);

		/* reserve a slot */
		idx = srpc_get_slot(s);
		if (idx < 0) {
			ret = tcp_read_full(s->cmn.c, buf_tmp, chdr.len);
			atomic64_inc(&srpc_stat_req_dropped_);
			goto again;
		}

		/* retrieve the payload */
		ret = tcp_read_full(s->cmn.c, s->slots[idx]->cmn.req_buf, chdr.len);
		if (unlikely(ret <= 0)) {
			srpc_put_slot(s, idx);
			if (ret == 0)
				return -EIO;
			return ret;
		}

		s->slots[idx]->cmn.req_len = chdr.len;
		s->slots[idx]->cmn.resp_len = 0;
		s->slots[idx]->cmn.id = chdr.id;
		s->slots[idx]->ts_sent = chdr.ts_sent;

		if (chdr.prio > atomic_read(&dagor_prio_thresh)) {
			thread_t *th;

			s->slots[idx]->drop = true;
			spin_lock_np(&s->lock);
			bitmap_set(s->completed_slots, idx);
			th = s->sender_th;
			s->sender_th = NULL;
			spin_unlock_np(&s->lock);
			if (th)
				thread_ready(th);
			atomic64_inc(&srpc_stat_req_dropped_);
			return 0;
		}

		spin_lock_np(&s->lock);
		s->num_pending++;
		spin_unlock_np(&s->lock);

		atomic_inc(&srpc_num_pending);
		ret = thread_spawn(srpc_worker, s->slots[idx]);
		BUG_ON(ret);

#if SDG_TRACK_FLOW
		uint64_t now = microtime();
		if (s->id == SDG_TRACK_FLOW_ID) {
			printf("[%lu] ===> Request: id=%lu, prio=%lu\n",
			       now, chdr.id, chdr.prio);
		}
#endif
		break;
	case DG_OP_WINUPDATE:
		printf("Oops\n");
		break;
	default:
		log_warn("srpc: got invalid op %d", chdr.op);
		return -EINVAL;
	}

	return ret;
}

static void srpc_sender(void *arg)
{
	DEFINE_BITMAP(tmp, SDG_MAX_WINDOW);
	struct sdg_session *s = (struct sdg_session *)arg;
	int i;
	bool sleep;
	int num_resp;

	while (true) {
		/* find slots that have completed */
		spin_lock_np(&s->lock);
		while (true) {
			sleep = !s->closed &&
				bitmap_popcount(s->completed_slots,
						SDG_MAX_WINDOW) == 0;
			if (!sleep) {
				s->sender_th = NULL;
				break;
			}
			s->sender_th = thread_self();
			thread_park_and_unlock_np(&s->lock);
			spin_lock_np(&s->lock);
		}
		if (unlikely(s->closed)) {
			spin_unlock_np(&s->lock);
			break;
		}
		memcpy(tmp, s->completed_slots, sizeof(tmp));
		bitmap_init(s->completed_slots, SDG_MAX_WINDOW, false);

		num_resp = bitmap_popcount(tmp, SDG_MAX_WINDOW);
		s->num_pending -= num_resp;

		spin_unlock_np(&s->lock);

		/* send a response for each completed slot */
		srpc_send_completion_vector(s, tmp);
	}

	/* wait for in-flight completions to finish */
	spin_lock_np(&s->lock);
	while (!s->closed ||
	       bitmap_popcount(s->avail_slots, SDG_MAX_WINDOW) +
	       bitmap_popcount(s->completed_slots, SDG_MAX_WINDOW) <
	       SDG_MAX_WINDOW) {
		s->sender_th = thread_self();
		thread_park_and_unlock_np(&s->lock);
		spin_lock_np(&s->lock);
		s->sender_th = NULL;
	}

	spin_unlock_np(&s->lock);

	/* free any left over slots */
	for (i = 0; i < SDG_MAX_WINDOW; i++) {
		if (s->slots[i])
			srpc_put_slot(s, i);
	}

	/* notify server thread that the sender is done */
	waitgroup_done(&s->send_waiter);
}

static void srpc_server(void *arg)
{
	tcpconn_t *c = (tcpconn_t *)arg;
	struct sdg_session *s;
	thread_t *th;
	int ret;

	s = smalloc(sizeof(*s));
	BUG_ON(!s);
	memset(s, 0, sizeof(*s));

	s->cmn.c = c;
	s->id = atomic_fetch_and_add(&srpc_num_sess, 1) + 1;
	bitmap_init(s->avail_slots, SDG_MAX_WINDOW, true);

	waitgroup_init(&s->send_waiter);
	waitgroup_add(&s->send_waiter, 1);

#if SDG_TRACK_FLOW
	if (s->id == SDG_TRACK_FLOW_ID) {
		printf("[%lu] connection established.\n",
		       microtime());
	}
#endif

	ret = thread_spawn(srpc_sender, s);
	BUG_ON(ret);

	while (true) {
		ret = srpc_recv_one(s);
		if (ret)
			break;
	}

	spin_lock_np(&s->lock);
	th = s->sender_th;
	s->sender_th = NULL;
	s->closed = true;
	atomic_sub_and_fetch(&srpc_num_pending, s->num_pending);
	s->num_pending = 0;
	spin_unlock_np(&s->lock);

	if (th)
		thread_ready(th);

	atomic_dec(&srpc_num_sess);
	waitgroup_wait(&s->send_waiter);
	tcp_close(c);
	sfree(s);
}

static void dagor_prio_update(void *arg)
{
	uint64_t now, us;
	int nreqs;

	while (true) {
		timer_sleep(DAGOR_PRIO_MONITOR);

		now = microtime();
		nreqs = atomic_read(&dagor_num_reqs);

		us = runtime_queue_us();
		dagor_delay = (int)(0.8 * dagor_delay + 0.2 * us);

		if (nreqs == 0 ||
		    (microtime() - last_prio_update < DAGOR_PRIO_UPDATE_INT &&
		    nreqs < DAGOR_PRIO_UPDATE_REQS))
			continue;

		if (dagor_delay >= DAGOR_OVERLOAD_THRESH)
			dagor_prio_ = DAGOR_ALPHA * dagor_prio_;
		else
			dagor_prio_ = dagor_prio_ + DAGOR_BETA * DG_MAX_PRIO;

		dagor_prio_ = MAX(dagor_prio_, 1.0);
		dagor_prio_ = MIN(dagor_prio_, DG_MAX_PRIO - 1);

		last_prio_update = now;

		atomic_write(&dagor_prio_thresh, (int)dagor_prio_);
		atomic_write(&dagor_num_reqs, 0);
	}
}

static void srpc_listener(void *arg)
{
	struct netaddr laddr;
	tcpconn_t *c;
	tcpqueue_t *q;
	int ret;

	dagor_prio_ = 32.0;
	atomic_write(&dagor_prio_thresh, 32);

	atomic_write(&srpc_num_sess, 0);
	atomic_write(&srpc_num_pending, 0);
	srpc_avg_st = 0;

	/* init stats */
	atomic64_write(&srpc_stat_winu_rx_, 0);
	atomic64_write(&srpc_stat_winu_tx_, 0);
	atomic64_write(&srpc_stat_req_rx_, 0);
	atomic64_write(&srpc_stat_resp_tx_, 0);

	atomic_write(&dagor_num_reqs, 0);

	laddr.ip = 0;
	laddr.port = SRPC_PORT;

	ret = tcp_listen(laddr, 4096, &q);
	BUG_ON(ret);

	last_prio_update = 0;
	ret = thread_spawn(dagor_prio_update, NULL);
	BUG_ON(ret);

	while (true) {
		ret = tcp_accept(q, &c);
		if (WARN_ON(ret))
			continue;
		ret = thread_spawn(srpc_server, c);
		WARN_ON(ret);
	}
}

int sdg_enable(srpc_fn_t handler)
{
	static DEFINE_SPINLOCK(l);
	int ret;

	spin_lock_np(&l);
	if (srpc_handler) {
		spin_unlock_np(&l);
		return -EBUSY;
	}
	srpc_handler = handler;
	spin_unlock_np(&l);

	ret = thread_spawn(srpc_listener, NULL);
	BUG_ON(ret);
	return 0;
}

uint64_t sdg_stat_winu_rx()
{
	return atomic64_read(&srpc_stat_winu_rx_);
}

uint64_t sdg_stat_winu_tx()
{
	return atomic64_read(&srpc_stat_winu_tx_);
}

uint64_t sdg_stat_win_tx()
{
	return atomic64_read(&srpc_stat_win_tx_);
}

uint64_t sdg_stat_req_rx()
{
	return atomic64_read(&srpc_stat_req_rx_);
}

uint64_t sdg_stat_req_dropped()
{
	return atomic64_read(&srpc_stat_req_dropped_);
}

uint64_t sdg_stat_resp_tx()
{
	return atomic64_read(&srpc_stat_resp_tx_);
}

struct srpc_ops sdg_ops = {
	.srpc_enable		= sdg_enable,
	.srpc_stat_winu_rx	= sdg_stat_winu_rx,
	.srpc_stat_winu_tx	= sdg_stat_winu_tx,
	.srpc_stat_win_tx	= sdg_stat_win_tx,
	.srpc_stat_req_rx	= sdg_stat_req_rx,
	.srpc_stat_req_dropped	= sdg_stat_req_dropped,
	.srpc_stat_resp_tx	= sdg_stat_resp_tx,
};
