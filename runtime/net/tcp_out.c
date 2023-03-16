/*
 * tcp_out.c - the egress datapath for TCP
 */

#include <string.h>

#include <base/stddef.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/chksum.h>

#include "tcp.h"
#include "defs.h"

static void tcp_tx_release_mbuf(struct mbuf *m)
{
	if (atomic_dec_and_test(&m->ref))
		net_tx_release_mbuf(m);
}

static uint16_t tcp_hdr_chksum(uint32_t local_ip, uint32_t remote_ip,
			       uint16_t len)
{
	if (cfg_directpath_enabled())
		return 0;

	return ipv4_phdr_cksum(IPPROTO_TCP, local_ip, remote_ip, len);
}

static __always_inline struct tcp_hdr *
tcp_push_tcphdr(struct mbuf *m, tcpconn_t *c, uint8_t flags,
		uint8_t off, uint16_t l4len)
{
	struct tcp_hdr *tcphdr;
	uint64_t rcv_nxt_wnd = load_acquire(&c->pcb.rcv_nxt_wnd);
	tcp_seq ack = c->tx_last_ack = (uint32_t)rcv_nxt_wnd;
	uint32_t win = c->tx_last_win = rcv_nxt_wnd >> 32;

	/* write the tcp header */
	tcphdr = mbuf_push_hdr(m, *tcphdr);
	mbuf_mark_transport_offset(m);
	tcphdr->sport = hton16(c->e.laddr.port);
	tcphdr->dport = hton16(c->e.raddr.port);
	tcphdr->ack = hton32(ack);
	tcphdr->off = off;
	tcphdr->flags = flags;
	tcphdr->win = hton16(win >> c->pcb.rcv_wscale);
	tcphdr->seq = hton32(m->seg_seq);
	tcphdr->sum = tcp_hdr_chksum(c->e.laddr.ip, c->e.raddr.ip,
				     off * sizeof(uint32_t) + l4len);
	return tcphdr;
}

/**
 * tcp_tx_raw_rst - send a RST without an established connection
 * @laddr: the local address
 * @raddr: the remote address
 * @seq: the segement's sequence number
 *
 * Returns 0 if successful, otherwise fail.
 */
int tcp_tx_raw_rst(struct netaddr laddr, struct netaddr raddr, tcp_seq seq)
{
	struct tcp_hdr *tcphdr;
	struct mbuf *m;
	int ret;

	m = net_tx_alloc_mbuf();
	if (unlikely((!m)))
		return -ENOMEM;

	m->txflags = OLFLAG_TCP_CHKSUM;

	/* write the tcp header */
	tcphdr = mbuf_push_hdr(m, *tcphdr);
	tcphdr->sport = hton16(laddr.port);
	tcphdr->dport = hton16(raddr.port);
	tcphdr->seq = hton32(seq);
	tcphdr->ack = hton32(0);
	tcphdr->off = 5;
	tcphdr->flags = TCP_RST;
	tcphdr->win = hton16(0);
	tcphdr->sum = tcp_hdr_chksum(laddr.ip, raddr.ip, 0);

	/* transmit packet */
	ret = net_tx_ip(m, IPPROTO_TCP, raddr.ip);
	if (unlikely(ret))
		mbuf_free(m);
	return ret;
}

/**
 * tcp_tx_raw_rst_ack - send a RST/ACK without an established connection
 * @laddr: the local address
 * @raddr: the remote address
 * @seq: the segment's sequence number
 * @ack: the segment's acknowledgement number
 *
 * Returns 0 if successful, otherwise fail.
 */
