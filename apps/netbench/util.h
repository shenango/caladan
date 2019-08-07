// util.h - a collection of shared utilities

#pragma once

#include "timer.h"

#include <chrono>
#include <string>
#include <tuple>
#include <vector>

using namespace std::chrono;

struct work_unit {
  double start_us, work_us, duration_us;
  int cpu;
};

template <class Arrival, class Service>
std::vector<work_unit> GenerateWork(Arrival a, Service s, double cur_us,
                                    double last_us, int cpu) {
  std::vector<work_unit> w;
  while (cur_us < last_us) {
    cur_us += a();
    w.emplace_back(work_unit{cur_us, s(), 0, cpu});
  }
  return w;
}

template <class Arrival, class Service>
std::vector<work_unit> GenerateWork(Arrival a, Service *s, double cur_us,
                                    double last_us, int cpu) {
  std::vector<work_unit> w;
  while (cur_us < last_us) {
    cur_us += a();
    w.emplace_back(work_unit{cur_us, (*s)(), 0, cpu});
  }
  return w;
}

std::vector<std::string> split(const std::string &text, char sep);

class Timer {
 public:
  using micro = duration<double, std::micro>;

  Timer() {
    barrier();
    start_ts_ = steady_clock::now();
    barrier();
  }
  ~Timer(){};

  // Reset the timer start time.
  void Reset() {
    barrier();
    start_ts_ = steady_clock::now();
    barrier();
  }

  // Returns the microseconds elapsed since the timer was constructed.
  double Elapsed() {
    barrier();
    auto now = steady_clock::now();
    barrier();
    return duration_cast<micro>(now - start_ts_).count();
  }

  // Busy spin until the deadline (in microseconds) passes.
  void SpinUntil(double deadline) {
    while (Elapsed() < deadline) cpu_relax();
  }

  // Sleep until the deadline (in microseconds) passes.
  void SleepUntil(double deadline) {
    double diff = deadline - Elapsed();
    if (diff <= 0) return;
    rt::Sleep(static_cast<uint64_t>(diff));
  }

 private:
  time_point<steady_clock> start_ts_;
};
