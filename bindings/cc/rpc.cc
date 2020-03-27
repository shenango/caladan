#include "rpc.h"

namespace rt {

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

uint64_t RpcServerStatReqDropped() {
  return srpc_stat_req_dropped();
}

uint64_t RpcServerStatReqRx() {
  return srpc_stat_req_rx();
}

uint64_t RpcServerStatDreqRx() {
  return srpc_stat_dreq_rx();
}

uint64_t RpcServerStatRespTx() {
  return srpc_stat_resp_tx();
}

uint64_t RpcServerStatOfferTx() {
  return srpc_stat_offer_tx();
}

} // namespace rt
