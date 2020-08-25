/*
 * util.c - utility functions for RPC
 */

#include "util.h"

/**
 * tcp_read_full - reads exactly the requested bytes or fails
 * @c: the TCP connection to read from
 * @buf: the buffer to store the read
 * @len: the exact length of the read
 *
 * Returns @len bytes or <= 0 if there was an error.
 */
ssize_t tcp_read_full(tcpconn_t *c, void *buf, size_t len)
{
	char *pos = buf;
	size_t n = 0;

	while (n < len) {
		ssize_t ret = tcp_read(c, pos + n, len - n);
		if (ret <= 0)
			return ret;
		n += ret;
	}

	assert(n == len);
	return n;
}

/**
 * tcp_write_full - writes exactly the requested bytes or fails
 * @c: the TCP connection to write to
 * @buf: the buffer to write to the socket
 * @len: the exact length of the write
 *
 * Returns @len bytes or < 0 if there was an error.
 */
ssize_t tcp_write_full(tcpconn_t *c, const void *buf, size_t len)
{
	const char *pos = buf;
	size_t n = 0;

	while (n < len) {
		ssize_t ret = tcp_write(c, pos + n, len - n);
		if (ret < 0)
			return ret;
		assert(ret > 0);
		n += ret;
	}

	assert(n == len);
	return n;
}

static bool pull_iov(struct iovec **iovp, int *iovcntp, size_t n)
{
	struct iovec *iov = *iovp;
	int iovcnt = *iovcntp, i;

	for (i = 0; i < iovcnt; i++) {
		if (n < iov[i].iov_len) {
			iov[i].iov_base = (char *)iov[i].iov_base + n;
			iov[i].iov_len -= n;
			*iovp = &iov[i];
			*iovcntp -= i;
			return true;
		}
		n -= iov[i].iov_len;
	}

	assert (n == 0);
	return false;
}

/**
 * tcp_writev_full - writes exactly the requested vector of bytes or fails
 * @c: the TCP connection to write to
 * @iov: the scatter-gather array of buffers to write
 * @iovcnt: the number of entries in @iov
 *
 * WARNING: @iov could be modified by this function, and its state is undefined
 * after calling it.
 *
 * Returns the number of written bytes or < 0 if there was an error.
 */
ssize_t tcp_writev_full(tcpconn_t *c, struct iovec *iov, int iovcnt)
{
	ssize_t n, len = 0;

	do {
		n = tcp_writev(c, iov, iovcnt);
		if (n < 0)
			return n;
		assert(n > 0);
		len += n;
	} while (pull_iov(&iov, &iovcnt, n));

	return len;
}
