/*
 * tcp_in.c - the ingress datapath for TCP
 *
 * Based on RFC 793 and RFC 1122 (errata).
 *
 * FIXME: We do too little to prevent heavy fragmentation in the out-of-order
 * RX queue.
 */

#include <base/stddef.h>
#include <runtime/smalloc.h>
#include <net/ip.h>
#include <net/tcp.h>

#include "tcp.h"
#include "defs.h"

#define TCP_SLOWPATH_FLAGS (TCP_FIN|TCP_RST|TCP_URG)

static void __tcp_rx_conn(tcpconn_t *c, struct mbuf *m, uint32_t ack,
			  uint32_t snd_nxt, uint32_t win,
			  const unsigned char *optp, int optlen);

/* four cases for the acceptability test for an incoming segment */
static bool is_acceptable(tcpconn_t *c, uint32_t len, uint32_t seq,
                          uint8_t flags)
{
	assert_spin_lock_held(&c->lock);

	if (len == 0 && c->pcb.rcv_wnd == 0) {
               /* RFC 793 section 3.9 page 69: If the RCV.WND is zero,
                  no segments will be acceptable, but special allowance
                  should be made to accept valid ACKs, URGs and RSTs. */
		if ((flags & TCP_ACK) || (flags & TCP_URG) || (flags & TCP_RST))
			return true;
		return seq == c->pcb.rcv_nxt;
	} else if (len == 0 && c->pcb.rcv_wnd > 0) {
		return wraps_lte(c->pcb.rcv_nxt, seq) &&
		       wraps_lt(seq, c->pcb.rcv_nxt + c->pcb.rcv_wnd);
	} else if (len > 0 && c->pcb.rcv_wnd == 0) {
		return false;
	}

	/* (len > 0 && c->rcv_wnd > 0) */
	return (wraps_lte(c->pcb.rcv_nxt, seq) &&
		wraps_lt(seq, c->pcb.rcv_nxt + c->pcb.rcv_wnd)) ||
	       (wraps_lte(c->pcb.rcv_nxt, seq + len - 1) &&
		wraps_lt(seq + len - 1, c->pcb.rcv_nxt + c->pcb.rcv_wnd));
}

/* see reset generation (RFC 793) */
static void send_rst(tcpconn_t *c, bool acked, uint32_t seq, uint32_t ack,
		     uint32_t len)
{
	if (acked) {
		tcp_tx_raw_rst(c->e.laddr, c->e.raddr, ack);
		return;
	}
	tcp_tx_raw_rst_ack(c->e.laddr, c->e.raddr, 0, seq + len);
}

static void tcp_rx_append_text(tcpconn_t *c, struct mbuf *m)
{
	uint64_t nxt_wnd;
	uint32_t len;

	assert_spin_lock_held(&c->lock);

	/* verify assumptions enforced by acceptability testing */
	assert(wraps_lte(m->seg_seq, c->pcb.rcv_nxt));
	assert(wraps_gt(m->seg_end, c->pcb.rcv_nxt));

	/* does the next receive octet clip the head of the text? */
	if (wraps_lt(m->seg_seq, c->pcb.rcv_nxt)) {
		len = c->pcb.rcv_nxt - m->seg_seq;
		mbuf_pull(m, len);
		m->seg_seq += len;
	}

	/* does the receive window clip the tail of the text? */
	if (wraps_lt(c->pcb.rcv_nxt + c->pcb.rcv_wnd, m->seg_end)) {
		len = m->seg_end - (c->pcb.rcv_nxt + c->pcb.rcv_wnd);
		mbuf_trim(m, len);
		m->seg_end = c->pcb.rcv_nxt + c->pcb.rcv_wnd;
	}

	/* enqueue the text */
	assert(c->pcb.rcv_wnd >= m->seg_end - m->seg_seq);
	nxt_wnd = (uint64_t)m->seg_end;
	nxt_wnd |= ((uint64_t)(c->pcb.rcv_wnd - (m->seg_end - m->seg_seq)) << 32);
	store_release(&c->pcb.rcv_nxt_wnd, nxt_wnd);
	list_add_tail(&c->rxq, &m->link);
}

