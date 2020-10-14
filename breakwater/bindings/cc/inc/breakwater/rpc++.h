// rpc.h - support for remote procedure calls (RPCs)

#pragma once

extern "C" {
#include <base/stddef.h>
#include <breakwater/rpc.h>
}

#include <functional>

namespace rpc {

class RpcClient {
 public:
  // The maximum size of an RPC request payload.
  static constexpr size_t kMaxPayloadSize = SRPC_BUF_SIZE;

  // Disable move and copy.
  RpcClient(const RpcClient&) = delete;
  RpcClient& operator=(const RpcClient&) = delete;

  // Creates an RPC session.
  static RpcClient *Dial(netaddr raddr, int id);

  // Sends an RPC request.
  ssize_t Send(const void *buf, size_t len, int hash);

  // Receives an RPC request.
  ssize_t Recv(void *buf, size_t len, uint64_t *latency);

  uint32_t WinAvail();

  void StatClear();

  uint64_t StatWinuRx();

  uint64_t StatWinuTx();

  uint64_t StatRespRx();

  uint64_t StatReqTx();

  uint64_t StatWinExpired();

  uint64_t StatReqDropped();

  // Shuts down the RPC connection.
  int Shutdown(int how);
  // Aborts the RPC connection.
  void Abort();

  void Close();

 private:
  RpcClient(struct crpc_session *s) : s_(s) { }

  // The client session object.
  struct crpc_session *s_;
};

// Enables the RPC server, listening for new sessions.
// Can only be called once.
int RpcServerEnable(std::function<void(struct srpc_ctx *)> f);

uint64_t RpcServerStatWinuRx();
uint64_t RpcServerStatWinuTx();
uint64_t RpcServerStatWinTx();
uint64_t RpcServerStatReqRx();
uint64_t RpcServerStatReqDropped();
uint64_t RpcServerStatRespTx();
} // namespace rpc
