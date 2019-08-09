#pragma once

template <class ProtoHdr>
struct Rpc {
  ProtoHdr req;
  void *req_body;
  size_t req_body_len;

  ProtoHdr rsp;
  void *rsp_body;
  size_t rsp_body_len;
};

template <class T>
class RpcEndpointConnection;
template <class T>
class RpcEndpoint {
 public:
  int SubmitRequestBlocking(Rpc<T> *r);
  int SubmitRequestAsync(rt::WaitGroup *wg, Rpc<T> *r);
  static RpcEndpoint *Create(netaddr remote);

 private:
  RpcEndpoint(std::vector<RpcEndpointConnection<T> *> connections)
      : connections_(std::move(connections)) {}
  std::vector<RpcEndpointConnection<T> *> connections_;
};

ssize_t WritevFull_(rt::TcpConn *c, const struct iovec *iov, int iovcnt);

/* Reflex protocol */
#define CMD_GET 0x00
#define CMD_SET 0x01

struct ReflexHdr {
  uint16_t magic;
  uint16_t opcode;
  uint64_t req_handle;
  unsigned long lba;
  unsigned int lba_count;
  uint32_t get_reqid() { return req_handle; }
  void set_reqid(uint32_t reqid) { req_handle = reqid; }
  size_t get_body_len() {
    BUG();
    return 512 * lba_count;
  }
} __packed;
static_assert(sizeof(ReflexHdr) == 24);
template class RpcEndpoint<ReflexHdr>;

/* Memcached binary protocol */
struct MemcachedHdr {
  uint8_t magic;
  uint8_t opcode;
  uint16_t key_length;
  uint8_t extras_length;
  uint8_t data_type;
  uint16_t vbucket_id_or_status;
  uint32_t total_body_length;
  uint32_t opaque;
  uint64_t cas;
  uint32_t get_reqid() { return opaque; }
  void set_reqid(uint32_t reqid) { opaque = reqid; }
  size_t get_body_len() { return ntoh32(total_body_length); }
} __packed;
static_assert(sizeof(MemcachedHdr) == 24);
template class RpcEndpoint<MemcachedHdr>;

class SharedTcpStream {
 public:
  SharedTcpStream(std::shared_ptr<rt::TcpConn> c) : c_(c) {}
  ssize_t WriteFull(const void *buf, size_t len) {
    rt::ScopedLock<rt::Mutex> lock(&sendMutex_);
    return c_->WriteFull(buf, len);
  }
  ssize_t WritevFull(const struct iovec *iov, int iovcnt) {
    rt::ScopedLock<rt::Mutex> lock(&sendMutex_);
    return WritevFull_(c_.get(), iov, iovcnt);
  }

 private:
  std::shared_ptr<rt::TcpConn> c_;
  rt::Mutex sendMutex_;
};
