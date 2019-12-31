/*
 * util.h - utility functions for RPC
 */

#pragma once

#include <base/stddef.h>
#include <runtime/tcp.h>

static ssize_t tcp_read_full(tcpconn_t *c, void *buf, size_t len)
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

static ssize_t tcp_write_full(tcpconn_t *c, const void *buf, size_t len)
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
