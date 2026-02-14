/*
 * test_tcp_read.c - exhaustive edge-case tests for tcp_read2 and tcp_readv2
 * (vectorized and non-vectorized, with and without peek).
 * Round 0: one write per case (single packet → one mbuf on RX).
 * Round 1: same cases with payload sent in two writes to stress multi-mbuf paths.
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <base/log.h>
#include <base/assert.h>
#include <base/time.h>
#include <runtime/runtime.h>
#include <runtime/sync.h>
#include <runtime/tcp.h>
#include <runtime/net.h>

#define PAYLOAD_SIZE		10
#define SMALL_BUF_SIZE		64
#define LONG_PAYLOAD_SIZE	(20 * 1024)
#define WAIT_RX_TIMEOUT_US	(5 * ONE_SECOND)

static void tcp_write_all(tcpconn_t *c, const void *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = tcp_write(c, (const char *)buf + off, len - off);
		if (n < 0) {
			log_err("tcp_write failed: %zd", n);
			BUG();
		}
		BUG_ON(n == 0);
		off += (size_t)n;
	}
}

enum send_mode {
	SEND_ONE_WRITE = 0,
	SEND_TWO_WRITES,
	SEND_RANDOM_SEGMENTS,
};

/* After consuming all RX data: no buffered bytes and nonblocking read returns EAGAIN */
static void assert_no_pending_input(tcpconn_t *c)
{
	char scratch;
	struct iovec scratch_iov = { .iov_base = &scratch, .iov_len = 1 };
	ssize_t n;

	BUG_ON_NE(tcp_get_input_bytes(c), 0);
	n = tcp_read2(c, &scratch, 1, false, true);
	BUG_ON_NE(n, -EAGAIN);
	n = tcp_readv2(c, &scratch_iov, 1, false, true);
	BUG_ON_NE(n, -EAGAIN);
}

/* Wait until at least @expected bytes are available. */
static void wait_for_input_bytes(tcpconn_t *c, uint32_t expected)
{
	uint64_t start = microtime();

	while (tcp_get_input_bytes(c) < expected) {
		if (microtime() - start > WAIT_RX_TIMEOUT_US) {
			log_err("timeout waiting for RX bytes (have=%u need=%u)",
			        tcp_get_input_bytes(c), expected);
			BUG();
		}
		thread_yield();
	}
}

static void fill_payload(char *buf, size_t len, char pattern)
{
	size_t i;
	for (i = 0; i < len; i++)
		buf[i] = (char)(pattern + (int)i);
}

static void fill_payload_offset(char *buf, size_t len, char pattern, size_t off)
{
	size_t i;
	for (i = 0; i < len; i++)
		buf[i] = (char)(pattern + (int)(off + i));
}

static void assert_payload_eq_offset(const char *buf, size_t len, char pattern, size_t off)
{
	for (size_t i = 0; i < len; i++)
		BUG_ON_NE(buf[i], (char)(pattern + (int)(off + i)));
}

static void assert_payload_eq(const char *buf, size_t len)
{
	assert_payload_eq_offset(buf, len, 'A', 0);
}

