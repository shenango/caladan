extern "C" {
#include <base/byteorder.h>
#include <base/hash.h>
#include <base/log.h>
#include <net/ip.h>
#include <runtime/runtime.h>
#include <runtime/storage.h>
#include <stdint.h>
}

#include "net.h"
#include "sync.h"
#include "thread.h"

#include <memory>
#include <vector>

#include "RpcManager.h"

static netaddr listenaddr;
static std::vector<netaddr> memcached_addrs;

static std::vector<RpcEndpoint<MemcachedHdr> *> memcached_endpoints;

static int StringToAddr(const char *str, netaddr *addr) {
  uint8_t a, b, c, d;
  uint16_t p;

  if (sscanf(str, "%hhu.%hhu.%hhu.%hhu:%hu", &a, &b, &c, &d, &p) != 5)
    return -EINVAL;

  addr->ip = MAKE_IP_ADDR(a, b, c, d);
  addr->port = p;
  return 0;
}

int setup() {
  for (auto &w : memcached_addrs) {
    auto ep = RpcEndpoint<MemcachedHdr>::Create(w);
    if (ep == nullptr)
      BUG();
    memcached_endpoints.push_back(ep);
  }
  return 0;
}

inline unsigned int route(unsigned char *key, size_t keylen) {
  return jenkins_hash(key, keylen) % memcached_endpoints.size();
}

void ClientHandler(std::shared_ptr<rt::TcpConn> conn) {
  unsigned char buf[4096];
  Rpc<MemcachedHdr> r;
  r.req_body = buf;
  r.rsp_body = buf;
  r.rsp_body_len = sizeof(buf);

  struct iovec out_vec[2];
  out_vec[0].iov_base = &r.rsp;
  out_vec[0].iov_len = sizeof(r.rsp);
  out_vec[1].iov_base = buf;

  while (true) {

    ssize_t rret = conn->ReadFull(&r.req, sizeof(r.req));
    if (rret != static_cast<ssize_t>(sizeof(r.req))) {
      if (rret == 0 || rret == -ECONNRESET)
        return;
      BUG();
    }

    r.req_body_len = ntoh32(r.req.total_body_length);
    BUG_ON(!r.req_body_len);

    rret = conn->ReadFull(buf, r.req_body_len);
    if (rret != static_cast<ssize_t>(r.req_body_len)) {
      if (rret == 0 || rret == -ECONNRESET)
        return;
      log_err("rret %ld", rret);
      BUG();
    }

    unsigned char *key_start = buf + r.req.extras_length;
    int server_idx = route(key_start, ntoh16(r.req.key_length));

    memcached_endpoints[server_idx]->SubmitRequestBlocking(&r);
    out_vec[1].iov_len = ntoh32(r.rsp.total_body_length);

    ssize_t wret =
        WritevFull(conn.get(), out_vec, r.rsp.total_body_length ? 2 : 1);
    if (wret != static_cast<ssize_t>(sizeof(r.rsp) + out_vec[1].iov_len)) {
      if (wret == -EPIPE || wret == -ECONNRESET)
        return;
      log_err("wret %ld", wret);
      BUG();
    }
  }
}

void ServerHandler(void *arg) {
  setup();

  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen(listenaddr, 4096));
  if (q == nullptr)
    panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr)
      panic("couldn't accept a connection");
    rt::Thread([=] { ClientHandler(std::shared_ptr<rt::TcpConn>(c)); })
        .Detach();
  }
}

int main(int argc, char *argv[]) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s [cfg] listenaddr memcached_addrs... \n",
            argv[0]);
    return -EINVAL;
  }

  if (StringToAddr(argv[2], &listenaddr)) {
    printf("failed to parse addr %s\n", argv[2]);
    return -EINVAL;
  }

  for (int i = 3; i < argc; i++) {
    netaddr addr;
    if (StringToAddr(argv[i], &addr)) {
      printf("failed to parse addr %s\n", argv[i]);
      return -EINVAL;
    }
    // Duplicate the servers for now...
    for (int j = 0; j < 64; j++) {
      memcached_addrs.push_back(addr);
    }
  }

  int ret = runtime_init(argv[1], ServerHandler, NULL);
  if (ret)
    printf("failed to start runtime\n");

  return ret;
}
