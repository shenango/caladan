#include "net.h"

#include <cstring>
#include <memory>

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

}  // namespace

namespace rt {

ssize_t TcpConn::WritevFullRaw(const iovec *iov, int iovcnt) {
  // first try to send without copying the vector
  ssize_t n = tcp_writev(c_, iov, iovcnt);
  if (n < 0) return n;
  assert(n > 0);

  // sum total length and check if everything was transfered
  size_t len = 0;
  for (int i = 0; i < iovcnt; ++i) len += iov[i].iov_len;
  if (static_cast<size_t>(n) == len) return len;

  // partial transfer occurred, send the rest
  len = n;
  std::unique_ptr<iovec[]> v = std::unique_ptr<iovec[]>{new iovec[iovcnt]};
  iovec *iovp = v.get();
  memcpy(iovp, iov, sizeof(iovec) * iovcnt);
  while (PullIOV(&iovp, &iovcnt, n)) {
    n = tcp_writev(c_, iovp, iovcnt);
    if (n < 0) return n;
    assert(n > 0);
    len += n;
  }

  return len;
}

}  // namespace rt
