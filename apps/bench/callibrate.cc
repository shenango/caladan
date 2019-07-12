#include "runtime.h"
#include "fake_worker.h"

#include <memory>
#include <iostream>
#include <chrono>

namespace {

using us = std::chrono::duration<double, std::micro>;
constexpr int kMultiply = 100000;

void Measure(FakeWorker *w, double target) {
  double elapsed;
  uint64_t i = 1;

  do {
    i *= 2;
    auto start = std::chrono::steady_clock::now();
    for (int j = 0; j < kMultiply; j++) w->Work(i);
    auto finish = std::chrono::steady_clock::now();
    elapsed = std::chrono::duration_cast<us>(finish - start).count();
  } while (elapsed < target * kMultiply);

  while (elapsed > target * kMultiply) {
    --i;
    auto start = std::chrono::steady_clock::now();
    for (int j = 0; j < kMultiply; j++) w->Work(i);
    auto finish = std::chrono::steady_clock::now();
    elapsed = std::chrono::duration_cast<us>(finish - start).count();
  }

  std::cout << i << " iterations took " << elapsed / kMultiply << " us."
            << std::endl;
}

} // anonymous namespace

int main(int argc, char *argv[]) {
  if (argc != 3) {
    std::cerr << "usage: [microseconds (double)] [worker_spec]" << std::endl;
    return 1;
  }

  FakeWorker *w = FakeWorkerFactory(argv[2]);
  if (!w) {
    std::cerr << "Invalid worker argument." << std::endl;
    return 1;
  }
  Measure(w, std::stod(argv[1], nullptr));

  return 0;
}