int tcp_tx_raw_rst_ack(struct netaddr laddr, struct netaddr raddr,
		       tcp_seq seq, tcp_seq ack)
{
	struct tcp_hdr *tcphdr;
	struct mbuf *m;
	int ret;

	m = net_tx_alloc_mbuf();
	if (unlikely((!m)))
		return -ENOMEM;

	m->txflags = OLFLAG_TCP_CHKSUM;

	/* write the tcp header */
	tcphdr = mbuf_push_hdr(m, *tcphdr);
	tcphdr->sport = hton16(laddr.port);
	tcphdr->dport = hton16(raddr.port);
	tcphdr->seq = hton32(seq);
	tcphdr->ack = hton32(ack);
	tcphdr->off = 5;
	tcphdr->flags = TCP_RST | TCP_ACK;
	tcphdr->win = hton16(0);
	tcphdr->sum = tcp_hdr_chksum(laddr.ip, raddr.ip, 0);

	/* transmit packet */
	ret = net_tx_ip(m, IPPROTO_TCP, raddr.ip);
	if (unlikely(ret))
		mbuf_free(m);
	return ret;
}

/**
 * tcp_tx_ack - send an acknowledgement and window update packet
 * @c: the connection to send the ACK
 *
 * Returns 0 if succesful, otherwise fail.
 */
int tcp_tx_ack(tcpconn_t *c)
{
	struct mbuf *m;
	int ret;

	m = net_tx_alloc_mbuf();
	if (unlikely(!m))
		return -ENOMEM;

	m->txflags = OLFLAG_TCP_CHKSUM;
	m->seg_seq = load_acquire(&c->pcb.snd_nxt);
	tcp_push_tcphdr(m, c, TCP_ACK, 5, 0);

	/* transmit packet */
	tcp_debug_egress_pkt(c, m);
	ret = net_tx_ip(m, IPPROTO_TCP, c->e.raddr.ip);
	if (unlikely(ret))
		mbuf_free(m);
	return ret;
}

/**
 * tcp_tx_probe_window - send a packet to probe the window size
 * @c: the connection to probe
 *
 * Deliberately use a sequence that has already been acked. The receiver
 * will find it fails acceptability testing, and immediately send back an
 * ack with the latest window.
 *
 * Returns 0 if succesful, otherwise fail.
 */
int tcp_tx_probe_window(tcpconn_t *c)
{
	struct mbuf *m;
	int ret;

	m = net_tx_alloc_mbuf();
	if (unlikely(!m))
		return -ENOMEM;

	m->txflags = OLFLAG_TCP_CHKSUM;
	m->seg_seq = load_acquire(&c->pcb.snd_una) - 1;
	tcp_push_tcphdr(m, c, TCP_ACK, 5, 0);

	/* transmit packet */
	tcp_debug_egress_pkt(c, m);
	ret = net_tx_ip(m, IPPROTO_TCP, c->e.raddr.ip);
	if (unlikely(ret))
		mbuf_free(m);
	return ret;
}

static int tcp_push_options(struct mbuf *m, const struct tcp_options *opts)
{
	uint32_t *ptr;
	int len = 0;

	/* WARNING: the order matters, as some devices are broken */

	if (opts->opt_en & TCP_OPTION_WSCALE) {
		ptr = (uint32_t *)mbuf_push(m, sizeof(uint32_t));
		*ptr = hton32((TCP_OPT_NOP << 24) | (TCP_OPT_WSCALE << 16) |
			      (TCP_OLEN_WSCALE << 8) | opts->wscale);
		len++;
	}
	if (opts->opt_en & TCP_OPTION_MSS) {
		ptr = (uint32_t *)mbuf_push(m, sizeof(uint32_t));
		*ptr = hton32((TCP_OPT_MSS << 24) | (TCP_OLEN_MSS << 16) |
			      opts->mss);
		len++;
	}

	return len;
}

/**
 * tcp_tx_ctl - sends a control message without data
 * @c: the TCP connection
 * @flags: the control flags (e.g. TCP_SYN, TCP_FIN, etc.)
 * @opts: TCP options to include
 *
 * WARNING: The caller must have write exclusive access to the socket or hold
 * @c->lock while write exclusion isn't taken.
 *
 * Returns 0 if successful, -ENOMEM if out memory.
 */