/* process RX text segments, returning true if @m is used for text */
static bool tcp_rx_text(tcpconn_t *c, struct mbuf *m, bool *wake, bool *fin)
{
	struct mbuf *pos;

	assert_spin_lock_held(&c->lock);

	/* don't accept any text if the receive window is zero */
	if (c->pcb.rcv_wnd == 0)
		return false;

	if (wraps_lte(m->seg_seq, c->pcb.rcv_nxt)) {
		/* we got the next in-order segment */
		STAT(RX_TCP_IN_ORDER)++;
		if ((m->flags & (TCP_PUSH | TCP_FIN)) != 0 ||
		    !list_empty(&c->rxq)) {
			*wake = true;
			*fin |= (m->flags & TCP_FIN) > 0;
		}
		tcp_rx_append_text(c, m);
	} else {
		/* we got an out-of-order segment */
		STAT(RX_TCP_OUT_OF_ORDER)++;

		if (c->rxq_ooo_len >= TCP_OOO_MAX_SIZE)
			return false;

		list_for_each_rev(&c->rxq_ooo, pos, link) {
			if (wraps_gt(m->seg_end, pos->seg_end)) {
				list_add_after(&pos->link, &m->link);
				c->rxq_ooo_len++;
				goto drain;
			} else if (wraps_gte(m->seg_seq, pos->seg_seq)) {
				return false;
			}
		}

		list_add(&c->rxq_ooo, &m->link);
		c->rxq_ooo_len++;
	}

drain:
	/* attempt to drain the out-of-order RX queue */
	while (true) {
		pos = list_top(&c->rxq_ooo, struct mbuf, link);
		if (!pos)
			break;

		/* has the segment been fully received already? */
		if (wraps_lte(pos->seg_end, c->pcb.rcv_nxt)) {
			list_del(&pos->link);
			c->rxq_ooo_len--;
			mbuf_free(pos);
			continue;
		}

		/* is the segment still out-of-order? */
		if (wraps_gt(pos->seg_seq, c->pcb.rcv_nxt))
			break;

		/* we got the next in-order segment */
		list_del(&pos->link);
		c->rxq_ooo_len--;
		*wake = true;
		*fin |= (pos->flags & TCP_FIN) > 0;
		tcp_rx_append_text(c, pos);
	}

	if (c->pcb.rcv_wnd == 0)
		*wake = true;

	return true;
}

