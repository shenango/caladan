extern "C" {
#include <base/log.h>
#include <net/ip.h>
}
#undef min
#undef max

#include "runtime.h"
#include "thread.h"
#include "sync.h"
#include "timer.h"
#include "net.h"
#include "proto.h"

#include <iostream>
#include <iomanip>
#include <utility>
#include <memory>
#include <chrono>
#include <vector>
#include <future>
#include <algorithm>
#include <numeric>

namespace {

using sec = std::chrono::duration<double, std::micro>;

// The number of samples to discard from the start and end.
constexpr uint64_t kDiscardSamples = 1000;

// the number of worker threads to spawn.
int threads;
// the remote UDP address of the server.
netaddr raddr;
// the number of samples to gather.
uint64_t n;

void KillConn(rt::UdpConn *c)
{
  constexpr int kKillRetries = 10;
  union {
    unsigned char buf[32];
    payload p;
  };
  p.tag = kKill;
  for (int i = 0; i < kKillRetries; ++i)
    udp_send(buf, sizeof(buf), c->LocalAddr(), c->RemoteAddr());
}

std::vector<double> ClientWorker(rt::UdpConn *c, uint64_t n,
                                 rt::WaitGroup *start) {
  unsigned char buf[32] = {};
  std::vector<double> timings;
  timings.reserve(n);

  // Synchronized start of load generation.
  start->Done();
  start->Wait();

  for (uint64_t i = 0; i < n; ++i) {
    barrier();
    auto start = std::chrono::steady_clock::now();
    barrier();

    // Send a network request.
    ssize_t ret = c->Write(buf, sizeof(buf));
    if (ret != static_cast<ssize_t>(sizeof(buf)))
      panic("udp write failed, ret = %ld", ret);

    // Receive a network response.
    ret = c->Read(buf, sizeof(buf));
    if (ret != static_cast<ssize_t>(sizeof(buf)))
      panic("udp read failed, ret = %ld", ret);

    barrier();
    auto finish = std::chrono::steady_clock::now();
    barrier();

    timings.push_back(std::chrono::duration_cast<sec>(finish - start).count());
  }

  return timings;
}

void RunExperiment() {
    std::unique_ptr<rt::UdpConn> c(rt::UdpConn::Dial({0, 0}, raddr));
    if (c == nullptr) panic("couldn't establish control connection");

    // Send the control message.
    nbench_req req = {kMagic, threads};
    ssize_t ret = c->Write(&req, sizeof(req));
    if (ret != sizeof(req)) panic("couldn't send control message");

    // Receive the control response.
    union {
      nbench_resp resp;
      char buf[rt::UdpConn::kMaxPayloadSize];
    };
    ret = c->Read(&resp, rt::UdpConn::kMaxPayloadSize);
    if (ret < static_cast<ssize_t>(sizeof(nbench_resp)))
      panic("failed to receive control response");
    if (resp.magic != kMagic || resp.nports != threads)
      panic("got back invalid control response");

    // Create one UDP connection per thread.
    std::vector<std::unique_ptr<rt::UdpConn>> conns;
    for (int i = 0; i < threads; ++i) {
      std::unique_ptr<rt::UdpConn>
        outc(rt::UdpConn::Dial(c->LocalAddr(), {raddr.ip, resp.ports[i]}));
      if (unlikely(outc == nullptr)) panic("couldn't connect to raddr.");
      conns.emplace_back(std::move(outc));
    }

    // Launch a worker thread for each connection.
    rt::WaitGroup starter(threads + 1);
    uint64_t total = 0;
    std::vector<rt::Thread> th;
    std::vector<double> *samples[threads];
    for (int i = 0; i < threads; ++i) {
      uint64_t pn = n / threads + 2 * kDiscardSamples;
      total += pn;
      th.emplace_back(rt::Thread([&, i]{
        auto v = ClientWorker(conns[i].get(), pn, &starter);
        samples[i] = new std::vector<double>(std::move(v));
      }));
    }

    // Give the workers time to initialize, then start recording.
    starter.Done();
    starter.Wait();

    // |--- start experiment duration timing ---|
    barrier();
    auto start = std::chrono::steady_clock::now();
    barrier();

    // Wait for the workers to finish.
    for (auto& t: th)
      t.Join();

    // |--- end experiment duration timing ---|
    barrier();
    auto finish = std::chrono::steady_clock::now();
    barrier();

    // Close the connections.
    for (auto& c: conns)
      KillConn(c.get());

    // Aggregate all the latency timings together.
    std::vector<double> timings;
    for (int i = 0; i < threads; ++i) {
      auto &v = *samples[i];
      if (v.size() <= kDiscardSamples * 2) panic("not enough samples");
      v.erase(v.begin(), v.begin() + kDiscardSamples);
      v.erase(v.end() - kDiscardSamples, v.end());
      timings.insert(timings.end(), v.begin(), v.end());
    }

    // Report statistics.
    double elapsed = std::chrono::duration_cast<sec>(finish - start).count();
    double reqs_per_sec = static_cast<double>(total) / elapsed * 1000000;
    std::sort(timings.begin(), timings.end());
    double sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    double mean = sum / timings.size();
    double count = static_cast<double>(timings.size());
    double p9 = timings[count * 0.9];
    double p99 = timings[count * 0.99];
    double p999 = timings[count * 0.999];
    double p9999 = timings[count * 0.9999];
    double min = timings[0];
    double max = timings[timings.size() - 1];
    std::cout << std::setprecision(2) << std::fixed
              << "t: "       << threads
              << " rps: "    << reqs_per_sec
              << " n: "      << timings.size()
              << " min: "    << min
              << " mean: "   << mean
              << " 90%: "    << p9
              << " 99%: "    << p99
              << " 99.9%: "  << p999
              << " 99.99%: " << p9999
              << " max: "    << max << std::endl;
}

void ClientHandler(void *arg) {
  for (int i = 5; i <= 200; i+=5) {
    threads = i;
    RunExperiment();
  }
}

int StringToAddr(const char *str, uint32_t *addr) {
  uint8_t a, b, c, d;

  if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4)
    return -EINVAL;

  *addr = MAKE_IP_ADDR(a, b, c, d);
  return 0;
}

} // anonymous namespace

int main(int argc, char *argv[]) {
  int ret;

  if (argc < 3) {
    std::cerr << "usage: [cfg_file] [cmd] ..." << std::endl;
    return -EINVAL;
  }

  std::string cmd = argv[2];
  if (cmd.compare("client") != 0) {
    std::cerr << "invalid command: " << cmd << std::endl;
    return -EINVAL;
  }

  if (argc != 6) {
    std::cerr << "usage: [cfg_file] client [#threads] [remote_ip] [n]"
              << std::endl;
    return -EINVAL;
  }

  threads = std::stoi(argv[3], nullptr, 0);

  ret = StringToAddr(argv[4], &raddr.ip);
  if (ret) return -EINVAL;
  raddr.port = kNetbenchPort;

  n = std::stoll(argv[5], nullptr, 0);

  ret = runtime_init(argv[1], ClientHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
