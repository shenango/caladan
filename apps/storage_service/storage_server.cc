extern "C" {
#include <base/log.h>
#include <runtime/smalloc.h>
#include <runtime/storage.h>
#include <runtime/runtime.h>
}

#include "net.h"
#include "sync.h"
#include "thread.h"

#include <iostream>
#include <memory>
#include <new>

#include "reflex.h"

constexpr unsigned int kSectorSize = 512;
constexpr uint64_t kStorageServicePort = 5000;
constexpr uint64_t kMaxSectorsPerPayload = 8;

class SharedTcpStream {
 public:
  SharedTcpStream(std::shared_ptr<rt::TcpConn> c) : c_(c) {}
  ssize_t WriteFull(const void *buf, size_t len) {
    rt::ScopedLock<rt::Mutex> lock(&sendMutex_);
    return c_->WriteFull(buf, len);
  }
  ssize_t WritevFull(const struct iovec *iov, int iovcnt) {
    int i = 0;
    ssize_t sent = 0;
    struct iovec vs[iovcnt];
    memcpy(vs, iov, sizeof(*iov) * iovcnt);
    rt::ScopedLock<rt::Mutex> lock(&sendMutex_);
    while (iovcnt) {
      ssize_t ret = c_->Writev(&vs[i], iovcnt);
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
    }
    return sent;
  }

 private:
  std::shared_ptr<rt::TcpConn> c_;
  rt::Mutex sendMutex_;
};

class RequestContext {
 public:
  RequestContext(std::shared_ptr<SharedTcpStream> c) : conn(c) {}
  binary_header_blk_t header;
  std::shared_ptr<SharedTcpStream> conn;
  char buf[kSectorSize * kMaxSectorsPerPayload];
  void *operator new(size_t size) {
    void *p = smalloc(size);
    if (unlikely(p == nullptr)) throw std::bad_alloc();
    return p;
  }
  void operator delete(void *p) { sfree(p); }
};

void HandleGetRequest(RequestContext *ctx) {
  ssize_t ret = storage_read(ctx->buf, ctx->header.lba, ctx->header.lba_count);
  if (ret < 0)
    return;
  size_t payload_size = kSectorSize * ctx->header.lba_count;
  struct iovec response[2] = {
      {
          .iov_base = &ctx->header,
          .iov_len = sizeof(ctx->header),
      },
      {
          .iov_base = &ctx->buf,
          .iov_len = payload_size,
      },
  };
  ret = ctx->conn->WritevFull(response, 2);
  if (ret != static_cast<ssize_t>(sizeof(ctx->header) + payload_size)) {
    if (ret != -EPIPE && ret != -ECONNRESET)
      log_err_ratelimited("WritevFull failed: ret = %ld", ret);
  }
}

void HandleSetRequest(RequestContext *ctx) {
  ssize_t ret = storage_write(ctx->buf, ctx->header.lba, ctx->header.lba_count);
  if (ret < 0)
    return;

  ret = ctx->conn->WriteFull(&ctx->header, sizeof(ctx->header));
  if (ret != static_cast<ssize_t>(sizeof(ctx->header))) {
    if (ret != -EPIPE && ret != -ECONNRESET) log_err("tcp_write failed");
  }
}

void ServerWorker(std::shared_ptr<rt::TcpConn> c) {
  auto resp = std::make_shared<SharedTcpStream>(c);
  while (true) {
    /* allocate context */
    auto ctx = new RequestContext(resp);
    binary_header_blk_t *h = &ctx->header;

    /* Receive a work request. */
    ssize_t ret = c->ReadFull(h, sizeof(*h));
    // log_err("read ret %ld", ret);
    if (ret != static_cast<ssize_t>(sizeof(*h))) {
      if (ret != 0 && ret != -ECONNRESET)
        log_err("read failed, ret = %ld", ret);
      delete ctx;
      return;
    }

    /* validate request */
    if (h->magic != sizeof(binary_header_blk_t) ||
        (h->opcode != CMD_GET && h->opcode != CMD_SET) ||
        h->lba_count > kMaxSectorsPerPayload) {
      log_err("invalid request %x %x %x", h->magic, h->opcode, h->lba_count);
      delete ctx;
      return;
    }

    /* spawn thread to handle storage request + response */
    if (h->opcode == CMD_SET) {
      size_t payload_size = h->lba_count * kSectorSize;
      ret = c->ReadFull(ctx->buf, payload_size);
      if (ret != static_cast<ssize_t>(payload_size)) {
        if (ret != 0 && ret != -ECONNRESET)
          log_err("tcp_read failed, ret = %ld", ret);
        delete ctx;
        return;
      }
      rt::Thread([=] {
        HandleSetRequest(ctx);
        delete ctx;
      })
          .Detach();
    } else {
      rt::Thread([=] {
        HandleGetRequest(ctx);
        delete ctx;
      })
          .Detach();
    }
  }
}

void MainHandler(void *arg) {
  std::unique_ptr<rt::TcpQueue> q(
      rt::TcpQueue::Listen({0, kStorageServicePort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  BUG_ON(kSectorSize != storage_block_size());
  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { ServerWorker(std::shared_ptr<rt::TcpConn>(c)); }).Detach();
  }
}

int main(int argc, char *argv[]) {
  int ret;

  if (argc < 2) {
    std::cerr << "usage: [cfg_file]" << std::endl;
    return -EINVAL;
  }

  ret = runtime_init(argv[1], MainHandler, NULL);
  if (ret) {
    std::cerr << "failed to start runtime" << std::endl;
    return ret;
  }

  return 0;
}