/* fast path for handling ingress packets for TCP connections */
void tcp_rx_conn(struct trans_entry *e, struct mbuf *m)
{
	tcpconn_t *c = container_of(e, tcpconn_t, e);
	struct list_head q;
	thread_t *rx_th = NULL;
	const struct ip_hdr *iphdr;
	const struct tcp_hdr *tcphdr;
	const unsigned char *optp;
	int optlen;
	uint64_t nxt_wnd;
	uint32_t seq, ack, len, snd_nxt, hdr_len, win;
	bool do_ack = false, slow_path;

	list_head_init(&q);
	snd_nxt = load_acquire(&c->pcb.snd_nxt);

	/* find header offsets */
	iphdr = mbuf_network_hdr(m, *iphdr);
	mbuf_mark_transport_offset(m);
	tcphdr = mbuf_pull_hdr_or_null(m, *tcphdr);
	if (unlikely(!tcphdr)) {
		mbuf_free(m);
		return;
	}

	/* parse header */
	seq = ntoh32(tcphdr->seq);
	ack = ntoh32(tcphdr->ack);
	win = (uint32_t)ntoh16(tcphdr->win) << c->pcb.snd_wscale;
	hdr_len = tcphdr->off * sizeof(uint32_t);
	if (unlikely(hdr_len < sizeof(struct tcp_hdr))) {
		mbuf_free(m);
		return;
	}
	len = ntoh16(iphdr->len) - sizeof(*iphdr) - hdr_len;
	if (unlikely(len > mbuf_length(m) || len > c->pcb.rcv_mss)) {
		mbuf_free(m);
		return;
	}

	m->seg_seq = seq;
	m->seg_end = seq + len;
	m->flags = tcphdr->flags;

	/* pull off options */
	optlen = hdr_len - sizeof(struct tcp_hdr);
	optp = mbuf_pull(m, optlen);

	/* Use slow path if not regular flags and the next segment */
	slow_path = (tcphdr->flags & TCP_SLOWPATH_FLAGS) != 0 ||
	            (len == 0) || wraps_gt(ack, snd_nxt);

	spin_lock_np(&c->lock);

	/* Is the connection in the established state? */
	slow_path |= (c->pcb.state != TCP_STATE_ESTABLISHED);

	/* Might we need to unblock waiting senders? */
	slow_path |= tcp_is_snd_full(c);

	/* Is the packet on the next in-order boundary? */
	slow_path |= (seq != c->pcb.rcv_nxt) || !list_empty(&c->rxq_ooo);

	/* Does it fit perfectly in the receive window? */
	slow_path |= c->pcb.rcv_wnd < len;

	/* Does the ack land outside snd_nxt? */
	slow_path |= wraps_gt(ack, snd_nxt);

	if (unlikely(slow_path))
		return __tcp_rx_conn(c, m, ack, snd_nxt, win, optp, optlen);

	STAT(RX_TCP_IN_ORDER)++;

	/* process acks and update send window */
	if (wraps_lte(c->pcb.snd_una, ack)) {
		/* did sent segments get acked? */
		if (c->pcb.snd_una != ack) {
			c->rep_acks = 0;
			c->pcb.snd_una = ack;
			tcp_conn_ack(c, &q);
		}

		/* should we update the send window? */
		if (wraps_lt(c->pcb.snd_wl1, seq) ||
		    (c->pcb.snd_wl1 == seq &&
		     wraps_lte(c->pcb.snd_wl2, ack))) {
			c->pcb.snd_wnd = win;
			c->pcb.snd_wl1 = seq;
			c->pcb.snd_wl2 = ack;
			c->rep_acks = 0;
		}
	}

	nxt_wnd = (uint64_t)m->seg_end;
	nxt_wnd |= ((uint64_t)(c->pcb.rcv_wnd - len) << 32);
	store_release(&c->pcb.rcv_nxt_wnd, nxt_wnd);

	/* should we wake a thread */
	if (!list_empty(&c->rxq) || (tcphdr->flags & TCP_PUSH) > 0)
		rx_th = waitq_signal(&c->rx_wq, &c->lock);

	/* handle delayed acks */
	if (++c->acks_delayed_cnt >= 2) {
		c->ack_delayed = false;
		do_ack = true;
		c->acks_delayed_cnt = 0;
	} else if (!c->ack_delayed) {
		c->ack_ts = microtime();
		c->ack_delayed = true;
		c->next_timeout = MIN(c->next_timeout, c->ack_ts + TCP_ACK_TIMEOUT);
	}

	list_add_tail(&c->rxq, &m->link);
	tcp_debug_ingress_pkt(c, m);
	spin_unlock_np(&c->lock);

	/* deferred work (delayed until after the lock was dropped) */
	waitq_signal_finish(rx_th);
	mbuf_list_free(&q);
	if (do_ack)
		tcp_tx_ack(c);
}

