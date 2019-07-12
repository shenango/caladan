#include "runtime.h"
#include "thread.h"
#include "sync.h"

#include <chrono>
#include <iostream>

namespace {

using us = std::chrono::duration<double, std::micro>;
constexpr int kMeasureRounds = 10000000;

void BenchSpawnJoin() {
  for (int i = 0; i < kMeasureRounds; ++i) {
    auto th = rt::Thread([](){;});
    th.Join();
  }
}

void BenchUncontendedMutex() {
  rt::Mutex m;
  volatile unsigned long foo = 0;

  for (int i = 0; i < kMeasureRounds; ++i) {
    rt::ScopedLock<rt::Mutex> l(&m);
    foo++;
  }
}

void BenchYield() {
  auto th = rt::Thread([](){
    for (int i = 0; i < kMeasureRounds / 2; ++i)
      rt::Yield();
  });

  for (int i = 0; i < kMeasureRounds / 2; ++i)
    rt::Yield();

  th.Join();
}

void BenchCondvarPingPong() {
  rt::Mutex m;
  rt::CondVar cv;
  bool dir = false; // shared and protected by @m.

  auto th = rt::Thread([&](){
    rt::ScopedLock<rt::Mutex> l(&m);
    for (int i = 0; i < kMeasureRounds / 2; ++i) {
      while (dir)
        cv.Wait(&m);
      dir = true;
      cv.Signal();
    }
  });

  rt::ScopedLock<rt::Mutex> l(&m);
  for (int i = 0; i < kMeasureRounds / 2; ++i) {
    while (!dir)
      cv.Wait(&m);
    dir = false;
    cv.Signal();
  }

  th.Join();
}

void PrintResult(std::string name, us time) {
  time /= kMeasureRounds;
  std::cout << "test '" << name << "' took "<< time.count() << " us."
            << std::endl;
}

void MainHandler(void *arg) {
  auto start = std::chrono::steady_clock::now();
  BenchSpawnJoin();
  auto finish = std::chrono::steady_clock::now();
  PrintResult("SpawnJoin",
	std::chrono::duration_cast<us>(finish - start));

  start = std::chrono::steady_clock::now();
  BenchUncontendedMutex();
  finish = std::chrono::steady_clock::now();
  PrintResult("UncontendedMutex",
    std::chrono::duration_cast<us>(finish - start));

  start = std::chrono::steady_clock::now();
  BenchYield();
  finish = std::chrono::steady_clock::now();
  PrintResult("Yield",
    std::chrono::duration_cast<us>(finish - start));

  start = std::chrono::steady_clock::now();
  BenchCondvarPingPong();
  finish = std::chrono::steady_clock::now();
  PrintResult("CondvarPingPong",
    std::chrono::duration_cast<us>(finish - start));
}

} // anonymous namespace

int main(int argc, char *argv[]) {
  int ret;

  if (argc < 2) {
    printf("arg must be config file\n");
    return -EINVAL;
  }

  ret = runtime_init(argv[1], MainHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }
  return 0;
}
