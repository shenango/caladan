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

} // namespace rt