static int tcp_parse_options(tcpconn_t *c, const unsigned char *ptr, int len)
{
	int opt_en = 0;
	uint16_t mss = 0;
	uint8_t wscale = 0;

	while (len > 0) {
		int opcode = *ptr++;
		int opsize;

		switch(opcode) {
		case TCP_OPT_EOL:
			goto done;
		case TCP_OPT_NOP:
			len--;
			continue;
		case TCP_OPT_MSS:
			opsize = *ptr++;
			if (opsize == TCP_OLEN_MSS) {
				mss = ntoh16(*(uint16_t *)ptr);
				opt_en |= TCP_OPTION_MSS;
			}
			break;
		case TCP_OPT_WSCALE:
			opsize = *ptr++;
			if (opsize == TCP_OLEN_WSCALE) {
				wscale = *(uint8_t *)ptr;
				if (wscale > 14)
					wscale = 14;
				opt_en |= TCP_OPTION_WSCALE;
			}
			break;
		default:
			opsize = *ptr++;
		}
		ptr += opsize-2;
		len -= opsize;
	}

done:
	c->pcb.snd_mss = MIN(MAX(mss, TCP_MIN_MSS), c->pcb.rcv_mss);
	c->pcb.snd_wscale = wscale;
	if (!(opt_en & TCP_OPTION_WSCALE)) {
		c->pcb.rcv_wnd = c->winmax = MIN(c->winmax, UINT16_MAX);
		c->pcb.rcv_wscale = 0;
	}
	if (!(opt_en & TCP_OPTION_MSS)) {
		c->pcb.snd_mss = tcp_calculate_mss(ETH_DEFAULT_MTU);
	}
	return opt_en;
}