int tcp_tx_ctl(tcpconn_t *c, uint8_t flags, const struct tcp_options *opts)
{
	struct mbuf *m;
	int ret = 0;

	BUG_ON(!c->tx_exclusive && !spin_lock_held(&c->lock));

	m = net_tx_alloc_mbuf();
	if (unlikely(!m))
		return -ENOMEM;

	m->txflags = OLFLAG_TCP_CHKSUM;
	m->seg_seq = c->pcb.snd_nxt;
	m->seg_end = c->pcb.snd_nxt + 1;
	m->flags = flags;

	if (opts)
		ret = tcp_push_options(m, opts);
	tcp_push_tcphdr(m, c, flags, 5 + ret, 0);
	store_release(&c->pcb.snd_nxt, c->pcb.snd_nxt + 1);
	list_add_tail(&c->txq, &m->link);
	m->timestamp = microtime();
	atomic_write(&m->ref, 2);
	m->release = tcp_tx_release_mbuf;
	tcp_debug_egress_pkt(c, m);
	ret = net_tx_ip(m, IPPROTO_TCP, c->e.raddr.ip);
	if (unlikely(ret)) {
		/* pretend the packet was sent */
		atomic_write(&m->ref, 1);
	}
	return ret;
}

/**
 * tcp_tx_send - transmit a buffer on a TCP connection
 * @c: the TCP connection
 * @buf: the buffer to transmit
 * @len: the length of the buffer to transmit
 * @push: indicates the data is ready for consumption by the receiver
 *
 * If @push is false, the implementation may buffer some or all of the data for
 * future transmission.
 *
 * WARNING: The caller is responsible for respecting the TCP window size limit.
 * WARNING: The caller must have write exclusive access to the socket or hold
 * @c->lock while write exclusion isn't taken.
 *
 * Returns the number of bytes transmitted, or < 0 if there was an error.
 */
ssize_t tcp_tx_send(tcpconn_t *c, const void *buf, size_t len, bool push)
{
	struct mbuf *m;
	const char *pos = buf;
	const char *end = pos + len;
	ssize_t ret = 0;
	size_t seglen;
	uint32_t mss = c->pcb.snd_mss;

	assert(c->pcb.state >= TCP_STATE_ESTABLISHED);
	assert((c->tx_exclusive == true) || spin_lock_held(&c->lock));

	pos = buf;
	end = pos + len;

	/* the main TCP segmenter loop */
	do {
		/* allocate a buffer and copy payload data */
		if (c->tx_pending) {
			m = c->tx_pending;
			c->tx_pending = NULL;
			seglen = MIN(end - pos, mss - mbuf_length(m));
			m->seg_end += seglen;
		} else {
			m = net_tx_alloc_mbuf();
			if (unlikely(!m)) {
				ret = -ENOBUFS;
				break;
			}
			seglen = MIN(end - pos, mss);
			m->seg_seq = c->pcb.snd_nxt;
			m->seg_end = c->pcb.snd_nxt + seglen;
			m->flags = TCP_ACK;
			atomic_write(&m->ref, 2);
			m->release = tcp_tx_release_mbuf;
		}

		memcpy(mbuf_put(m, seglen), pos, seglen);
		store_release(&c->pcb.snd_nxt, c->pcb.snd_nxt + seglen);
		pos += seglen;

		/* if not pushing, keep the last buffer for later */
		if (!push && pos == end && mbuf_length(m) -
		    sizeof(struct tcp_hdr) < mss) {
			c->tx_pending = m;
			break;
		}

		/* initialize TCP header */
		if (push && pos == end)
			m->flags |= TCP_PUSH;
		tcp_push_tcphdr(m, c, m->flags, 5, m->seg_end - m->seg_seq);

		/* transmit the packet */
		list_add_tail(&c->txq, &m->link);
		tcp_debug_egress_pkt(c, m);
		m->timestamp = microtime();
		m->txflags = OLFLAG_TCP_CHKSUM;
		ret = net_tx_ip(m, IPPROTO_TCP, c->e.raddr.ip);
		if (unlikely(ret)) {
			/* pretend the packet was sent */
			atomic_write(&m->ref, 1);
		}
	} while (pos < end);

	/* if we sent anything return the length we sent instead of an error */
	if (pos - (const char *)buf > 0)
		ret = pos - (const char *)buf;
	return ret;
}