static uint32_t xorshift32(uint32_t *state)
{
	/* Deterministic, cheap PRNG (not crypto). */
	uint32_t x = *state;
	if (x == 0)
		x = 0xdeadbeefu;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static size_t rand_range(uint32_t *state, size_t min_incl, size_t max_incl)
{
	BUG_ON(min_incl > max_incl);
	if (min_incl == max_incl)
		return min_incl;
	return min_incl + (xorshift32(state) % (max_incl - min_incl + 1));
}

struct tcp_pair {
	tcpconn_t *tx; /* "server" side (writer) */
	tcpconn_t *rx; /* "client" side (reader) */
};

static struct tcp_pair tcp_pair_open(void)
{
	tcpqueue_t *q;
	tcpconn_t *rx, *tx;
	bind_token_t *token;
	struct netaddr listen_laddr = { .ip = 0, .port = 0 };
	struct netaddr raddr;
	int ret;

	ret = tcp_reserve_port(&listen_laddr, false, &token);
	if (ret) {
		log_err("tcp_reserve_port failed: %d", ret);
		BUG();
	}

	/* If listen succeeds, @token is consumed and no longer ours (see tcp.h). */
	ret = __tcp_listen(listen_laddr, 1, &q, token);
	if (ret) {
		log_err("__tcp_listen failed: %d", ret);
		tcp_release_port(token);
		BUG();
	}
	token = NULL;

	ret = str_to_netaddr("127.0.0.1", &raddr);
	if (ret) {
		log_err("str_to_netaddr failed: %d", ret);
		BUG();
	}
	raddr.port = tcpq_local_addr(q).port;

	{
		struct netaddr laddr = { .ip = 0, .port = 0 };
		ret = tcp_dial(laddr, raddr, &rx);
		if (ret) {
			log_err("tcp_dial failed: %d", ret);
			BUG();
		}
	}

	ret = tcp_accept(q, &tx);
	if (ret) {
		log_err("tcp_accept failed: %d", ret);
		BUG();
	}

	tcp_qclose(q);
	return (struct tcp_pair){
		.tx = tx,
		.rx = rx,
	};
}

static void tcp_pair_close(struct tcp_pair *p)
{
	if (p->rx)
		tcp_close(p->rx);
	if (p->tx)
		tcp_close(p->tx);
	p->rx = NULL;
	p->tx = NULL;
}

static void send_payload(tcpconn_t *tx, tcpconn_t *rx, size_t len, bool split)
{
	char payload[SMALL_BUF_SIZE];

	BUG_ON(len > SMALL_BUF_SIZE);
	fill_payload(payload, len, 'A');

	if (split && len > 1) {
		size_t first = len / 2;
		tcp_write_all(tx, payload, first);
		tcp_write_all(tx, payload + first, len - first);
	} else {
		tcp_write_all(tx, payload, len);
	}

	wait_for_input_bytes(rx, len);
}

static void send_payload_mode(tcpconn_t *tx, tcpconn_t *rx, size_t len,
                              enum send_mode mode, uint32_t *rng)
{
	if (mode == SEND_ONE_WRITE) {
		send_payload(tx, rx, len, false);
		return;
	}

	if (mode == SEND_TWO_WRITES) {
		send_payload(tx, rx, len, true);
		return;
	}

	/* Random segmentation: multiple small writes to create multiple mbufs. */
	BUG_ON(mode != SEND_RANDOM_SEGMENTS);
	BUG_ON(!rng);

	size_t off = 0;
	char tmp[2048];
	const size_t max_seg = (len < sizeof(tmp)) ? len : sizeof(tmp);

	while (off < len) {
		size_t seg = rand_range(rng, 1, (len - off < max_seg) ? (len - off) : max_seg);

		fill_payload_offset(tmp, seg, 'A', off);
		tcp_write_all(tx, tmp, seg);
		off += seg;
	}

	wait_for_input_bytes(rx, len);
}

static void read_full(tcpconn_t *c, char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = tcp_read2(c, buf + off, len - off, false, false);
		BUG_ON(n <= 0);
		off += (size_t)n;
	}
}

