/*
 * RPC server-side support
 */

#include <stdio.h>

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

#include <breakwater/nocontrol.h>

#include "util.h"
#include "nc_proto.h"
#include "nc_config.h"

/* the maximum supported window size */
#define SNC_MAX_WINDOW_EXP	6
#define SNC_MAX_WINDOW		64

#define SNC_TRACK_FLOW		false
#define SNC_TRACK_FLOW_ID	1

#define SNC_TS_OUT		false
#define TS_BUF_SIZE_EXP		15
#define TS_BUF_SIZE		(1 << TS_BUF_SIZE_EXP)
#define TS_BUF_MASK		(TS_BUF_SIZE - 1)

#define EWMA_WEIGHT		0.1f

BUILD_ASSERT((1 << SNC_MAX_WINDOW_EXP) == SNC_MAX_WINDOW);

#if SNC_TS_OUT
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

struct snc_session {
	struct srpc_session	cmn;
	int			id;
	/* drained_list's core number. -1 if not in the drained list */
	waitgroup_t		send_waiter;
	int			num_pending;

	/* shared state between receiver and sender */
	DEFINE_BITMAP(avail_slots, SNC_MAX_WINDOW);

	/* shared state between workers and sender */
	spinlock_t		lock;
	int			closed;
	thread_t		*sender_th;
	DEFINE_BITMAP(completed_slots, SNC_MAX_WINDOW);

	/* worker slots (one for each credit issued) */
	struct snc_ctx		*slots[SNC_MAX_WINDOW];
};

/* credit-related stats */
atomic64_t srpc_stat_winu_rx_;
atomic64_t srpc_stat_winu_tx_;
atomic64_t srpc_stat_win_tx_;
atomic64_t srpc_stat_req_rx_;
atomic64_t srpc_stat_req_dropped_;
atomic64_t srpc_stat_resp_tx_;

#if SNC_TS_OUT
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

static int srpc_get_slot(struct snc_session *s)
{
	int slot = __builtin_ffsl(s->avail_slots[0]) - 1;
	if (slot >= 0) {
		bitmap_atomic_clear(s->avail_slots, slot);
		s->slots[slot] = smalloc(sizeof(struct snc_ctx));
		s->slots[slot]->cmn.s = (struct srpc_session *)s;
		s->slots[slot]->cmn.idx = slot;
	}
	return slot;
}

static void srpc_put_slot(struct snc_session *s, int slot)
{
	sfree(s->slots[slot]);
	s->slots[slot] = NULL;
	bitmap_atomic_set(s->avail_slots, slot);
}

