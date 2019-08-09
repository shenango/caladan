extern "C" {
#include <base/byteorder.h>
#include <base/hash.h>
#include <base/log.h>
#include <net/ip.h>
#include <runtime/runtime.h>
#include <runtime/smalloc.h>
#include <runtime/storage.h>
}

#include "net.h"
#include "sync.h"
#include "thread.h"

#include <memory>
#include <vector>

#include "RpcManager.h"

#define MAX_REQUEST_BODY 1024

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

class RequestContext {
 public:
  RequestContext(std::shared_ptr<SharedTcpStream> c) : conn(c) {
    r.req_body = request_body;
    r.rsp_body = request_body;
    r.rsp_body_len = sizeof(request_body);
  }
  Rpc<MemcachedHdr> r;
  unsigned char request_body[MAX_REQUEST_BODY];
  std::shared_ptr<SharedTcpStream> conn;
  void *operator new(size_t size) {
    void *p = smalloc(size);
    if (unlikely(p == nullptr)) throw std::bad_alloc();
    return p;
  }
  void operator delete(void *p) { sfree(p); }
};

inline unsigned int route(unsigned char *key, size_t keylen) {
  return jenkins_hash(key, keylen) % memcached_endpoints.size();
}

void HandleRequest(RequestContext *ctx) {
  unsigned char *key = ctx->request_body + ctx->r.req.extras_length;
  int server_idx = route(key, ntoh16(ctx->r.req.key_length));

  memcached_endpoints[server_idx]->SubmitRequestBlocking(&ctx->r);

  struct iovec out_vec[2];
  out_vec[0].iov_base = &ctx->r.rsp;
  out_vec[0].iov_len = sizeof(ctx->r.rsp);
  out_vec[1].iov_base = ctx->request_body;
  out_vec[1].iov_len = ntoh32(ctx->r.rsp.total_body_length);

  ssize_t wret = ctx->conn->WritevFull(out_vec, 2);
  if (wret != static_cast<ssize_t>(sizeof(ctx->r.rsp) + out_vec[1].iov_len)) {
    BUG_ON(wret != -EPIPE && wret != -ECONNRESET);
  }
}

void ClientHandler(std::shared_ptr<rt::TcpConn> conn) {
  auto resp = std::make_shared<SharedTcpStream>(conn);

  while (true) {
    auto ctx = new RequestContext(resp);
    MemcachedHdr *h = &ctx->r.req;

    ssize_t rret = conn->ReadFull(h, sizeof(*h));
    if (rret != static_cast<ssize_t>(sizeof(*h))) {
      BUG_ON(rret != 0 && rret != -ECONNRESET);
      delete ctx;
      return;
    }

    ctx->r.req_body_len = ntoh32(h->total_body_length);
    BUG_ON(!ctx->r.req_body_len);

    if (ctx->r.req_body_len > sizeof(ctx->request_body)) {
      log_err("request too large, destroying connection");
      delete ctx;
      return;
    }

    rret = conn->ReadFull(ctx->request_body, ctx->r.req_body_len);
    if (rret != static_cast<ssize_t>(ctx->r.req_body_len)) {
      BUG_ON(rret != 0 && rret != -ECONNRESET);
      delete ctx;
      return;
    }

    rt::Thread([=] {
      HandleRequest(ctx);
      delete ctx;
    })
        .Detach();
  }
}

void ServerHandler(void *arg) {
  /* dial memcached connections */
  for (auto &w : memcached_addrs) {
    auto ep = RpcEndpoint<MemcachedHdr>::Create(w);
    if (ep == nullptr) BUG();
    memcached_endpoints.push_back(ep);
  }

  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen(listenaddr, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
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
  if (ret) printf("failed to start runtime\n");

  return ret;
}
