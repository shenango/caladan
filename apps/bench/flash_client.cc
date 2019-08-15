
extern "C" {
#include <base/hash.h>
#include <base/log.h>
#include <runtime/runtime.h>
#include <runtime/smalloc.h>
#include <runtime/storage.h>
}

#include "fake_worker.h"
#include "thread.h"

#include <memory>
#include <vector>

#include "RpcManager.h"

bool use_local;
netaddr listen_addr;
netaddr flash_server_addr;

FakeWorker *worker;
RpcEndpoint<ReflexHdr> *flash_server;

struct Payload {
  uint64_t work_iterations;
  uint64_t index;
};

class FlashClientReqContext : public RequestContext {
 public:
  using RequestContext::RequestContext;
  Payload req_hdr;
  ReflexHdr storage_hdr;
  unsigned char buf[512];
};

void HandleSingleRequest(FlashClientReqContext *ctx) {
  int ret;

  unsigned long lba = (unsigned long)rdtsc() % 524280UL;

  if (!use_local) {
    Rpc<ReflexHdr> r;
    r.req.magic = 24;
    r.req.opcode = 0;
    r.req.lba = lba;
    r.req.lba_count = 1;
    r.req_body = nullptr;
    r.req_body_len = 0;

    r.rsp_body = ctx->buf;
    r.rsp_body_len = 512;
    ret = flash_server->SubmitRequestBlocking(&r);
  } else {
    ret = storage_read(ctx->buf, lba, 1);
  }

  if (ret) return;

  uint32_t h = jenkins_hash(ctx->buf, 512);
  std::ignore = h;

  uint64_t iterations = ntoh64(ctx->req_hdr.work_iterations);
  worker->Work(iterations);

  ssize_t sret = ctx->conn->WriteFull(&ctx->req_hdr, sizeof(ctx->req_hdr));
  if (sret != sizeof(ctx->req_hdr))
    BUG_ON(sret != -EPIPE && sret != -ECONNRESET);
}

void ClientHandler(std::shared_ptr<rt::TcpConn> c) {
  auto stream = std::make_shared<SharedTcpStream>(c);

  while (true) {
    auto ctx = new FlashClientReqContext(stream);

    ssize_t rret = c->ReadFull(&ctx->req_hdr, sizeof(ctx->req_hdr));
    if (rret != static_cast<ssize_t>(sizeof(ctx->req_hdr))) {
      BUG_ON(rret != 0 && rret != -ECONNRESET);
      delete ctx;
      return;
    }

    rt::Thread([=] {
      HandleSingleRequest(ctx);
      delete ctx;
    })
        .Detach();
  }
}

void ServerHandler(void *arg) {
  if (!use_local) {
    flash_server = RpcEndpoint<ReflexHdr>::Create(flash_server_addr);
    if (flash_server == nullptr) {
      log_err("couldn't dial rpc connection");
      return;
    }
  }

  worker = FakeWorkerFactory("stridedmem:1024:7");
  if (!worker) {
    log_err("couldn't instatiate fake worker");
    return;
  }

  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen(listen_addr, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { ClientHandler(std::shared_ptr<rt::TcpConn>(c)); })
        .Detach();
  }
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: ./flash_client [cfg] [listen_addr:listen_port] "
            "<storageserveraddr>\n");
    return -EINVAL;
  }

  if (StringToAddr(argv[2], &listen_addr)) {
    printf("failed to parse addr %s\n", argv[2]);
    return -EINVAL;
  }

  use_local = argc == 3;
  if (!use_local && StringToAddr(argv[3], &flash_server_addr)) {
    printf("failed to parse addr %s\n", argv[3]);
    return -EINVAL;
  }

  int ret = runtime_init(argv[1], ServerHandler, NULL);
  if (ret) printf("failed to start runtime\n");

  return ret;
}
