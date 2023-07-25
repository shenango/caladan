
#include "synthetic_worker.h"

#include <chrono>
#include <iostream>
#include <thread>

extern "C" {
#include <base/init.h>
}

namespace {

bool use_barrier = false;
pthread_barrier_t barrier;

int threads;
uint64_t n;
std::string worker_spec;

void MainHandler(void *arg) {
  std::vector<uint64_t> cnt(threads);

  for (int i = 0; i < threads; ++i) {
    std::thread([i, &cnt]() {
      int ret = base_init_thread();
      BUG_ON(ret);
      auto *w = SyntheticWorkerFactory(worker_spec);
      if (w == nullptr) {
        std::cerr << "Failed to create worker." << std::endl;
        exit(1);
      }

      while (true) {
        w->Work(n);
        cnt[i]++;
        if (use_barrier) pthread_barrier_wait(&barrier);
      }
    })
        .detach();
  }

  std::thread([&]() {
    uint64_t last_total = 0;
    auto last = std::chrono::steady_clock::now();
    while (1) {
      std::chrono::seconds sec(1);
      std::this_thread::sleep_for(sec);
      auto now = std::chrono::steady_clock::now();
      uint64_t total = 0;
      double duration =
          std::chrono::duration_cast<std::chrono::duration<double>>(now - last)
              .count();
      for (int i = 0; i < threads; i++) total += cnt[i];
      std::cerr << static_cast<double>(total - last_total) / duration
                << std::endl;
      last_total = total;
      last = now;
    }
  })
      .join();

  // never returns
}

}  // anonymous namespace

void PrintUsage() {
  std::cerr << "usage: [#threads] [#n] [worker_spec] <use_barrier>"
            << std::endl;
}

int main(int argc, char *argv[]) {
  int ret;
  if (argc < 4) {
    PrintUsage();
    return -EINVAL;
  }

  threads = std::stoi(argv[1], nullptr, 0);
  n = std::stoul(argv[2], nullptr, 0);
  worker_spec = std::string(argv[3]);

  pthread_barrier_init(&barrier, NULL, threads);

  if (argc > 4) {
    if (std::string(argv[4]) != "use_barrier") {
      PrintUsage();
      return -EINVAL;
    }
    use_barrier = true;
  }

  ret = base_init();
  if (ret) return ret;

  ret = base_init_thread();
  if (ret) return ret;

  MainHandler(NULL);

  return 0;
}
