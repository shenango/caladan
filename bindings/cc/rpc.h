// rpc.h - support for remote procedure calls (RPCs)

#pragma once

extern "C" {
#include <base/stddef.h>
#include <runtime/rpc.h>
}

#include <functional>

namespace rt {

class RpcClient {
 public:
  // The maximum size of an RPC request payload.
  static constexpr size_t kMaxPayloadSize = SRPC_BUF_SIZE;

  ~RpcClient() { crpc_close(s_); }

  // Disable move and copy.
  RpcClient(const RpcClient&) = delete;
  RpcClient& operator=(const RpcClient&) = delete;

  // Creates an RPC session.
  static RpcClient *Dial(netaddr raddr) {
    crpc_session *s;
    raddr.port = SRPC_PORT;
    int ret = crpc_open(raddr, &s);
    if (ret) return nullptr;
    return new RpcClient(s);
  }

  // Is the session currently busy (will the next RPC be rejected)?
  bool IsBusy() const { return crpc_is_busy(s_); }

  // Sends an RPC request.
  ssize_t Send(const void *buf, size_t len) {
    return crpc_send_one(s_, buf, len);
  }

  // Receives an RPC request.
  ssize_t Recv(void *buf, size_t len) {
    return crpc_recv_one(s_, buf, len);
  }

  // Shuts down the RPC connection.
  int Shutdown(int how) { return tcp_shutdown(s_->c, how); }
  // Aborts the RPC connection.
  void Abort() { return tcp_abort(s_->c); }

 private:
  RpcClient(struct crpc_session *s) : s_(s) { }

  // The client session object.
  struct crpc_session *s_;
};

// Enables the RPC server, listening for new sessions.
// Can only be called once.
int RpcServerEnable(std::function<void(struct srpc_ctx *)> f);

} // namespace rt
