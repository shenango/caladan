/*
 * RPC server-side support
 */

#include <base/stddef.h>
#include <base/list.h>
#include <base/log.h>
#include <runtime/rpc.h>
#include <runtime/tcp.h>
#include <runtime/smalloc.h>
#include <runtime/thread.h>
#include <runtime/runtime.h>

#include "util.h"
#include "proto.h"

/* the maximum supported window size */
#define SRPC_MAX_WINDOW		64
/* the queuing delay limit (for dropping threshold) */
#define SRPC_TARGET_DELAY_US	40
/* new tokens are created at this rate */
#define SRPC_TOKEN_RATE         1.2f
/* the maximum burst of tokens available to a client */
#define SRPC_TOKEN_LIMIT	((float)SRPC_MAX_WINDOW - 1.0f)

/* the handler function for each RPC */
static srpc_fn_t srpc_handler;
/* a list of sessions without tokens */
static LIST_HEAD(drained_sessions);
/* a lock protecting @drained_sessions */
static DEFINE_SPINLOCK(drained_lock);

struct srpc_session {
	tcpconn_t		*c;
	waitgroup_t		send_waiter;
	bool			drained;
	bool			undrain_pending;
	float			tokens_offered;
	float			tokens;
	struct list_node	link;

	/* shared state between receiver and sender */
	DEFINE_BITMAP(avail_slots, SRPC_MAX_WINDOW);

	/* shared state between workers and sender */
	spinlock_t		lock;
	bool			closed;
	thread_t		*sender_th;
	DEFINE_BITMAP(completed_slots, SRPC_MAX_WINDOW);

	/* worker slots (one for each credit issued) */
	struct srpc_ctx		*slots[SRPC_MAX_WINDOW];
};

/** server-side stats **/
/* # dropped requests */
atomic64_t srpc_stat_req_dropped_;
/* # rx requests */
atomic64_t srpc_stat_req_rx_;
/* # droppable rx requests */
atomic64_t srpc_stat_dreq_rx_;
/* # tx responses (completions) */
atomic64_t srpc_stat_resp_tx_;
/* # tx offers */
atomic64_t srpc_stat_offer_tx_;

static int srpc_get_slot(struct srpc_session *s)
{
	int slot = __builtin_ffsl(s->avail_slots[0]) - 1;
	if (slot >= 0) {
		bitmap_atomic_clear(s->avail_slots, slot);
		s->slots[slot] = smalloc(sizeof(struct srpc_ctx));
		s->slots[slot]->s = s;
		s->slots[slot]->idx = slot;
	}
	return slot;
}

static void srpc_put_slot(struct srpc_session *s, int slot)
{
	sfree(s->slots[slot]);
	s->slots[slot] = NULL;
	bitmap_atomic_set(s->avail_slots, slot);
}

static void srpc_worker(void *arg)
{
	struct srpc_ctx *c = (struct srpc_ctx *)arg;
	struct srpc_session *s = c->s;
	thread_t *th;

	srpc_handler(c);

	spin_lock_np(&s->lock);
	bitmap_set(s->completed_slots, c->idx);
	th = s->sender_th;
	s->sender_th = NULL;
	spin_unlock_np(&s->lock);
	if (th)
		thread_ready(th);
}

