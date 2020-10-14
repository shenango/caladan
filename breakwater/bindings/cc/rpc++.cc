#include <breakwater/rpc++.h>

namespace rpc {

namespace {

std::function<void(struct srpc_ctx *)> handler;

void RpcServerTrampoline(struct srpc_ctx *arg) {
  handler(arg);
}

} // namespace

RpcClient *RpcClient::Dial(netaddr raddr, int id) {
  crpc_session *s;
  raddr.port = SRPC_PORT;
  int ret = crpc_ops->crpc_open(raddr, &s, id);
  if (ret) return nullptr;
  return new RpcClient(s);
}

ssize_t RpcClient::Send(const void *buf, size_t len, int hash) {
  return crpc_ops->crpc_send_one(s_, buf, len, hash);
}

ssize_t RpcClient::Recv(void *buf, size_t len,
			uint64_t *latency = nullptr) {
  return crpc_ops->crpc_recv_one(s_, buf, len, latency);
}

uint32_t RpcClient::WinAvail() {
  return crpc_ops->crpc_win_avail(s_);
}

void RpcClient::StatClear() {
  return crpc_ops->crpc_stat_clear(s_);
}

uint64_t RpcClient::StatWinuRx() {
  return crpc_ops->crpc_stat_winu_rx(s_);
}

uint64_t RpcClient::StatWinuTx() {
  return crpc_ops->crpc_stat_winu_tx(s_);
}

uint64_t RpcClient::StatRespRx() {
  return crpc_ops->crpc_stat_resp_rx(s_);
}

uint64_t RpcClient::StatReqTx() {
  return crpc_ops->crpc_stat_req_tx(s_);
}

uint64_t RpcClient::StatWinExpired() {
  return crpc_ops->crpc_stat_win_expired(s_);
}

uint64_t RpcClient::StatReqDropped() {
  return crpc_ops->crpc_stat_req_dropped(s_);
}

int RpcClient::Shutdown(int how) {
  return tcp_shutdown(s_->c, how);
}

void RpcClient::Abort() {
  tcp_abort(s_->c);
}

void RpcClient::Close() {
  crpc_ops->crpc_close(s_);
}

int RpcServerEnable(std::function<void(struct srpc_ctx *)> f) {
  handler = f;
  int ret = srpc_ops->srpc_enable(RpcServerTrampoline);
  BUG_ON(ret == -EBUSY);
  return ret;
}

uint64_t RpcServerStatWinuRx() {
  return srpc_ops->srpc_stat_winu_rx();
}

uint64_t RpcServerStatWinuTx() {
  return srpc_ops->srpc_stat_winu_tx();
}

uint64_t RpcServerStatWinTx() {
  return srpc_ops->srpc_stat_win_tx();
}

uint64_t RpcServerStatReqRx() {
  return srpc_ops->srpc_stat_req_rx();
}

uint64_t RpcServerStatReqDropped() {
  return srpc_ops->srpc_stat_req_dropped();
}

uint64_t RpcServerStatRespTx() {
  return srpc_ops->srpc_stat_resp_tx();
}

} // namespace rpc
