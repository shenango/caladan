extern "C" {
#include <base/log.h>
#include <runtime/runtime.h>
#include <runtime/smalloc.h>
#include <runtime/storage.h>

#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>
}

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "net.h"
#include "sync.h"
#include "thread.h"

#include <snappy.h>

#include <iostream>
#include <memory>
#include <new>

#include "reflex.h"

constexpr unsigned int kSectorSize = 512;
constexpr uint64_t kStorageServicePort = 5000;

static unsigned char iv[16];

class SharedTcpStream {
 public:
  SharedTcpStream(std::shared_ptr<rt::TcpConn> c) : c_(c) {
    int ret;

    preempt_disable();
    aes_ctx_ = EVP_CIPHER_CTX_new();
    preempt_enable();
    if (!aes_ctx_)
      throw std::bad_alloc();

    memset(aes_key_, 0xcc, sizeof(aes_key_));
    ret = EVP_EncryptInit_ex(aes_ctx_, EVP_aes_256_cbc(), NULL, aes_key_, iv);
    if (ret != 1)
      panic("AES init %d", ret);

  }

  ~SharedTcpStream() {
    preempt_disable();
    EVP_CIPHER_CTX_free(aes_ctx_);
    preempt_enable();
  }

  ssize_t WriteFull(const void *buf, size_t len) {
    rt::ScopedLock<rt::Mutex> lock(&sendMutex);
    return c_->WriteFull(buf, len);
  }
  ssize_t WritevFull(const struct iovec *iov, int iovcnt) {
    rt::ScopedLock<rt::Mutex> lock(&sendMutex);
    return WritevFullLocked(iov, iovcnt);
  }

  ssize_t EncryptStream(char *plaintext, size_t size, char *ciphertext) {
    int ret;
    int len;

    if (size % 16 != 0)
      return -EINVAL;

    ret = EVP_EncryptUpdate(aes_ctx_, (unsigned char *)ciphertext, &len, (unsigned char *)plaintext, size);
    if (ret != 1)
      return -EINVAL;

    return len;
  }

  ssize_t WritevFullLocked(const struct iovec *iov, int iovcnt) {
    int i = 0;
    ssize_t sent = 0;
    struct iovec vs[iovcnt];
    memcpy(vs, iov, sizeof(*iov) * iovcnt);
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

  rt::Mutex sendMutex;
 private:
  EVP_CIPHER_CTX *aes_ctx_;
  unsigned char aes_key_[32];
  std::shared_ptr<rt::TcpConn> c_;
};

static char *allocate_buf(size_t sz) {
  char *buf;

  if (sz <= (1 << 18)) {
    buf = (char *)smalloc(sz);
  } else {
    preempt_disable();
    buf = (char *)malloc(sz);
    preempt_enable();
  }
  if (!buf) throw std::bad_alloc();
  return buf;
}

static void free_buf(char *buf, size_t sz) {
  if (!buf) return;
  if (sz <= (1 << 18)) {
    sfree(buf);
  } else {
    preempt_disable();
    free(buf);
    preempt_enable();
  }
}

class RequestContext {
 public:
  RequestContext(std::shared_ptr<SharedTcpStream> c) : conn(c) {}
  ~RequestContext() { free_buf(buf, bufsz); }
  binary_header_blk_t header;
  std::shared_ptr<SharedTcpStream> conn;
  char *buf{nullptr};
  size_t bufsz{0};

  void *operator new(size_t size) {
    void *p = smalloc(size);
    if (unlikely(p == nullptr)) throw std::bad_alloc();
    return p;
  }
  void operator delete(void *p) { sfree(p); }
};


static void DoRequest(RequestContext *ctx, char *read_buf, char *compress_buf)
{
  size_t input_length = ctx->header.lba_count * kSectorSize;
  ssize_t ret = storage_read(read_buf, ctx->header.lba, ctx->header.lba_count);
  if (unlikely(ret != 0)) {
    log_warn_ratelimited("storage ret: %ld", ret);
    return;
  }

  size_t compressed_length;
  snappy::RawCompress(read_buf, input_length,
                      compress_buf, &compressed_length);

  rt::ScopedLock<rt::Mutex> l(&ctx->conn->sendMutex);

  ssize_t encrypt_len = ctx->conn->EncryptStream(compress_buf, align_up(compressed_length, 16), read_buf);
  if (unlikely(encrypt_len < 0))
    panic("encrypt");

  barrier();
  ctx->header.tsc = rdtsc();
  ctx->header.lba_count = (size_t)encrypt_len;
  struct iovec response[2] = {
      {
          .iov_base = &ctx->header,
          .iov_len = sizeof(ctx->header),
      },
      {
          .iov_base = read_buf,
          .iov_len = (size_t)encrypt_len,
      },
  };
  ssize_t wret = ctx->conn->WritevFullLocked(response, 2);
  if (wret != static_cast<ssize_t>(sizeof(ctx->header) + encrypt_len)) {
    if (wret != -EPIPE && wret != -ECONNRESET)
      log_err_ratelimited("WritevFull failed: ret = %ld", wret);
  }
}


#define ON_STACK_THRESH (32 * KB)

void HandleGetRequestSmall(RequestContext *ctx) {
  size_t input_length = ctx->header.lba_count * kSectorSize;
  ssize_t max_buf_sz = snappy::MaxCompressedLength(input_length);
  max_buf_sz = align_up(max_buf_sz + 16, 16);

  char read_buf[max_buf_sz];
  char compress_buf[max_buf_sz];

  DoRequest(ctx, read_buf, compress_buf);
}

void HandleGetRequest(RequestContext *ctx) {
  size_t input_length = ctx->header.lba_count * kSectorSize;
  ssize_t max_buf_sz = snappy::MaxCompressedLength(input_length);
  max_buf_sz = align_up(max_buf_sz + 16, 16);

  char *read_buf, *compress_buf;
  read_buf = allocate_buf(max_buf_sz);
  compress_buf = allocate_buf(max_buf_sz);

  DoRequest(ctx, read_buf, compress_buf);


  free_buf(read_buf, max_buf_sz);
  free_buf(compress_buf, max_buf_sz);

}

void HandleSetRequest(RequestContext *ctx) {
  ssize_t ret = storage_write(ctx->buf, ctx->header.lba, ctx->header.lba_count);
  if (unlikely(ret != 0)) {
    log_warn("bad set: rc %ld", ret);
  }
  return;

  ctx->header.tsc = rdtsc();
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
        (h->opcode != CMD_GET && h->opcode != CMD_SET)) {
      log_err("invalid request %x %x %x", h->magic, h->opcode, h->lba_count);
      delete ctx;
      return;
    }

    size_t payload_size = h->lba_count * kSectorSize;

    /* spawn thread to handle storage request + response */
    if (h->opcode == CMD_SET) {
      ctx->buf = allocate_buf(payload_size);
      ctx->bufsz = payload_size;
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
        size_t payload_size = ctx->header.lba_count * kSectorSize;
        if (payload_size > ON_STACK_THRESH) {
          HandleGetRequest(ctx);
        } else {
          HandleGetRequestSmall(ctx);
        }
        delete ctx;
      })
          .Detach();
    }
  }
}

void MainHandler(void *arg) {
  if (kSectorSize != storage_block_size())
    panic("storage not enabled");

  std::unique_ptr<rt::TcpQueue> q(
      rt::TcpQueue::Listen({0, kStorageServicePort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

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
