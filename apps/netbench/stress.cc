extern "C" {
#include <base/log.h>
}

#include "runtime.h"
#include "sync.h"
#include "synthetic_worker.h"
#include "thread.h"
#include "timer.h"

#include <chrono>
#include <iostream>

barrier_t barrier;

bool synth_barrier_wait() { return barrier_wait(&barrier); }

namespace {

int threads;
uint64_t n;
std::string worker_spec;

void MainHandler(void *arg) {
  rt::WaitGroup wg(1);
  std::vector<uint64_t> cnt(threads);

  barrier_init(&barrier, threads);

  for (int i = 0; i < threads; ++i) {
    rt::Spawn([&, i]() {
      auto *w = SyntheticWorkerFactory(worker_spec);
      if (w == nullptr) {
        std::cerr << "Failed to create worker." << std::endl;
        exit(1);
      }

      while (true) {
        w->Work(n);
        cnt[i]++;
        rt::Yield();
      }
    });
  }

  rt::Spawn([&]() {
    uint64_t last_total = 0;
    auto last = std::chrono::steady_clock::now();
    while (1) {
      rt::Sleep(rt::kSeconds);
      auto now = std::chrono::steady_clock::now();
      uint64_t total = 0;
      double duration =
          std::chrono::duration_cast<std::chrono::duration<double>>(now - last)
              .count();
      for (int i = 0; i < threads; i++) total += cnt[i];
      preempt_disable();
      log_info("%f", static_cast<double>(total - last_total) / duration);
      preempt_enable();
      last_total = total;
      last = now;
    }
  });

  // never returns
  wg.Wait();
}

}  // anonymous namespace

int main(int argc, char *argv[]) {
  int ret;

  if (argc != 5) {
    std::cerr << "usage: [config_file] [#threads] [#n] [worker_spec]"
              << std::endl;
    return -EINVAL;
  }

  threads = std::stoi(argv[2], nullptr, 0);
  n = std::stoul(argv[3], nullptr, 0);
  worker_spec = std::string(argv[4]);

  ret = runtime_init(argv[1], MainHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