/* slow path for handling ingress packets for TCP connections */
static __noinline void
__tcp_rx_conn(tcpconn_t *c, struct mbuf *m, uint32_t ack, uint32_t snd_nxt,
	      uint32_t win, const unsigned char *optp, int optlen)
{
	struct list_head q, waiters;
	thread_t *rx_th = NULL;
	struct mbuf *retransmit = NULL;
	uint32_t seq, len;
	bool do_ack = false, do_drop = true, fin = false, snd_was_full;
	bool ack_same = false, wnd_updated = false;
	int ret;

	list_head_init(&q);
	list_head_init(&waiters);

	seq = m->seg_seq;
	len = m->seg_end - m->seg_seq;

	if (unlikely((m->flags & TCP_FIN) > 0))
		len++;

	if (unlikely(c->pcb.state == TCP_STATE_CLOSED)) {
		if ((m->flags & TCP_RST) == 0)
			send_rst(c, false, seq, ack, len);
		goto done;
	}

	if (unlikely(c->pcb.state == TCP_STATE_SYN_SENT)) {
		if ((m->flags & TCP_ACK) > 0) {
			if (wraps_lte(ack, c->pcb.iss) ||
			    wraps_gt(ack, snd_nxt)) {
				send_rst(c, false, seq, ack, len);
				goto done;
			}
			if ((m->flags & TCP_RST) > 0) {
				/* check if the ack is valid */
				if (wraps_lte(c->pcb.snd_una, ack) &&
				    wraps_lte(ack, snd_nxt)) {
					tcp_conn_fail(c, ECONNRESET);
					goto done;
				}
			}
		} else if ((m->flags & TCP_RST) > 0) {
			goto done;
		}
		if ((m->flags & TCP_SYN) > 0) {
			struct tcp_options opts;
			c->pcb.rcv_nxt = seq + 1;
			c->pcb.irs = seq;

			/* set up options */
			opts.opt_en = tcp_parse_options(c, optp, optlen);
			opts.mss = c->pcb.rcv_mss;
			opts.wscale = c->pcb.rcv_wscale;

			if ((m->flags & TCP_ACK) > 0) {
				c->pcb.snd_una = ack;
				tcp_conn_ack(c, &q);
			}
			if (wraps_gt(c->pcb.snd_una, c->pcb.iss)) {
				do_ack = true;
				c->pcb.snd_wnd = win;
				c->pcb.snd_wl1 = seq;
				c->pcb.snd_wl2 = ack;
				tcp_conn_set_state(c, TCP_STATE_ESTABLISHED);
			} else {
				ret = tcp_tx_ctl(c, TCP_SYN | TCP_ACK, &opts);
				if (unlikely(ret)) {
					goto done; /* feign packet loss */
				}
				tcp_conn_set_state(c, TCP_STATE_SYN_RECEIVED);
			}
		}
		goto done;
	}

	/*
	 * TCP_STATE_SYN_RECEIVED || TCP_STATE_ESTABLISHED ||
	 * TCP_STATE_FIN_WAIT1 || TCP_STATE_FIN_WAIT2 ||
	 * TCP_STATE_CLOSE_WAIT || TCP_STATE_CLOSING ||
	 * TCP_STATE_LAST_ACK || TCP_STATE_TIME_WAIT
	 */

	/* step 1 - acceptability testing */
	if (unlikely(!is_acceptable(c, len, seq, m->flags))) {
		do_ack = (m->flags & TCP_RST) == 0;
		goto done;
	}

	/* step 2 - RST */
	if (unlikely((m->flags & TCP_RST) > 0)) {
		tcp_conn_fail(c, ECONNRESET);
		goto done;
	}

	/* step 3 - security checks skipped */

	/* step 4 - SYN */
	if (unlikely((m->flags & TCP_SYN) > 0)) {
		send_rst(c, (m->flags & TCP_ACK) > 0, seq, ack, len);
		tcp_conn_fail(c, ECONNRESET);
		goto done;
	}

	/* step 5 - ACK */
	if (unlikely((m->flags & TCP_ACK) == 0)) {
		goto done;
	}
	if (unlikely(c->pcb.state == TCP_STATE_SYN_RECEIVED)) {
		if (!(wraps_lte(c->pcb.snd_una, ack) &&
		      wraps_lte(ack, snd_nxt))) {
			send_rst(c, true, seq, ack, len);
			do_drop = true;
			goto done;
		}
		c->pcb.snd_wnd = win;
		c->pcb.snd_wl1 = seq;
		c->pcb.snd_wl2 = ack;
		tcp_conn_set_state(c, TCP_STATE_ESTABLISHED);
	}

	/* process ack and window update */
	snd_was_full = tcp_is_snd_full(c);
	if (wraps_lte(c->pcb.snd_una, ack) && wraps_lte(ack, snd_nxt)) {
		/* did sent segments get acked? */
		if (c->pcb.snd_una != ack) {
			c->pcb.snd_una = ack;
			tcp_conn_ack(c, &q);
		} else {
			ack_same = true;
		}

		/* should we update the send window? */
		if (wraps_lt(c->pcb.snd_wl1, seq) ||
		    (c->pcb.snd_wl1 == seq &&
		     wraps_lte(c->pcb.snd_wl2, ack))) {
			if (!ack_same || c->pcb.snd_wnd <= win) {
				if (c->pcb.snd_wnd != win || c->pcb.snd_wl2 != ack)
					wnd_updated = true;
				c->pcb.snd_wnd = win;
				c->pcb.snd_wl1 = seq;
				c->pcb.snd_wl2 = ack;
			}
		}
	} else if (wraps_gt(ack, snd_nxt)) {
		do_ack = true;
		goto done;
	}
	if (snd_was_full && !tcp_is_snd_full(c))
		waitq_release_start(&c->tx_wq, &waiters);

	/*
	 * Fast retransmit -> detect a duplicate ACK if:
	 * 1. The ACK number is the same as the largest seen.
	 * 2. There is unacknowledged data pending.
	 * 3. There is no data payload included with the ACK.
	 * 4. There is no window update.
	 */
	if (unlikely(ack_same && c->pcb.snd_una != c->pcb.snd_nxt &&
		     len == 0 && !wnd_updated)) {
		c->rep_acks++;
		if (c->rep_acks >= TCP_FAST_RETRANSMIT_THRESH) {
			if (c->tx_exclusive) {
				c->do_fast_retransmit = true;
				c->fast_retransmit_last_ack = ack;
			} else {
				retransmit = tcp_tx_fast_retransmit_start(c);
			}
			c->rep_acks = 0;
		}
	} else if (c->pcb.snd_una == ack) {
		c->rep_acks = 0;
	}

	if (c->pcb.state == TCP_STATE_FIN_WAIT1 &&
	    c->pcb.snd_una == snd_nxt) {
		tcp_conn_set_state(c, TCP_STATE_FIN_WAIT2);
	} else if (c->pcb.state == TCP_STATE_CLOSING &&
		   c->pcb.snd_una == snd_nxt) {
		c->time_wait_ts = microtime();
		tcp_conn_set_state(c, TCP_STATE_TIME_WAIT);
	} else if (c->pcb.state == TCP_STATE_LAST_ACK &&
		   c->pcb.snd_una == snd_nxt) {
		tcp_conn_set_state(c, TCP_STATE_CLOSED);
		tcp_conn_put(c); /* safe because RCU + preempt is disabled */
		goto done;
	}

	/* step 6 - URG support skipped */

	/* step 7 - segment text */
	if (len > 0 &&
	    (c->pcb.state == TCP_STATE_ESTABLISHED ||
	     c->pcb.state == TCP_STATE_FIN_WAIT1 ||
	     c->pcb.state == TCP_STATE_FIN_WAIT2)) {
		bool wake = false;
		m->seg_end = seq + len;

#ifdef TCP_RX_STATS
		uint64_t before_tsc = rdtsc();
		do_drop = !tcp_rx_text(c, m, &wake, &fin);
		STAT(RX_TCP_TEXT_CYCLES) += rdtsc() - before_tsc;
#else
		do_drop = !tcp_rx_text(c, m, &wake, &fin);
#endif

		if (wake) {
			assert(!list_empty(&c->rxq));
			assert(do_drop == false);
			rx_th = waitq_signal(&c->rx_wq, &c->lock);
		}
		if (++c->acks_delayed_cnt >= 2) {
			do_ack = true;
		} else if (!c->ack_delayed) {
			c->ack_delayed = true;
			c->ack_ts = microtime();
		}
		do_ack |= !list_empty(&c->rxq_ooo);
	}

	/* step 8 - FIN */
	if (likely(!fin))
		goto done;
	assert(c->pcb.state != TCP_STATE_SYN_RECEIVED);
	if (c->pcb.state == TCP_STATE_ESTABLISHED) {
		tcp_conn_set_state(c, TCP_STATE_CLOSE_WAIT);
	} else if (c->pcb.state == TCP_STATE_FIN_WAIT1) {
		assert(c->pcb.snd_una != snd_nxt);
		tcp_conn_set_state(c, TCP_STATE_CLOSING);
	} else if (c->pcb.state == TCP_STATE_FIN_WAIT2) {
		c->time_wait_ts = microtime();
		do_ack = true;
		tcp_conn_set_state(c, TCP_STATE_TIME_WAIT);
	}

done:
	tcp_timer_update(c);
	tcp_debug_ingress_pkt(c, m);
	if (do_ack) {
		c->ack_delayed = false;
		c->acks_delayed_cnt = 0;
	}
	spin_unlock_np(&c->lock);

	/* deferred work (delayed until after the lock was dropped) */
	waitq_release_finish(&waiters);
	if (rx_th)
		waitq_signal_finish(rx_th);
	mbuf_list_free(&q);
	tcp_tx_fast_retransmit_finish(c, retransmit);
	if (do_ack)
		tcp_tx_ack(c);
	if (do_drop)
		mbuf_free(m);
}