static int srpc_recv_one(struct srpc_session *s)
{
	struct crpc_hdr chdr;
	int idx, ret;

	/* read the client header */
	ret = tcp_read_full(s->c, &chdr, sizeof(chdr));
	if (unlikely(ret <= 0)) {
		if (ret == 0)
			return -EIO;
		return ret;
	}

	/* parse the client header */
	if (unlikely(chdr.magic != RPC_REQ_MAGIC)) {
		log_warn("srpc: got invalid magic %x", chdr.magic);
		return -EINVAL;
	}
	if (unlikely(chdr.len > SRPC_BUF_SIZE)) {
		log_warn("srpc: request len %ld too large (limit %d)",
			 chdr.len, SRPC_BUF_SIZE);
		return -EINVAL;
	}
	if (unlikely(chdr.op >= RPC_OP_MAX)) {
		log_warn("srpc: got invalid op %d", chdr.op);
		return -EINVAL;
	}

	/* reserve a slot */
	idx = srpc_get_slot(s);
	if (unlikely(idx < 0)) {
		log_warn("srpc: client tried to use more than %d slots",
			 SRPC_MAX_WINDOW);
		return -ENOENT;
	}

	/* retrieve the payload */
	ret = tcp_read_full(s->c, s->slots[idx]->req_buf, chdr.len);
	if (unlikely(ret <= 0)) {
		srpc_put_slot(s, idx);
		if (ret == 0)
			return -EIO;
		return ret;
	}

	atomic64_inc(&srpc_stat_req_rx_);
	atomic64_fetch_and_add(&srpc_stat_dreq_rx_,
			       (long)(chdr.op == RPC_OP_DROPCALL));

	/* should this request be dropped? */
	if (chdr.op == RPC_OP_DROPCALL &&
	    runtime_queue_us() >= SRPC_TARGET_DELAY_US) {
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

	s->slots[idx]->drop = false;
	s->slots[idx]->req_len = chdr.len;
	s->slots[idx]->resp_len = 0;
	ret = thread_spawn(srpc_worker, s->slots[idx]);
	BUG_ON(ret);
	return ret;
}

static int srpc_send_offer(struct srpc_session *s)
{
	struct srpc_hdr shdr;
	ssize_t ret;

	/* initialize the header */
	shdr.magic = RPC_RESP_MAGIC;
	shdr.op = RPC_OP_OFFER;
	shdr.len = 0;
	shdr.tokens = s->tokens;
	shdr.accepted = false;

	/* send the request */
	ret = tcp_write_full(s->c, &shdr, sizeof(shdr));
	if (unlikely(ret < 0))
		return ret;

	assert(ret == sizeof(shdr));
	return 0;
}

static int srpc_send_completion_vector(struct srpc_session *s,
				       unsigned long *slots)
{
	struct srpc_hdr shdr[SRPC_MAX_WINDOW];
	struct iovec v[SRPC_MAX_WINDOW * 2];
	struct srpc_session *ds;
	int nriov = 0, nrhdr = 0, i;
	ssize_t ret;

	bitmap_for_each_set(slots, SRPC_MAX_WINDOW, i) {
		struct srpc_ctx *c = s->slots[i];

		shdr[nrhdr].magic = RPC_RESP_MAGIC;
		shdr[nrhdr].op = RPC_OP_CALL;
		shdr[nrhdr].tokens = 0.0f;
		if (c->drop) {
			shdr[nrhdr].accepted = false;
			shdr[nrhdr].len = 0;
			s->tokens -= 1.0f;
		} else {
			shdr[nrhdr].accepted = true;
			shdr[nrhdr].len = c->resp_len;
			s->tokens += SRPC_TOKEN_RATE - 1.0f;
		};
		v[nriov].iov_base = &shdr[nrhdr];
		v[nriov].iov_len = sizeof(struct srpc_hdr);
		nrhdr++;
		nriov++;

		if (!c->drop && c->resp_len > 0) {
			v[nriov].iov_base = c->resp_buf;
			v[nriov++].iov_len = c->resp_len;
		}
	}

	atomic64_fetch_and_add(&srpc_stat_resp_tx_, nrhdr);

	/* don't undrain if it would cause this session to drain */
	if (nriov == 0)
		return 0;
	if (s->tokens < 2.0f)
		goto skip;

	/* try to find a drained session */
	spin_lock_np(&drained_lock);
	ds = list_pop(&drained_sessions, struct srpc_session, link);
	if (ds) {
		ds->drained = false;
		ds->undrain_pending = true;
	}
	spin_unlock_np(&drained_lock);

	/* wake up the drained session (if found) */
	if (ds) {
		thread_t *th;

		spin_lock_np(&ds->lock);
		ds->undrain_pending = false;
		ds->tokens_offered += 1.0f;
		th = ds->sender_th;
		ds->sender_th = NULL;
		spin_unlock_np(&ds->lock);
		if (th)
			thread_ready(th);
		s->tokens -= 1.0f;
	}

skip:
	/* send the completion(s) */
	if (s->tokens > SRPC_TOKEN_LIMIT)
		s->tokens = SRPC_TOKEN_LIMIT;
	for (i = 0; i < nrhdr; i++)
		shdr[i].tokens = s->tokens;
	ret = tcp_writev_full(s->c, v, nriov);
	bitmap_for_each_set(slots, SRPC_MAX_WINDOW, i)
		srpc_put_slot(s, i);
	if (unlikely(ret < 0))
		return ret;
        return 0;
}

static void srpc_sender(void *arg)
{
	DEFINE_BITMAP(tmp, SRPC_MAX_WINDOW);
	struct srpc_session *s = (struct srpc_session *)arg;
	float offered;
	int ret, i;
	bool sleep;

	while (true) {
		/* find slots that have completed */
		spin_lock_np(&s->lock);
		while (true) {
			sleep = !s->closed && s->tokens_offered == 0.0f &&
				bitmap_popcount(s->completed_slots,
						SRPC_MAX_WINDOW) == 0;
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
		bitmap_init(s->completed_slots, SRPC_MAX_WINDOW, false);
		offered = s->tokens_offered;
		s->tokens_offered = 0.0f;
		spin_unlock_np(&s->lock);

		/* send any pending token offers */
		if (offered > 0.0f) {
			s->tokens += offered;
			ret = srpc_send_offer(s);
			if (unlikely(ret))
				goto close;
		}

		/* send a response for each completed slot */
		ret = srpc_send_completion_vector(s, tmp);
		if (unlikely(ret))
			goto close;

		/* check if out of tokens */
		if (s->tokens < 1.0f) {
			spin_lock_np(&drained_lock);
			if (s->drained) {
				spin_unlock_np(&drained_lock);
				continue;
			}
			s->drained = true;
			list_add_tail(&drained_sessions, &s->link);
			spin_unlock_np(&drained_lock);
		}
	}

close:
	/* remove from drained list */
	if (s->drained) {
		spin_lock_np(&drained_lock);
		if (s->drained)
			list_del_from(&drained_sessions, &s->link);
		spin_unlock_np(&drained_lock);
	}

	/* wait for in-flight undrains and completions to finish */
	spin_lock_np(&s->lock);
	while (!s->closed || s->undrain_pending ||
	       bitmap_popcount(s->avail_slots, SRPC_MAX_WINDOW) +
	       bitmap_popcount(s->completed_slots, SRPC_MAX_WINDOW) <
	       SRPC_MAX_WINDOW) {
		s->sender_th = thread_self();
		thread_park_and_unlock_np(&s->lock);
		spin_lock_np(&s->lock);
		s->sender_th = NULL;
	}
	spin_unlock_np(&s->lock);

	/* free any left over slots */
	for (i = 0; i < SRPC_MAX_WINDOW; i++) {
		if (s->slots[i])
			srpc_put_slot(s, i);
	}

	/* notify server thread that the sender is done */
	waitgroup_done(&s->send_waiter);
}

static void srpc_server(void *arg)
{
	tcpconn_t *c = (tcpconn_t *)arg;
	struct srpc_session *s;
	thread_t *th;
	int ret;

	s = smalloc(sizeof(*s));
	BUG_ON(!s);
	memset(s, 0, sizeof(*s));
	s->c = c;
	bitmap_init(s->avail_slots, SRPC_MAX_WINDOW, true);
	waitgroup_init(&s->send_waiter);
	waitgroup_add(&s->send_waiter, 1);
	s->tokens = 1.0f;

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
	spin_unlock_np(&s->lock);
	if (th)
		thread_ready(th);

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

	/* init stats */
	atomic64_write(&srpc_stat_req_dropped_, 0);
	atomic64_write(&srpc_stat_req_rx_, 0);
	atomic64_write(&srpc_stat_dreq_rx_, 0);
	atomic64_write(&srpc_stat_resp_tx_, 0);
	atomic64_write(&srpc_stat_offer_tx_, 0);

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
int srpc_enable(srpc_fn_t handler)
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

uint64_t srpc_stat_req_dropped()
{
	return atomic64_read(&srpc_stat_req_dropped_);
}

uint64_t srpc_stat_req_rx() {
	return atomic64_read(&srpc_stat_req_rx_);
}

uint64_t srpc_stat_dreq_rx() {
	return atomic64_read(&srpc_stat_dreq_rx_);
}

uint64_t srpc_stat_resp_tx() {
	return atomic64_read(&srpc_stat_resp_tx_);
}

uint64_t srpc_stat_offer_tx() {
	return atomic64_read(&srpc_stat_offer_tx_);
}
