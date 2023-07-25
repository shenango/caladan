
extern "C" {
#include <base/byteorder.h>
#include <base/log.h>
#include <runtime/runtime.h>
#include <runtime/smalloc.h>
#include <runtime/thread.h>
#include <stdint.h>
}

#include "net.h"
#include "sync.h"
#include "thread.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "RpcManager.h"

template <class T>
struct RequestTracker {
  rt::WaitGroup *wg;
  uint32_t prev_reqid;
  Rpc<T> *rpc;
};

template <class T>
class RpcEndpointConnection {
 public:
  RpcEndpointConnection(std::unique_ptr<rt::TcpConn> conn);
  int SubmitRequest(rt::WaitGroup *wg, Rpc<T> *rpc);

 private:
  std::unique_ptr<rt::TcpConn> conn_;
  rt::Mutex lock_;
  std::unordered_map<uint64_t, RequestTracker<T>> reqs_;
  size_t req_id_ctr_{0};
};

template <class T>
RpcEndpointConnection<T>::RpcEndpointConnection(
    std::unique_ptr<rt::TcpConn> conn)
    : conn_(std::move(conn)) {
  rt::Thread([=] {
    while (true) {
      T hdr;
      ssize_t rret = conn_->ReadFull(&hdr, sizeof(hdr));
      if (rret != static_cast<ssize_t>(sizeof(hdr))) BUG();

      uint32_t reqid = hdr.get_reqid();
      lock_.Lock();
      RequestTracker<T> &rr = reqs_.at(reqid);
      RequestTracker<T> req = rr;
      BUG_ON(reqs_.erase(reqid) != 1);
      lock_.Unlock();

      hdr.set_reqid(req.prev_reqid);
      req.rpc->rsp = hdr;

      BUG_ON(hdr.get_body_len() > req.rpc->rsp_body_len);
      if (hdr.get_body_len() > 0) {
        rret = conn_->ReadFull(req.rpc->rsp_body, hdr.get_body_len());
        if (rret <= 0) BUG();
      }

      req.wg->Done();
    }
  })
      .Detach();
}

template <class T>
RpcEndpoint<T> *RpcEndpoint<T>::Create(netaddr remote) {
  std::vector<std::unique_ptr<rt::TcpConn>> conns;
  for (int i = 0; i < runtime_max_cores(); i++) {
    rt::TcpConn *c = rt::TcpConn::DialAffinity(i, remote);
    if (!c) return nullptr;
    conns.emplace_back(c);
  }

  std::vector<RpcEndpointConnection<T> *> epcs;
  for (auto &w : conns)
    epcs.push_back(new RpcEndpointConnection<T>(std::move(w)));

  return new RpcEndpoint<T>(epcs);
}

template <class T>
int RpcEndpoint<T>::SubmitRequestBlocking(Rpc<T> *req) {
  rt::WaitGroup wg(1);

  int ret = SubmitRequestAsync(&wg, req);
  if (ret) return ret;

  wg.Wait();
  return 0;
}

template <class T>
int RpcEndpoint<T>::SubmitRequestAsync(rt::WaitGroup *wg, Rpc<T> *r) {
  int idx = get_current_affinity();
  return connections_.at(idx)->SubmitRequest(wg, r);
}

template <class T>
int RpcEndpointConnection<T>::SubmitRequest(rt::WaitGroup *wg, Rpc<T> *r) {
  struct iovec iov[2];
  iov[0].iov_base = &r->req;
  iov[0].iov_len = sizeof(r->req);
  iov[1].iov_base = r->req_body;
  iov[1].iov_len = r->req_body_len;

  uint32_t prev_reqid = r->req.get_reqid();
  rt::ScopedLock<rt::Mutex> l(&lock_);
  r->req.set_reqid(req_id_ctr_);
  reqs_[req_id_ctr_++] = {wg, prev_reqid, r};

  ssize_t wret = WritevFull_(conn_.get(), iov, 2);
  if (wret != static_cast<ssize_t>(sizeof(T) + r->req_body_len)) BUG();

  return 0;
}

ssize_t WritevFull_(rt::TcpConn *c, const struct iovec *iov, int iovcnt) {
  int i = 0;
  ssize_t sent = 0;
  struct iovec vs[iovcnt];
  memcpy(vs, iov, sizeof(*iov) * iovcnt);

  do {
    ssize_t ret = c->Writev(&vs[i], iovcnt);
    if (ret <= 0) return ret;
    sent += ret;
    while (iovcnt && ret >= static_cast<ssize_t>(vs[i].iov_len)) {
      ret -= vs[i].iov_len;
      i++;
      iovcnt--;
    }
    if (ret) {
      vs[i].iov_base = (unsigned char *)vs[i].iov_base + ret;
      vs[i].iov_len -= ret;
    }
  } while (iovcnt);
  return sent;
}

template class RpcEndpoint<MemcachedHdr>;
template class RpcEndpoint<ReflexHdr>;