static int tcp_tx_retransmit_one(tcpconn_t *c, struct mbuf *m)
{
	int ret;
	uint8_t opts_len;
	uint16_t l4len;

	l4len = m->seg_end - m->seg_seq;
	if (m->flags & (TCP_SYN | TCP_FIN))
		l4len--;

	opts_len = ((struct tcp_hdr *)mbuf_transport_offset(m))->off - 5;

	/*
	 * Check if still transmitting. Because of a limitation in some DPDK NIC
	 * drivers, completions could be delayed long after transmission is
	 * finished. We copy the packet to allow retransmission to still succeed
	 * in such corner cases.
	 */
	if (unlikely(atomic_read(&m->ref) != 1)) {
		struct mbuf *newm = net_tx_alloc_mbuf();
		if (unlikely(!newm))
			return -ENOMEM;
		memcpy(mbuf_put(newm, sizeof(uint32_t) * opts_len + l4len),
		       mbuf_transport_offset(m) + sizeof(struct tcp_hdr),
		       sizeof(uint32_t) * opts_len + l4len);
		newm->flags = m->flags;
		newm->seg_seq = m->seg_seq;
		newm->seg_end = m->seg_end;
		newm->txflags = OLFLAG_TCP_CHKSUM;
		m = newm;
	} else {
		/* strip headers and reset ref count */
		mbuf_reset(m, m->transport_off + sizeof(struct tcp_hdr));
		atomic_write(&m->ref, 2);
	}

	/* handle a partially acknowledged packet */
	uint32_t una = load_acquire(&c->pcb.snd_una);
	if (unlikely(wraps_lte(m->seg_end, una))) {
		mbuf_free(m);
		return 0;
	} else if (unlikely(wraps_lt(m->seg_seq, una))) {
		mbuf_pull(m, una - m->seg_seq);
		m->seg_seq = una;
	}

	/* push the TCP header back on (now with fresher ack) */
	tcp_push_tcphdr(m, c, m->flags, 5 + opts_len, l4len);

	/* transmit the packet */
	tcp_debug_egress_pkt(c, m);
	ret = net_tx_ip(m, IPPROTO_TCP, c->e.raddr.ip);
	if (unlikely(ret))
		mbuf_free(m);
	return ret;
}

/**
 * tcp_tx_fast_retransmit - resend the first pending egress packet
 * @c: the TCP connection in which to send retransmissions
 */
struct mbuf *tcp_tx_fast_retransmit_start(tcpconn_t *c)
{
	struct mbuf *m;

	assert_spin_lock_held(&c->lock);

	if (c->tx_exclusive)
		return NULL;

	m = list_top(&c->txq, struct mbuf, link);
	if (m) {
		m->timestamp = microtime();
		atomic_inc(&m->ref);
	}

	return m;
}

void tcp_tx_fast_retransmit_finish(tcpconn_t *c, struct mbuf *m)
{
	if (m) {
		tcp_tx_retransmit_one(c, m);
		mbuf_free(m);
	}
}

/**
 * tcp_tx_retransmit - resend any pending egress packets that timed out
 * @c: the TCP connection in which to send retransmissions
 */
void tcp_tx_retransmit(tcpconn_t *c)
{
	struct mbuf *m;
	uint64_t now = microtime();

	assert(spin_lock_held(&c->lock) || c->tx_exclusive);

	int ret;

	int count = 0;
	list_for_each(&c->txq, m, link) {
		/* check if the timeout expired */
		if (now - m->timestamp < TCP_RETRANSMIT_TIMEOUT)
			break;

		if (wraps_gte(load_acquire(&c->pcb.snd_una), m->seg_end))
			continue;

		m->timestamp = now;
		ret = tcp_tx_retransmit_one(c, m);
		if (ret)
			break;

		if (++count >= TCP_RETRANSMIT_BATCH)
			break;
	}
}
