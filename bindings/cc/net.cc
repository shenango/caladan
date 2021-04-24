#include "net.h"

#include <cstring>
#include <memory>
#include <algorithm>

namespace {

bool PullIOV(struct iovec **iovp, int *iovcntp, size_t n) {
  struct iovec *iov = *iovp;
  int iovcnt = *iovcntp, i;

  for (i = 0; i < iovcnt; ++i) {
    if (n < iov[i].iov_len) {
      iov[i].iov_base = reinterpret_cast<char *>(iov[i].iov_base) + n;
      iov[i].iov_len -= n;
      *iovp = &iov[i];
      *iovcntp -= i;
      return true;
    }
    n -= iov[i].iov_len;
  }

  assert(n == 0);
  return false;
}

size_t SumIOV(const iovec *iov, int iovcnt) {
  size_t len = 0;
  for (int i = 0; i < iovcnt; ++i) len += iov[i].iov_len;
  return len;
}

}  // namespace

namespace rt {

ssize_t TcpConn::WritevFullRaw(const iovec *iov, int iovcnt) {
  // first try to send without copying the vector
  ssize_t n = tcp_writev(c_, iov, iovcnt);
  if (n < 0) return n;
  assert(n > 0);

  // sum total length and check if everything was transfered
  size_t total = SumIOV(iov, iovcnt);
  if (static_cast<size_t>(n) == total) return n;

  // partial transfer occurred, send the rest
  size_t len = n;
  std::unique_ptr<iovec[]> v = std::unique_ptr<iovec[]>{new iovec[iovcnt]};
  iovec *iovp = v.get();
  std::copy_n(iov, iovcnt, iovp);
  while (PullIOV(&iovp, &iovcnt, n)) {
    n = tcp_writev(c_, iovp, iovcnt);
    if (n < 0) return n;
    assert(n > 0);
    len += n;
  }

  assert(len == total);
  return len;
}

ssize_t TcpConn::ReadvFullRaw(const iovec *iov, int iovcnt) {
  // first try to receive without copying the vector
  ssize_t n = tcp_readv(c_, iov, iovcnt);
  if (n <= 0) return n;

  // sum total length and check if everything was transfered
  size_t total = SumIOV(iov, iovcnt);
  if (static_cast<size_t>(n) == total) return n;

  // partial transfer occurred, receive the rest
  size_t len = n;
  std::unique_ptr<iovec[]> v = std::unique_ptr<iovec[]>{new iovec[iovcnt]};
  iovec *iovp = v.get();
  std::copy_n(iov, iovcnt, iovp);
  while (PullIOV(&iovp, &iovcnt, n)) {
    n = tcp_readv(c_, iovp, iovcnt);
    if (n <= 0) return n;
    len += n;
  }

  assert(len == total);
  return len;
}

}  // namespace rt
