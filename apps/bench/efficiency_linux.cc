extern "C" {
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
}

#include "fake_worker.h"

#include <iostream>
#include <utility>
#include <memory>
#include <chrono>
#include <thread>

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
uint32_t ip;
uint16_t port;
// the time in seconds of each measurement.
int measure_sec;
// the step size in number of microseconds of fake work.
int step_us;
// the maximum number of microseconds of fake work to measure.
int end_us;

uint64_t Worker(int fd, int cur_us) {
  constexpr std::size_t kPayloadLen = 32;
  unsigned char buf[kPayloadLen];

  std::unique_ptr<FakeWorker> w(FakeWorkerFactory(worker_spec));
  if (w == nullptr) {
    printf("couldn't create worker");
    exit(1);
  }

  uint64_t requests = 0;

  while (true) {
    // Do fake work.
    for (int i = 0; i < cur_us; ++i)
      w->Work(n);

    // Send a network request.
    ssize_t ret = write(fd, buf, sizeof(buf));
    if (ret != sizeof(buf)) {
      if (-errno == -EPIPE) break;
      break;
      printf("udp write failed, ret = %ld", ret);
      exit(1);
    }

    // Receive a network response.
    ret = read(fd, buf, sizeof(buf));
    if (ret != sizeof(buf)) {
      if (ret == 0) break;
      printf("udp read failed, ret = %ld", ret);
      exit(1);
    }

    requests += 1;
  }

  close(fd);
  return requests;
}

void MainHandler(void *arg) {
  for (int cur_us = step_us; cur_us <= end_us; cur_us += step_us) {
    std::vector<std::pair<int, uint64_t>> conns;
    std::vector<std::thread> threadv;

    // Open one UDP connection per thread.
    for (int i = 0; i < threads; ++i) {
      struct sockaddr_in addr;
      int fd;

      if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        printf("socket() failed %d", -errno);
        exit(1);
      }

      memset((char *)&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(ip);
      addr.sin_port = htons(port);

      if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("connect() failed %d", -errno);
        exit(1);
      }

      conns.emplace_back(fd, 0);
    }

    auto start = std::chrono::steady_clock::now();

    // Launch a worker thread for each connection.
    for (auto& c: conns)
      threadv.emplace_back([&](){c.second = Worker(c.first, cur_us);});

    // Sleep for the experiment measurement duration.
    sleep(measure_sec);

    // Shutdown all the connections.
    for (auto& c: conns)
      shutdown(c.first, SHUT_RDWR);

    // Wait until all the threads have terminated.
    for (auto& t: threadv)
      t.join();

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

#define MAKE_IP_ADDR(a, b, c, d)                        \
        (((uint32_t) a << 24) | ((uint32_t) b << 16) |  \
         ((uint32_t) c << 8) | (uint32_t) d)

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

  ret = StringToAddr(argv[5], &ip);
  if (ret) return -EINVAL;
  port = kNetperfPort;

  measure_sec = std::stoi(argv[6], nullptr, 0);
  step_us = std::stoi(argv[7], nullptr, 0);
  end_us = std::stoi(argv[8], nullptr, 0);

  MainHandler(nullptr);

  return 0;
}
