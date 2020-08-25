#include "rpc.h"

namespace bw {

namespace {

std::function<void(struct srpc_ctx *)> handler;

void RpcServerTrampoline(struct srpc_ctx *arg) {
  handler(arg);
}

} // namespace

int RpcServerEnable(std::function<void(struct srpc_ctx *)> f) {
  handler = f;
  int ret = srpc_enable(RpcServerTrampoline);
  BUG_ON(ret == -EBUSY);
  return ret;
}

uint64_t RpcServerStatWinuRx() {
  return srpc_stat_winu_rx();
}

uint64_t RpcServerStatWinuTx() {
  return srpc_stat_winu_tx();
}

uint64_t RpcServerStatWinTx() {
  return srpc_stat_win_tx();
}

uint64_t RpcServerStatReqRx() {
  return srpc_stat_req_rx();
}

uint64_t RpcServerStatReqDropped() {
  return srpc_stat_req_dropped();
}

uint64_t RpcServerStatRespTx() {
  return srpc_stat_resp_tx();
}

} // namespace bw
