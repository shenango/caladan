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
#include "fake_worker.h"
#include "proto.h"

#include <iostream>
#include <iomanip>
#include <utility>
#include <memory>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>

namespace {

using sec = std::chrono::duration<double, std::micro>;

// The number of samples to discard from the start and end.
constexpr uint64_t kDiscardSamples = 1000;
// The maximum lateness to tolerate before dropping egress samples.
constexpr uint64_t kMaxCatchUpUS = 5;

// the number of worker threads to spawn.
int threads;
// the remote UDP address of the server.
netaddr raddr;
// the number of samples to gather.
uint64_t n;
// the mean service time in us.
double st;

void ServerWorker(rt::UdpConn *c) {
  union {
    unsigned char buf[rt::UdpConn::kMaxPayloadSize];
    payload p;
  };
  std::unique_ptr<FakeWorker> w(FakeWorkerFactory("stridedmem:3200:64"));
  if (unlikely(w == nullptr)) panic("couldn't create worker");

  while (true) {
    // Receive a network response.
    ssize_t ret = c->Read(&buf, sizeof(buf));
    if (ret <= 0 || ret > static_cast<ssize_t>(sizeof(buf))) {
      if (ret == 0) break;
      panic("udp read failed, ret = %ld", ret);
    }

    // Determine if the connection is being killed.
    if (unlikely(p.tag == kKill)) {
      c->Shutdown();
      break;
    }

    // Perform fake work if requested.
    if (p.workn != 0) w->Work(p.workn * 82.0);

    // Send a network request.
    ssize_t sret = c->Write(&buf, ret);
    if (sret != ret) {
      if (sret == -EPIPE) break;
      panic("udp write failed, ret = %ld", sret);
    }
  }
}

void ServerHandler(void *arg) {
  std::unique_ptr<rt::UdpConn> c(rt::UdpConn::Listen({0, kNetbenchPort}));
  if (unlikely(c == nullptr)) panic("couldn't listen for control connections");

  while (true) {
    nbench_req req;
    netaddr raddr;
    ssize_t ret = c->ReadFrom(&req, sizeof(req), &raddr);
    if (ret != sizeof(req) || req.magic != kMagic) continue;

    rt::Spawn([=, &c]{
      log_info("got connection %x:%d, %d ports", raddr.ip,
               raddr.port, req.nports);

      union {
        nbench_resp resp;
        char buf[rt::UdpConn::kMaxPayloadSize];
      };
      resp.magic = kMagic;
      resp.nports = req.nports;

      std::vector<rt::Thread> threads;

      // Create the worker threads.
      std::vector<std::unique_ptr<rt::UdpConn>> conns;
      for (int i = 0; i < req.nports; ++i) {
        std::unique_ptr<rt::UdpConn> cin(rt::UdpConn::Dial({0, 0}, raddr));
	if (unlikely(cin == nullptr)) panic("couldn't dial data connection");
	resp.ports[i] = cin->LocalAddr().port;
        threads.emplace_back(rt::Thread(std::bind(ServerWorker, cin.get())));
        conns.emplace_back(std::move(cin));
      }

      // Send the port numbers to the client.
      ssize_t len = sizeof(nbench_resp) + sizeof(uint16_t) * req.nports;
      if (len > static_cast<ssize_t>(rt::UdpConn::kMaxPayloadSize))
        panic("too big");
      ssize_t ret = c->WriteTo(&resp, len, &raddr);
      if (ret != len) {
        log_err("udp write failed, ret = %ld", ret);
      }

      for (auto& t: threads)
        t.Join();
      log_info("done");
    });
  }
}

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

std::vector<double> PoissonWorker(rt::UdpConn *c, double req_rate,
                                  double service_time, rt::WaitGroup *starter)
{
  // Seed the random generator with the local port number.
  std::mt19937 g(c->RemoteAddr().port);

  // Create a packet transmit schedule.
  std::vector<double> sched;
  std::exponential_distribution<double> rd(1.0 / (1000000.0 / req_rate));
  std::vector<double> tmp(n);
  std::generate(tmp.begin(), tmp.end(), std::bind(rd, g));
  sched.push_back(tmp[0]);
  for (std::vector<double>::size_type j = 1; j < tmp.size(); ++j) {
    tmp[j] += tmp[j - 1];
    sched.push_back(static_cast<uint64_t>(tmp[j]));
  }

  // Create a fake work schedule.
  std::vector<double> work(n);
  std::exponential_distribution<double> wd(1.0 / service_time);
  std::generate(work.begin(), work.end(), std::bind(wd, g));

  // Reserve space to record results.
  auto n = sched.size();
  std::vector<double> timings;
  timings.reserve(n);
  std::vector<uint64_t> start_us(n);

  // Start the receiver thread.
  auto th = rt::Thread([&]{
    union {
      unsigned char rbuf[32] = {};
      payload rp;
    };

    while (true) {
     ssize_t ret = c->Read(rbuf, sizeof(rbuf));
     if (ret != static_cast<ssize_t>(sizeof(rbuf))) {
       if (ret == 0) break;
       panic("udp read failed, ret = %ld", ret);
     }

     barrier();
     uint64_t ts = microtime();
     barrier();
     timings.push_back(ts - start_us[rp.idx]);
    }
  });

  // Initialize timing measurement data structures.
  union {
    unsigned char buf[32] = {};
    payload p;
  };

  // Synchronized start of load generation.
  starter->Done();
  starter->Wait();

  barrier();
  uint64_t expstart = microtime();
  barrier();

  for (unsigned int i = 0; i < n; ++i) {
    barrier();
    uint64_t now = microtime();
    barrier();
    if (now - expstart < sched[i]) {
      rt::Sleep(sched[i] - (now - expstart));
      now = microtime();
    }
    if (now - expstart - sched[i] > kMaxCatchUpUS)
      continue;

    barrier();
    start_us[i] = microtime();
    barrier();

    // Send a network request.
    p.idx = i;
    p.workn = work[i];
    p.tag = 0;
    ssize_t ret = udp_send(buf, sizeof(buf), c->LocalAddr(), c->RemoteAddr());
    if (ret != static_cast<ssize_t>(sizeof(buf)))
      panic("udp write failed, ret = %ld", ret);
  }

  c->Shutdown();
  th.Join();

  return timings;
}

std::vector<double> RunExperiment(double req_rate, double *reqs_per_sec) {
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
  std::vector<rt::Thread> th;
  std::unique_ptr<std::vector<double>> samples[threads];
  for (int i = 0; i < threads; ++i) {
    th.emplace_back(rt::Thread([&, i]{
      auto v = PoissonWorker(conns[i].get(), req_rate / threads, st,
                             &starter);
      samples[i].reset(new std::vector<double>(std::move(v)));
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
  uint64_t total = 0;
  std::vector<double> timings;
  for (int i = 0; i < threads; ++i) {
    auto &v = *samples[i];
    total += v.size();
    if (v.size() <= kDiscardSamples * 2) panic("not enough samples");
    v.erase(v.begin(), v.begin() + kDiscardSamples);
    v.erase(v.end() - kDiscardSamples, v.end());
    timings.insert(timings.end(), v.begin(), v.end());
  }

  // Report results.
  double elapsed = std::chrono::duration_cast<sec>(finish - start).count();
  *reqs_per_sec = static_cast<double>(total) / elapsed * 1000000;
  return timings;
}

void DoExperiment(double req_rate) {
  constexpr int kRounds = 1;
  std::vector<double> timings;
  double reqs_per_sec = 0;
  for (int i = 0; i < kRounds; i++) {
    double tmp;
    auto t = RunExperiment(req_rate, &tmp);
    timings.insert(timings.end(), t.begin(), t.end());
    reqs_per_sec += tmp;
    rt::Sleep(500 * rt::kMilliseconds);
  }
  reqs_per_sec /= kRounds;

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
  for (double i = 500000; i <= 5000000; i += 500000)
    DoExperiment(i);
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
  if (cmd.compare("server") == 0) {
    ret = runtime_init(argv[1], ServerHandler, NULL);
    if (ret) {
      printf("failed to start runtime\n");
      return ret;
    }
  } else if (cmd.compare("client") != 0) {
    std::cerr << "invalid command: " << cmd << std::endl;
    return -EINVAL;
  }

  if (argc != 7) {
    std::cerr << "usage: [cfg_file] client [#threads] [remote_ip] [n] [service_us]"
              << std::endl;
    return -EINVAL;
  }

  threads = std::stoi(argv[3], nullptr, 0);

  ret = StringToAddr(argv[4], &raddr.ip);
  if (ret) return -EINVAL;
  raddr.port = kNetbenchPort;

  n = std::stoll(argv[5], nullptr, 0);
  st = std::stod(argv[6], nullptr);

  ret = runtime_init(argv[1], ClientHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
