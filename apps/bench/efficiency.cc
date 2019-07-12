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

#include <iostream>
#include <utility>
#include <memory>
#include <chrono>

namespace {

using sec = std::chrono::duration<double>;

// The netperf server responds to this port.
constexpr uint64_t kNetperfPort = 8000;

// the number of worker threads to spawn.
int threads;
// the callibrated number of fake work iterations for 1us of fake work.
uint64_t n;
// the fake work specification.
std::string worker_spec;
// the remote UDP address of the server.
netaddr raddr;
// the time in seconds of each measurement.
int measure_sec;
// the step size in number of microseconds of fake work.
int step_us;
// the maximum number of microseconds of fake work to measure.
int end_us;

uint64_t Worker(rt::UdpConn *c, int cur_us, rt::WaitGroup *wg) {
  constexpr std::size_t kPayloadLen = 32;
  unsigned char buf[kPayloadLen];

  std::unique_ptr<FakeWorker> w(FakeWorkerFactory(worker_spec));
  if (unlikely(w == nullptr)) panic("couldn't create worker");

  uint64_t requests = 0;

  while (true) {
    // Do fake work.
    for (int i = 0; i < cur_us; ++i)
      w->Work(n);

    // Send a network request.
    ssize_t ret = c->Write(buf, sizeof(buf));
    if (ret != sizeof(buf)) {
      if (ret == -EPIPE) break;
      panic("udp write failed, ret = %ld", ret);
    }

    // Receive a network response.
    ret = c->Read(buf, sizeof(buf));
    if (ret != sizeof(buf)) {
      if (ret == 0) break;
      panic("udp read failed, ret = %ld", ret);
    }

    requests += 1;
  }

  wg->Done();
  return requests;
}

void MainHandler(void *arg) {
  for (int cur_us = step_us; cur_us <= end_us; cur_us += step_us) {
    std::vector<std::pair<std::unique_ptr<rt::UdpConn>, uint64_t>> conns;
    rt::WaitGroup wg(threads);

    // Open one UDP connection per thread.
    for (int i = 0; i < threads; ++i) {
      netaddr laddr = {0, 0};
      std::unique_ptr<rt::UdpConn> c(rt::UdpConn::Dial(laddr, raddr));
      if (unlikely(c == nullptr)) panic("couldn't connect to raddr.");
      conns.emplace_back(std::move(c), 0);
    }

    auto start = std::chrono::steady_clock::now();

    // Launch a worker thread for each connection.
    for (auto& c: conns)
      rt::Spawn([&](){c.second = Worker(c.first.get(), cur_us, &wg);});

    // Sleep for the experiment measurement duration.
    rt::Sleep(measure_sec * rt::kSeconds);

    // Shutdown all the connections.
    for (auto& c: conns)
      c.first->Shutdown();

    // Wait until all the threads have terminated.
    wg.Wait();

    auto finish = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration_cast<sec>(finish - start).count();

    uint64_t reqs = 0;
    for (auto& c: conns)
      reqs += c.second;

    double reqs_per_sec = static_cast<double>(reqs) / elapsed;
    double ideal_reqs_per_sec = 8 * 1000000 / static_cast<double>(cur_us);
    double efficiency = reqs_per_sec / ideal_reqs_per_sec * 100;
    std::cout << cur_us << " " << reqs_per_sec << " " << efficiency << std::endl;
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

  if (argc != 9) {
    std::cerr << "usage: [config_file] [#threads] [#n] [worker_spec] "
		 "[rip:rport] [measure_sec] [step_us] [end_us]"
              << std::endl;
    return -EINVAL;
  }

  threads = std::stoi(argv[2], nullptr, 0);
  n = std::stoul(argv[3], nullptr, 0);
  worker_spec = std::string(argv[4]);

  ret = StringToAddr(argv[5], &raddr.ip);
  if (ret) return -EINVAL;
  raddr.port = kNetperfPort;

  measure_sec = std::stoi(argv[6], nullptr, 0);
  step_us = std::stoi(argv[7], nullptr, 0);
  end_us = std::stoi(argv[8], nullptr, 0);

  ret = runtime_init(argv[1], MainHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