/* handles ingress packets for TCP listener queues */
tcpconn_t *tcp_rx_listener(struct netaddr laddr, struct mbuf *m)
{
	struct netaddr raddr;
	const struct ip_hdr *iphdr;
	const struct tcp_hdr *tcphdr;
	const unsigned char *optp;
	tcpconn_t *c;
	struct tcp_options opts;
	uint32_t hdr_len;
	int optlen, ret;

	/* find header offsets */
	iphdr = mbuf_network_hdr(m, *iphdr);
	tcphdr = mbuf_pull_hdr_or_null(m, *tcphdr);
	if (unlikely(!tcphdr))
		return NULL;

	/* calculate local and remote network addresses */
	raddr.ip = ntoh32(iphdr->saddr);
	raddr.port = ntoh16(tcphdr->sport);

	/* do exactly what RFC 793 says */
	if ((tcphdr->flags & TCP_RST) > 0)
		return NULL;
	if ((tcphdr->flags & TCP_ACK) > 0) {
		tcp_tx_raw_rst(laddr, raddr, ntoh32(tcphdr->ack));
		return NULL;
	}
	if ((tcphdr->flags & TCP_SYN) == 0)
		return NULL;

	/* TODO: the spec requires us to enqueue but not post any data */
	hdr_len = tcphdr->off * sizeof(uint32_t);
	if (ntoh16(iphdr->len) - sizeof(*iphdr) != hdr_len)
		return NULL;

	/* parse options */
	optlen = hdr_len - sizeof(struct tcp_hdr);
	optp = mbuf_pull_or_null(m, optlen);
	if (!optp)
		return NULL;

	/* we have a valid SYN packet, initialize a new connection */
	c = tcp_conn_alloc();
	if (unlikely(!c))
		return NULL;
	c->pcb.irs = ntoh32(tcphdr->seq);
	c->pcb.rcv_nxt = c->pcb.irs + 1;

	/* set up options */
	opts.opt_en = tcp_parse_options(c, optp, optlen);
	opts.mss = c->pcb.rcv_mss;
	opts.wscale = c->pcb.rcv_wscale;

	/*
	 * attach the connection to the transport layer. From this point onward
	 * ingress packets can be dispatched to the connection.
	 */
	ret = tcp_conn_attach(c, laddr, raddr);
	if (unlikely(ret)) {
		sfree(c);
		return NULL;
	}
	tcp_debug_ingress_pkt(c, m);

	/* finally, send a SYN/ACK to the remote host */
	spin_lock_np(&c->lock);
	ret = tcp_tx_ctl(c, TCP_SYN | TCP_ACK, &opts);
	if (unlikely(ret)) {
		spin_unlock_np(&c->lock);
		tcp_conn_destroy(c);
		return NULL;
	}
	tcp_conn_get(c); /* take a ref for the state machine */
	tcp_conn_set_state(c, TCP_STATE_SYN_RECEIVED);
	spin_unlock_np(&c->lock);

	return c;
}

void tcp_rx_closed(struct mbuf *m)
{
	struct netaddr l, r;
	uint32_t len;
	const struct ip_hdr *iphdr;
	const struct tcp_hdr *tcphdr;

	iphdr = mbuf_network_hdr(m, *iphdr);
	tcphdr = mbuf_pull_hdr_or_null(m, *tcphdr);
	if (!tcphdr)
		return;

	if ((tcphdr->flags & TCP_RST) > 0)
		return;

	l.ip = ntoh32(iphdr->daddr);
	l.port = ntoh16(tcphdr->dport);

	r.ip = ntoh32(iphdr->saddr);
	r.port = ntoh16(tcphdr->sport);

	if ((tcphdr->flags & TCP_ACK) > 0) {
		tcp_tx_raw_rst(l, r, ntoh32(tcphdr->ack));
	} else {
		len = ntoh16(iphdr->len) - sizeof(*iphdr) - tcphdr->off * 4;
		tcp_tx_raw_rst_ack(l, r, 0, ntoh32(tcphdr->seq) + len);
	}
}
