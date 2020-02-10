/*
 * RPC server-side support
 */

#include <base/stddef.h>
#include <base/list.h>
#include <base/log.h>
#include <runtime/rpc.h>
#include <runtime/tcp.h>
#include <runtime/sync.h>
#include <runtime/smalloc.h>
#include <runtime/thread.h>
#include <runtime/runtime.h>

#include "util.h"
#include "proto.h"

/* the maximum supported window size */
#define SRPC_MAX_WINDOW		64

/* the handler function for each RPC */
static srpc_fn_t srpc_handler;

struct srpc_session {
	tcpconn_t		*c;
	waitgroup_t		send_waiter;
	bool			probe_pending;
	bool			probe_accepted;

	/* shared state between receiver and sender */
	DEFINE_BITMAP(avail_slots, SRPC_MAX_WINDOW);

	/* shared state between workers and sender */
	spinlock_t		lock;
	int			closed;
	thread_t		*sender_th;
	DEFINE_BITMAP(completed_slots, SRPC_MAX_WINDOW);

	/* worker slots (one for each credit issued) */
	struct srpc_ctx		*slots[SRPC_MAX_WINDOW];
};

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

	/* handle probe request */
	if (chdr.op == RPC_OP_PROBE) {
		thread_t *th;

		spin_lock_np(&s->lock);
		s->probe_pending = true;
		s->probe_accepted = runtime_standing_queue_us() <= 20;
		th = s->sender_th;
		s->sender_th = NULL;
		spin_unlock_np(&s->lock);
		if (th)
			thread_ready(th);

		/* trim payload if the probe wasn't accepted */
		if (!s->probe_accepted) {
			char buf[SRPC_BUF_SIZE];
			ret = tcp_read_full(s->c, buf, chdr.len);
			if (unlikely(ret <= 0)) {
				if (ret == 0)
					return -EIO;
				return ret;
			}
			return 0;
		}
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

	s->slots[idx]->req_len = chdr.len;
	s->slots[idx]->resp_len = 0;
	ret = thread_spawn(srpc_worker, s->slots[idx]);
	BUG_ON(ret);
	return ret;
}

#define SRPC_RTT	10
#define SRPC_PROBE_RPS	200000

static atomic_t srpc_conn_count;

static uint64_t srpc_calculate_probe_us(void)
{
	/*
	 * TODO: Could adjust based on actual observed probe rate, not number
	 * of connections. Should also subtract RTT estimate from delay.
	 */

	uint64_t delay = ONE_SECOND / SRPC_PROBE_RPS *
			 atomic_read(&srpc_conn_count);
	if (delay < SRPC_RTT)
		return 0;
	return delay - SRPC_RTT;
}

static int srpc_send_call(struct srpc_session *s, struct srpc_ctx *c)
{
	struct iovec vec[2];
	struct srpc_hdr shdr;
	int ret;

	/* must have a response payload */
	if (unlikely(c->resp_len == 0))
		return -EINVAL;

	/* craft the response header */
	shdr.magic = RPC_RESP_MAGIC;
	shdr.op = RPC_OP_CALL;
	shdr.len = c->resp_len;
	shdr.delay_us = runtime_standing_queue_us();
	shdr.probe_us = srpc_calculate_probe_us();
	shdr.accepted = true;

	/* initialize the SG vector */
	vec[0].iov_base = &shdr;
	vec[0].iov_len = sizeof(shdr);
	vec[1].iov_base = c->resp_buf;
	vec[1].iov_len = c->resp_len;

	/* send the packet */
	ret = tcp_writev_full(s->c, vec, 2);
	if (unlikely(ret < 0))
		return ret;

	assert(ret == sizeof(shdr) + c->resp_len);
	return 0;
}

static int srpc_send_probe(struct srpc_session *s, bool accept)
{
	struct srpc_hdr shdr;
	ssize_t ret;

	/* initialize the header */
	shdr.magic = RPC_RESP_MAGIC;
	shdr.op = RPC_OP_PROBE;
	shdr.len = 0;
	shdr.delay_us = runtime_standing_queue_us();
	shdr.probe_us = srpc_calculate_probe_us();
	shdr.accepted = accept;

	/* send the request */
	ret = tcp_write_full(s->c, &shdr, sizeof(shdr));
	if (unlikely(ret < 0))
		return ret;

	assert(ret == sizeof(shdr));
	return 0;
}

static void srpc_sender(void *arg)
{
	DEFINE_BITMAP(tmp, SRPC_MAX_WINDOW);
	struct srpc_session *s = (struct srpc_session *)arg;
	int ret, i;
	bool sleep, probe, accepted;

	while (true) {
		/* find slots that have completed */
		spin_lock_np(&s->lock);
		while (true) {
			sleep = !s->closed && !s->probe_pending &&
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
		probe = s->probe_pending;
		s->probe_pending = false;
		accepted = s->probe_accepted;
		spin_unlock_np(&s->lock);

		/* send a probe response if requested */
		/* TODO: okay to only send reject messages? */
		if (probe && !accepted) {
			ret = srpc_send_probe(s, accepted);
			if (unlikely(ret))
				goto close;
		}

		/* send a response for each completed slot */
		ret = 0;
		bitmap_for_each_set(tmp, SRPC_MAX_WINDOW, i) {
			ret = srpc_send_call(s, s->slots[i]);
			srpc_put_slot(s, i);
		}
		if (unlikely(ret))
			goto close;
	}

close:
	/* wait for in-flight completions to finish */
	spin_lock_np(&s->lock);
	while (!s->closed ||
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

	atomic_inc(&srpc_conn_count);

	s = smalloc(sizeof(*s));
	BUG_ON(!s);
	memset(s, 0, sizeof(*s));
	s->c = c;
	bitmap_init(s->avail_slots, SRPC_MAX_WINDOW, true);
	waitgroup_init(&s->send_waiter);
	waitgroup_add(&s->send_waiter, 1);

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
	atomic_dec(&srpc_conn_count);
	tcp_close(c);
	sfree(s);
}

static void srpc_listener(void *arg)
{
	struct netaddr laddr;
	tcpconn_t *c;
	tcpqueue_t *q;
	int ret;

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
