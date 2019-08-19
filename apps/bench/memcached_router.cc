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

static bool use_pooled_connections;
static bool use_affinity_dial;

class MemcachedRequestContext : public RequestContext {
 public:
  MemcachedRequestContext(std::shared_ptr<SharedTcpStream> c)
      : RequestContext(c) {
    r.req_body = request_body;
    r.rsp_body = request_body;
    r.rsp_body_len = sizeof(request_body);
  }
  Rpc<MemcachedHdr> r;
  unsigned char request_body[MAX_REQUEST_BODY];
};

static inline unsigned int route(unsigned char *key, size_t keylen) {
  return jenkins_hash(key, keylen) % memcached_addrs.size();
}

void HandleRequest(MemcachedRequestContext *ctx) {
  unsigned char *key = ctx->request_body + ctx->r.req.extras_length;
  int server_idx = route(key, ntoh16(ctx->r.req.key_length));

  memcached_endpoints[server_idx]->SubmitRequestBlocking(&ctx->r);

  struct iovec out_vec[2];
  out_vec[0].iov_base = &ctx->r.rsp;
  out_vec[0].iov_len = sizeof(ctx->r.rsp);
  out_vec[1].iov_base = ctx->request_body;
  out_vec[1].iov_len = ntoh32(ctx->r.rsp.total_body_length);

  ssize_t wret = ctx->conn->WritevFull(out_vec, 2);
  if (unlikely(wret !=
               static_cast<ssize_t>(sizeof(ctx->r.rsp) + out_vec[1].iov_len))) {
    BUG_ON(wret != -EPIPE && wret != -ECONNRESET);
  }
}

static ssize_t pull_memcached_req(rt::TcpConn *c, MemcachedHdr *h,
                                  unsigned char *body) {
  ssize_t body_len;

  ssize_t rret = c->ReadFull(h, sizeof(*h));
  if (unlikely(rret != static_cast<ssize_t>(sizeof(*h)))) {
    BUG_ON(rret != 0 && rret != -ECONNRESET);
    return -1;
  }

  body_len = ntoh32(h->total_body_length);
  if (unlikely(body_len > MAX_REQUEST_BODY)) return -1;

  if (body_len) {
    rret = c->ReadFull(body, body_len);
    if (unlikely(rret != body_len)) {
      BUG_ON(rret != 0 && rret != -ECONNRESET);
      return -1;
    }
  }

  return body_len;
}

void OnetoOneHandler(std::unique_ptr<rt::TcpConn> conn) {
  MemcachedHdr h;
  unsigned char body[MAX_REQUEST_BODY];

  struct iovec iov[2];
  iov[0].iov_base = &h;
  iov[0].iov_len = sizeof(h);
  iov[1].iov_base = body;

  std::vector<std::unique_ptr<rt::TcpConn>> backends;
  for (auto &addr : memcached_addrs) {
    rt::TcpConn *c;
    if (use_affinity_dial) {
      c = conn->DialAffinity(addr);
    } else {
      c = rt::TcpConn::Dial({0, 0}, addr);
    }
    if (!c) {
      log_err("couldn't connect");
      exit(1);
    }
    backends.emplace_back(c);
  }

  while (true) {
    ssize_t body_len = pull_memcached_req(conn.get(), &h, body);
    if (unlikely(body_len < 0)) return;

    unsigned char *key = body + h.extras_length;
    unsigned int srv_idx = route(key, ntoh16(h.key_length));

    iov[1].iov_len = body_len;
    if (unlikely(WritevFull_(backends[srv_idx].get(), iov, 2) <= 0)) {
      log_err("error writing to backend");
      return;
    }

    body_len = pull_memcached_req(backends[srv_idx].get(), &h, body);
    iov[1].iov_len = body_len;

    if (unlikely(WritevFull_(conn.get(), iov, 2) <= 0)) {
      return;
    }
  }
}

void ClientHandler(std::shared_ptr<rt::TcpConn> conn) {
  auto resp = std::make_shared<SharedTcpStream>(conn);

  while (true) {
    auto ctx = new MemcachedRequestContext(resp);

    ssize_t body_len =
        pull_memcached_req(conn.get(), &ctx->r.req, ctx->request_body);
    if (unlikely(body_len < 0)) {
      delete ctx;
      return;
    }

    ctx->r.req_body_len = body_len;
    BUG_ON(!ctx->r.req_body_len);

    rt::Thread([=] {
      HandleRequest(ctx);
      delete ctx;
    })
        .Detach();
  }
}

void ServerHandler(void *arg) {
  /* dial memcached connections */
  if (use_pooled_connections) {
    for (auto &w : memcached_addrs) {
      auto ep = RpcEndpoint<MemcachedHdr>::Create(w);
      if (ep == nullptr) BUG();
      memcached_endpoints.push_back(ep);
    }
  }

  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen(listenaddr, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    if (use_pooled_connections) {
      rt::Thread([=] { ClientHandler(std::shared_ptr<rt::TcpConn>(c)); })
          .Detach();
    } else {
      rt::Thread([=] { OnetoOneHandler(std::unique_ptr<rt::TcpConn>(c)); })
          .Detach();
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc < 6) {
    fprintf(stderr,
            "usage: %s [cfg] listenaddr use_pool use_affinity_dial "
            "memcached_addrs... \n",
            argv[0]);
    return -EINVAL;
  }

  if (StringToAddr(argv[2], &listenaddr)) {
    printf("failed to parse addr %s\n", argv[2]);
    return -EINVAL;
  }

  use_pooled_connections = !!atoi(argv[3]);
  use_affinity_dial = !!atoi(argv[4]);

  for (int i = 5; i < argc; i++) {
    netaddr addr;
    if (StringToAddr(argv[i], &addr)) {
      printf("failed to parse addr %s\n", argv[i]);
      return -EINVAL;
    }
    // Duplicate the servers for now...
    // for (int j = 0; j < 400; j++) {
    memcached_addrs.push_back(addr);
    // }
  }

  int ret = runtime_init(argv[1], ServerHandler, NULL);
  if (ret) printf("failed to start runtime\n");

  return ret;
}
