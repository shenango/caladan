/*
 * util.h - utility functions for RPC
 */

#pragma once

#include <base/stddef.h>
#include <runtime/tcp.h>

extern ssize_t tcp_read_full(tcpconn_t *c, void *buf, size_t len);
extern ssize_t tcp_write_full(tcpconn_t *c, const void *buf, size_t len);
extern ssize_t tcp_writev_full(tcpconn_t *c, struct iovec *iov, int iovcnt);