static int srpc_send_completion_vector(struct snc_session *s,
				       unsigned long *slots)
{
	struct snc_hdr shdr[SNC_MAX_WINDOW];
	struct iovec v[SNC_MAX_WINDOW * 2];
	int nriov = 0;
	int nrhdr = 0;
	int i;
	ssize_t ret = 0;

	bitmap_for_each_set(slots, SNC_MAX_WINDOW, i) {
		struct snc_ctx *c = s->slots[i];

		shdr[nrhdr].magic = NC_RESP_MAGIC;
		shdr[nrhdr].op = NC_OP_CALL;
		shdr[nrhdr].len = c->cmn.resp_len;
		shdr[nrhdr].id = c->cmn.id;
		shdr[nrhdr].ts = c->ts;

		v[nriov].iov_base = &shdr[nrhdr];
		v[nriov].iov_len = sizeof(struct snc_hdr);
		nrhdr++;
		nriov++;

		if (c->cmn.resp_len > 0) {
			v[nriov].iov_base = c->cmn.resp_buf;
			v[nriov++].iov_len = c->cmn.resp_len;
		}
	}

	/* send the completion(s) */
	if (nriov == 0)
		return 0;
	ret = tcp_writev_full(s->cmn.c, v, nriov);
	bitmap_for_each_set(slots, SNC_MAX_WINDOW, i)
		srpc_put_slot(s, i);

#if SNC_TRACK_FLOW
	if (s->id == SNC_TRACK_FLOW_ID) {
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
	struct snc_ctx *c = (struct snc_ctx *)arg;
	struct snc_session *s = (struct snc_session *)c->cmn.s;
	uint64_t st = microtime();
	thread_t *th;

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

static int srpc_recv_one(struct snc_session *s)
{
	struct cnc_hdr chdr;
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
	if (unlikely(chdr.magic != NC_REQ_MAGIC)) {
		log_warn("srpc: got invalid magic %x", chdr.magic);
		return -EINVAL;
	}
	if (unlikely(chdr.len > SRPC_BUF_SIZE)) {
		log_warn("srpc: request len %ld too large (limit %d)",
			 chdr.len, SRPC_BUF_SIZE);
		return -EINVAL;
	}

	switch (chdr.op) {
	case NC_OP_CALL:
		atomic64_inc(&srpc_stat_req_rx_);
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
		s->slots[idx]->ts = chdr.ts;

		spin_lock_np(&s->lock);
		s->num_pending++;

#if SNC_AQM_ON
		if (runtime_queue_us() >= SNC_AQM_THRESH) {
			thread_t *th;

			bitmap_set(s->completed_slots, idx);
			th = s->sender_th;
			s->sender_th = NULL;
			spin_unlock_np(&s->lock);
			if (th)
				thread_ready(th);
			atomic64_inc(&srpc_stat_req_dropped_);
			goto again;
		}
#endif

		spin_unlock_np(&s->lock);

		atomic_inc(&srpc_num_pending);
		ret = thread_spawn(srpc_worker, s->slots[idx]);
		BUG_ON(ret);

#if SND_TRACK_FLOW
		uint64_t now = microtime();
		if (s->id == SNC_TRACK_FLOW_ID) {
			printf("[%lu] ===> Request: id=%lu, demand=%lu\n",
			       now, chdr.id, chdr.demand);
		}
#endif
		break;
	case NC_OP_WINUPDATE:
		printf("Invalid Op\n");
		break;
	default:
		printf("Invalid Op\n");;
		log_warn("srpc: got invalid op %d", chdr.op);
		return -EINVAL;
	}

	return ret;
}

static void srpc_sender(void *arg)
{
	DEFINE_BITMAP(tmp, SNC_MAX_WINDOW);
	struct snc_session *s = (struct snc_session *)arg;
	int i;
	bool sleep;
	int num_resp;

	while (true) {
		/* find slots that have completed */
		spin_lock_np(&s->lock);
		while (true) {
			sleep = !s->closed &&
				bitmap_popcount(s->completed_slots,
						SNC_MAX_WINDOW) == 0;
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
		bitmap_init(s->completed_slots, SNC_MAX_WINDOW, false);

		num_resp = bitmap_popcount(tmp, SNC_MAX_WINDOW);
		s->num_pending -= num_resp;

		spin_unlock_np(&s->lock);

		/* send a response for each completed slot */
		srpc_send_completion_vector(s, tmp);
	}

	/* wait for in-flight completions to finish */
	spin_lock_np(&s->lock);
	while (!s->closed ||
	       bitmap_popcount(s->avail_slots, SNC_MAX_WINDOW) +
	       bitmap_popcount(s->completed_slots, SNC_MAX_WINDOW) <
	       SNC_MAX_WINDOW) {
		s->sender_th = thread_self();
		thread_park_and_unlock_np(&s->lock);
		spin_lock_np(&s->lock);
		s->sender_th = NULL;
	}

	spin_unlock_np(&s->lock);

	/* free any left over slots */
	for (i = 0; i < SNC_MAX_WINDOW; i++) {
		if (s->slots[i])
			srpc_put_slot(s, i);
	}

	/* notify server thread that the sender is done */
	waitgroup_done(&s->send_waiter);
}

static void srpc_server(void *arg)
{
	tcpconn_t *c = (tcpconn_t *)arg;
	struct snc_session *s;
	thread_t *th;
	int ret;

	s = smalloc(sizeof(*s));
	BUG_ON(!s);
	memset(s, 0, sizeof(*s));

	s->cmn.c = c;
	s->id = atomic_fetch_and_add(&srpc_num_sess, 1) + 1;
	bitmap_init(s->avail_slots, SNC_MAX_WINDOW, true);

	waitgroup_init(&s->send_waiter);
	waitgroup_add(&s->send_waiter, 1);

#if SNC_TRACK_FLOW
	if (s->id == SNC_TRACK_FLOW_ID) {
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

static void srpc_listener(void *arg)
{
	struct netaddr laddr;
	tcpconn_t *c;
	tcpqueue_t *q;
	int ret;

	atomic_write(&srpc_num_sess, 0);

	atomic_write(&srpc_num_pending, 0);
	srpc_avg_st = 0;

	/* init stats */
	atomic64_write(&srpc_stat_winu_rx_, 0);
	atomic64_write(&srpc_stat_winu_tx_, 0);
	atomic64_write(&srpc_stat_req_rx_, 0);
	atomic64_write(&srpc_stat_resp_tx_, 0);

	laddr.ip = 0;
	laddr.port = SRPC_PORT;

	ret = tcp_listen(laddr, 4096, &q);
	BUG_ON(ret);

	while (true) {
		ret = tcp_accept(q, &c);
		if (WARN_ON(ret))
			continue;
		ret = thread_spawn(srpc_server, c);
		WARN_ON(ret);
	}
}

/**
 * srpc_enable - starts the RPC server
 * @handler: the handler function to call for each RPC.
 *
 * Returns 0 if successful.
 */
int snc_enable(srpc_fn_t handler)
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

uint64_t snc_stat_winu_rx()
{
	return atomic64_read(&srpc_stat_winu_rx_);
}

uint64_t snc_stat_winu_tx()
{
	return atomic64_read(&srpc_stat_winu_tx_);
}

uint64_t snc_stat_win_tx()
{
	return atomic64_read(&srpc_stat_win_tx_);
}

uint64_t snc_stat_req_rx()
{
	return atomic64_read(&srpc_stat_req_rx_);
}

uint64_t snc_stat_req_dropped()
{
	return atomic64_read(&srpc_stat_req_dropped_);
}

uint64_t snc_stat_resp_tx()
{
	return atomic64_read(&srpc_stat_resp_tx_);
}

struct srpc_ops snc_ops = {
	.srpc_enable		= snc_enable,
	.srpc_stat_winu_rx	= snc_stat_winu_rx,
	.srpc_stat_winu_tx	= snc_stat_winu_tx,
	.srpc_stat_win_tx	= snc_stat_win_tx,
	.srpc_stat_req_rx	= snc_stat_req_rx,
	.srpc_stat_req_dropped	= snc_stat_req_dropped,
	.srpc_stat_resp_tx	= snc_stat_resp_tx,
};