static void test_read2_exact_no_peek(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, PAYLOAD_SIZE, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static void test_readv2_exact_no_peek(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	struct iovec iov[1];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	iov[0].iov_base = buf;
	iov[0].iov_len = PAYLOAD_SIZE;
	n = tcp_readv2(p->rx, iov, 1, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static void test_peek_then_read(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, PAYLOAD_SIZE, true, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, PAYLOAD_SIZE, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static void test_read5_then_read5(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);

	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, 5, false, false);
	BUG_ON_NE(n, 5);
	assert_payload_eq_offset(buf, 5, 'A', 0);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, 5, false, false);
	BUG_ON_NE(n, 5);
	assert_payload_eq_offset(buf, 5, 'A', 5);
	assert_no_pending_input(p->rx);
}

static void test_read20_more_than_available(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, 20, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static void test_peek5_then_read10(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);

	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, 5, true, false);
	BUG_ON_NE(n, 5);
	assert_payload_eq_offset(buf, 5, 'A', 0);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, PAYLOAD_SIZE, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static void test_readv_3_3_4(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	struct iovec iov[3];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	iov[0].iov_base = buf;
	iov[0].iov_len = 3;
	iov[1].iov_base = buf + 3;
	iov[1].iov_len = 3;
	iov[2].iov_base = buf + 6;
	iov[2].iov_len = 4;
	n = tcp_readv2(p->rx, iov, 3, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static void test_peek_twice_then_read(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, PAYLOAD_SIZE, true, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, PAYLOAD_SIZE, true, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, PAYLOAD_SIZE, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static void test_read1_seven_times_then_read3(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	for (int i = 0; i < 7; i++) {
		n = tcp_read2(p->rx, buf + i, 1, false, false);
		BUG_ON_NE(n, 1);
		BUG_ON_NE(buf[i], (char)('A' + i));
	}
	n = tcp_read2(p->rx, buf + 7, 3, false, false);
	BUG_ON_NE(n, 3);
	BUG_ON_NE(memcmp(buf + 7, "HIJ", 3), 0);
	assert_no_pending_input(p->rx);
}

static void test_single_byte_read(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, 1, mode, rng);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, 1, false, false);
	BUG_ON_NE(n, 1);
	BUG_ON_NE(buf[0], 'A');
	assert_no_pending_input(p->rx);
}

static void test_readv10_peek_then_read(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	struct iovec iov[1];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	iov[0].iov_base = buf;
	iov[0].iov_len = PAYLOAD_SIZE;
	n = tcp_readv2(p->rx, iov, 1, true, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	memset(buf, 0, sizeof(buf));
	n = tcp_readv2(p->rx, iov, 1, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static void test_readv_5_5_no_peek(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	struct iovec iov[2];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	iov[0].iov_base = buf;
	iov[0].iov_len = 5;
	iov[1].iov_base = buf + 5;
	iov[1].iov_len = 5;
	n = tcp_readv2(p->rx, iov, 2, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static void test_read0_then_read10(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, 0, false, false);
	BUG_ON_NE(n, 0);
	BUG_ON_NE(tcp_get_input_bytes(p->rx), PAYLOAD_SIZE);
	n = tcp_read2(p->rx, buf, PAYLOAD_SIZE, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static void test_read7_then_read3(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, 7, false, false);
	BUG_ON_NE(n, 7);
	assert_payload_eq_offset(buf, 7, 'A', 0);
	memset(buf, 0, sizeof(buf));
	n = tcp_read2(p->rx, buf, 3, false, false);
	BUG_ON_NE(n, 3);
	assert_payload_eq_offset(buf, 3, 'A', 7);
	assert_no_pending_input(p->rx);
}

static void test_readv_3_4_get7_then_read3(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	struct iovec iov[2];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);

	memset(buf, 0, sizeof(buf));
	iov[0].iov_base = buf;
	iov[0].iov_len = 3;
	iov[1].iov_base = buf + 3;
	iov[1].iov_len = 4;
	n = tcp_readv2(p->rx, iov, 2, false, false);
	BUG_ON_NE(n, 7);
	assert_payload_eq_offset(buf, 7, 'A', 0);
	n = tcp_read2(p->rx, buf + 7, 3, false, false);
	BUG_ON_NE(n, 3);
	assert_payload_eq_offset(buf + 7, 3, 'A', 7);
	assert_no_pending_input(p->rx);
}

static void test_readv_zero_len_iov(struct tcp_pair *p, enum send_mode mode, uint32_t *rng)
{
	char buf[SMALL_BUF_SIZE];
	struct iovec iov[3];
	ssize_t n;

	send_payload_mode(p->tx, p->rx, PAYLOAD_SIZE, mode, rng);

	memset(buf, 0, sizeof(buf));
	iov[0].iov_base = buf;
	iov[0].iov_len = 0;
	iov[1].iov_base = buf;
	iov[1].iov_len = 5;
	iov[2].iov_base = buf + 5;
	iov[2].iov_len = 5;

	n = tcp_readv2(p->rx, iov, 3, false, false);
	BUG_ON_NE(n, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);
	assert_no_pending_input(p->rx);
}

static const char *send_mode_name(enum send_mode mode)
{
	switch (mode) {
	case SEND_ONE_WRITE:
		return "one write";
	case SEND_TWO_WRITES:
		return "two writes (split)";
	case SEND_RANDOM_SEGMENTS:
		return "random segments";
	default:
		return "unknown";
	}
}

static void run_round_small(struct tcp_pair *p, enum send_mode mode, uint32_t seed)
{
	uint32_t rng = seed;
	log_info("tcp_read (small): mode=%s seed=0x%x", send_mode_name(mode), seed);

	test_read2_exact_no_peek(p, mode, &rng);
	test_readv2_exact_no_peek(p, mode, &rng);
	test_peek_then_read(p, mode, &rng);
	test_read5_then_read5(p, mode, &rng);
	test_read20_more_than_available(p, mode, &rng);
	test_peek5_then_read10(p, mode, &rng);
	test_readv_3_3_4(p, mode, &rng);
	test_peek_twice_then_read(p, mode, &rng);
	test_read1_seven_times_then_read3(p, mode, &rng);
	test_single_byte_read(p, mode, &rng);
	test_readv10_peek_then_read(p, mode, &rng);
	test_readv_5_5_no_peek(p, mode, &rng);
	test_read0_then_read10(p, mode, &rng);
	test_read7_then_read3(p, mode, &rng);
	test_readv_3_4_get7_then_read3(p, mode, &rng);
	test_readv_zero_len_iov(p, mode, &rng);
}

static void test_long_random_segments_read_full(struct tcp_pair *p, size_t len, uint32_t seed)
{
	uint32_t rng = seed;
	char *buf;

	log_info("tcp_read (long): len=%zu mode=%s seed=0x%x",
	         len, send_mode_name(SEND_RANDOM_SEGMENTS), seed);

	buf = malloc(len);
	BUG_ON(!buf);

	send_payload_mode(p->tx, p->rx, len, SEND_RANDOM_SEGMENTS, &rng);
	read_full(p->rx, buf, len);
	assert_payload_eq_offset(buf, len, 'A', 0);
	assert_no_pending_input(p->rx);

	free(buf);
}

static void test_long_random_segments_peek_prefix_then_read_full(struct tcp_pair *p, size_t len,
                                                                size_t peek_len, uint32_t seed)
{
	uint32_t rng = seed;
	char *buf;
	ssize_t n;

	BUG_ON(peek_len > len);
	log_info("tcp_read (long): len=%zu peek=%zu mode=%s seed=0x%x",
	         len, peek_len, send_mode_name(SEND_RANDOM_SEGMENTS), seed);

	buf = malloc(len);
	BUG_ON(!buf);

	send_payload_mode(p->tx, p->rx, len, SEND_RANDOM_SEGMENTS, &rng);

	n = tcp_read2(p->rx, buf, peek_len, true, false);
	BUG_ON_NE(n, (ssize_t)peek_len);
	assert_payload_eq_offset(buf, peek_len, 'A', 0);

	read_full(p->rx, buf, len);
	assert_payload_eq_offset(buf, len, 'A', 0);
	assert_no_pending_input(p->rx);

	free(buf);
}

static void test_eof_after_peer_shutdown(void)
{
	struct tcp_pair p = tcp_pair_open();
	char buf[SMALL_BUF_SIZE];
	char tmp;
	ssize_t n;
	int ret;
	uint64_t start;

	/* Send data, then half-close the writer; reader should eventually observe EOF. */
	send_payload_mode(p.tx, p.rx, PAYLOAD_SIZE, SEND_ONE_WRITE, NULL);
	ret = tcp_shutdown(p.tx, SHUT_WR);
	BUG_ON(ret);

	memset(buf, 0, sizeof(buf));
	read_full(p.rx, buf, PAYLOAD_SIZE);
	assert_payload_eq(buf, PAYLOAD_SIZE);

	/* FIN arrival is asynchronous; poll with a bounded timeout. */
	start = microtime();
	while (true) {
		n = tcp_read2(p.rx, &tmp, 1, false, true);
		if (n == 0)
			break;
		if (n == -EAGAIN) {
			if (microtime() - start > WAIT_RX_TIMEOUT_US) {
				log_err("timeout waiting for EOF after peer shutdown");
				BUG();
			}
			thread_yield();
			continue;
		}
		log_err("unexpected read after shutdown: %zd", n);
		BUG();
	}

	/* Once EOF is observed, subsequent reads should keep returning 0. */
	n = tcp_read2(p.rx, &tmp, 1, false, true);
	BUG_ON_NE(n, 0);

	tcp_pair_close(&p);
}

static void main_handler(void *arg)
{
	(void)arg;

	struct tcp_pair p = tcp_pair_open();
	run_round_small(&p, SEND_ONE_WRITE, 0x12345678u);
	run_round_small(&p, SEND_TWO_WRITES, 0x12345678u);
	run_round_small(&p, SEND_RANDOM_SEGMENTS, 0x12345678u);

	test_long_random_segments_read_full(&p, LONG_PAYLOAD_SIZE, 0xabcdef01u);
	test_long_random_segments_peek_prefix_then_read_full(&p, LONG_PAYLOAD_SIZE, 256, 0xabcdef02u);
	tcp_pair_close(&p);

	test_eof_after_peer_shutdown();

	log_info("tcp_read tests passed");
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc < 2) {
		printf("arg must be config file\n");
		return -EINVAL;
	}
	ret = runtime_init(argv[1], main_handler, NULL);
	if (ret) {
		log_err("runtime_init failed: %d", ret);
		return ret;
	}
	return 0;
}
