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
constexpr uint64_t kMaxCatchUpUS = 10;

// the number of worker threads to spawn.
int threads;
// the remote UDP address of the server.
netaddr raddr;
// the number of samples to gather.
uint64_t n;
// the mean service time in us.
double st;

void ServerWorker(std::unique_ptr<rt::TcpConn> c) {
  payload p;
  std::unique_ptr<FakeWorker> w(FakeWorkerFactory("stridedmem:3200:64"));
  if (w == nullptr) panic("couldn't create worker");

  while (true) {
    // Receive a network response.
    ssize_t ret = c->ReadFull(&p, sizeof(p));
    if (ret <= 0 || ret > static_cast<ssize_t>(sizeof(p))) {
      if (ret == 0 || ret == -ECONNRESET) break;
      panic("read failed, ret = %ld", ret);
    }

    // Perform fake work if requested.
    if (p.workn != 0) w->Work(p.workn * 82.0);

    // Send a network request.
    ssize_t sret = c->WriteFull(&p, ret);
    if (sret != ret) {
      if (sret == -EPIPE || sret == -ECONNRESET) break;
      panic("write failed, ret = %ld", sret);
    }
  }
}

void ServerHandler(void *arg) {
  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen({0, kNetbenchPort},
				  4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=]{ServerWorker(std::unique_ptr<rt::TcpConn>(c));}).Detach();
  }
}

std::vector<double> PoissonWorker(rt::TcpConn *c, double req_rate,
                                  double service_time, rt::WaitGroup *starter)
{
  constexpr int kBatchSize = 32;

  // Seed the random generator.
  std::mt19937 g(microtime());

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
    payload rp;

    while (true) {
     ssize_t ret = c->ReadFull(&rp, sizeof(rp));
     if (ret != static_cast<ssize_t>(sizeof(rp))) {
       if (ret == 0 || ret < 0) break;
       panic("read failed, ret = %ld", ret);
     }

     barrier();
     uint64_t ts = microtime();
     barrier();
     timings.push_back(ts - start_us[rp.idx]);
    }
  });

  // Initialize timing measurement data structures.
  payload p[kBatchSize];
  int j = 0;

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
      ssize_t ret = c->WriteFull(p, sizeof(payload) * j);
      if (ret != static_cast<ssize_t>(sizeof(payload) * j))
        panic("write failed, ret = %ld", ret);
      j = 0;

      rt::Sleep(sched[i] - (microtime() - expstart));
      now = microtime();
    }
    if (now - expstart - sched[i] > kMaxCatchUpUS)
      continue;

    barrier();
    start_us[i] = microtime();
    barrier();

    // Enqueue a network request.
    p[j].idx = i;
    p[j].workn = work[i];
    p[j].tag = 0;
    j++;

    if (j >= kBatchSize || i == n - 1) {
      ssize_t ret = c->WriteFull(p, sizeof(payload) * j);
      if (ret != static_cast<ssize_t>(sizeof(payload) * j))
        panic("write failed, ret = %ld", ret);
      j = 0;
    }
  }

  c->Shutdown(SHUT_RD);
  th.Join();

  return timings;
}

std::vector<double> RunExperiment(double req_rate, double *reqs_per_sec) {
  // Create one TCP connection per thread.
  std::vector<std::unique_ptr<rt::TcpConn>> conns;
  for (int i = 0; i < threads; ++i) {
    std::unique_ptr<rt::TcpConn> outc(rt::TcpConn::Dial({0, 0}, raddr));
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
    c->Abort();

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
