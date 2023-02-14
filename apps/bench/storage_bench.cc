extern "C" {
#include <base/log.h>
#include <runtime/storage.h>
}

#include "net.h"
#include "runtime.h"
#include "sync.h"
#include "thread.h"
#include "timer.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>

namespace {

using namespace std::chrono;
using sec = duration<double, std::micro>;

// <- ARGUMENTS FOR EXPERIMENT ->
// the number of worker threads to spawn.
int threads;
size_t block_count;
size_t pct_set;
double total_block_count = 547002288.0;
size_t us_per_sample;
size_t nsamples;

// The maximum lateness to tolerate before dropping egress samples.
constexpr uint64_t kMaxCatchUpUS = 5;

struct work_unit {
  double start_us;
  size_t lba;
  bool is_set;
  double duration_us;
};

struct uptime {
  uint64_t idle;
  uint64_t busy;
};

uptime ReadUptime() {
    std::ifstream file("/proc/stat");
    std::string line;
    std::getline(file, line);
    std::istringstream ss(line);
    std::string tmp;
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal, guest,
             guest_nice;
    ss >> tmp >> user >> nice >> system >> idle >> iowait >> irq >> softirq
       >> steal >> guest >> guest_nice;
    return {idle + iowait,
                user + nice + system + irq + softirq + steal};
}


template <class Arrival, class Service>
std::vector<work_unit> GenerateWork(Arrival a, Service s, double cur_us,
                                    double last_us) {
  std::vector<work_unit> w;
  while (cur_us < last_us) {
    cur_us += a();
    auto set = s() % 100 < pct_set;
    w.emplace_back(work_unit{cur_us, s() & ~0x7, set, 0});
  }
  return w;
}


std::vector<work_unit> ClientWorker(
    rt::WaitGroup *starter, std::function<std::vector<work_unit>()> wf) {
  std::vector<work_unit> w(wf());
  std::vector<time_point<steady_clock>> timings;
  timings.reserve(w.size());

  // Start the receiver thread.
  // Synchronized start of load generation.
  starter->Done();
  starter->Wait();
  barrier();
  auto expstart = steady_clock::now();
  barrier();

  auto wsize = w.size();
  rt::WaitGroup wg(wsize);
  int dropped = 0;

  for (unsigned int i = 0; i < wsize; ++i) {
    barrier();
    auto now = steady_clock::now();
    barrier();
    if (duration_cast<sec>(now - expstart).count() < w[i].start_us)
      rt::Sleep(w[i].start_us - duration_cast<sec>(now - expstart).count());
    now = steady_clock::now();
    if (duration_cast<sec>(now - expstart).count() - w[i].start_us >
        kMaxCatchUpUS) {
      dropped++;
      continue;
    }

    barrier();
    timings[i] = steady_clock::now();
    barrier();
    rt::Spawn([&, i] {
      int ret;
      unsigned char dat[block_count * 512];

      auto now = steady_clock::now();

      if (duration_cast<sec>(now - expstart).count() - w[i].start_us >
          kMaxCatchUpUS) {
        wg.Done();
        return;
      }

      if (w[i].is_set) {
        ret = storage_write(dat, w[i].lba, block_count);
      } else {
        ret = storage_read(dat, w[i].lba, block_count);
      }

      barrier();
      auto ts = steady_clock::now();
      barrier();
      if (ret == 0)
        w[i].duration_us = duration_cast<sec>(ts - timings[i]).count();
      wg.Done();
    });
  }

  wg.Add(-1 * dropped);
  wg.Wait();

  return w;
}

std::vector<work_unit> RunExperiment(
    int threads, double *reqs_per_sec, double *cpu_usage, uint64_t *start_wct,
    std::function<std::vector<work_unit>()> wf) {
  // Launch a worker thread for each connection.
  rt::WaitGroup starter(threads + 1);
  std::vector<rt::Thread> th;
  std::unique_ptr<std::vector<work_unit>> samples[threads];
  for (int i = 0; i < threads; ++i) {
    th.emplace_back(rt::Thread([&, i] {
      auto v = ClientWorker(&starter, wf);
      samples[i].reset(new std::vector<work_unit>(std::move(v)));
    }));
  }

  // Give the workers time to initialize, then start recording.
  starter.Done();
  starter.Wait();

  // |--- start experiment duration timing ---|
  barrier();
  auto start = steady_clock::now();
  auto start_sys = system_clock::now();
  barrier();
  uptime u1 = ReadUptime();

  // Wait for the workers to finish.
  for (auto &t : th) t.Join();

  // |--- end experiment duration timing ---|
  barrier();
  auto finish = steady_clock::now();
  barrier();
  uptime u2 = ReadUptime();

  // Aggregate all the samples together.
  std::vector<work_unit> w;
  for (int i = 0; i < threads; ++i) {
    auto &v = *samples[i];
    w.insert(w.end(), v.begin(), v.end());
  }

  // Remove requests that did not complete.
  w.erase(std::remove_if(w.begin(), w.end(),
                         [](const work_unit &s) { return s.duration_us == 0; }),
          w.end());

  // Report results.
  double elapsed = duration_cast<sec>(finish - start).count();
  if (reqs_per_sec != nullptr)
    *reqs_per_sec = static_cast<double>(w.size()) / elapsed * 1000000;
  uint64_t idle = u2.idle - u1.idle;
  uint64_t busy = u2.busy - u1.busy;
  if (cpu_usage != nullptr)
    *cpu_usage = static_cast<double>(busy) /
                 static_cast<double>(idle + busy);
  if (start_wct != nullptr)
    *start_wct = duration_cast<sec>(start_sys.time_since_epoch()).count() / 1000000;
  return w;
}

void PrintStatResults(std::vector<work_unit> w, double offered_rps, double rps,
                      double cpu_usage, uint64_t start_wct) {
  std::sort(w.begin(), w.end(), [](const work_unit &s1, work_unit &s2) {
    return s1.duration_us < s2.duration_us;
  });
  double sum = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.duration_us; });
  double mean = sum / w.size();
  double count = static_cast<double>(w.size());
  double p90 = w[count * 0.9].duration_us;
  double p99 = w[count * 0.99].duration_us;
  double p999 = w[count * 0.999].duration_us;
  double p9999 = w[count * 0.9999].duration_us;
  double min = w[0].duration_us;
  double max = w[w.size() - 1].duration_us;
  std::cout  //<<
             //"#threads,offered_rps,rps,cpu_usage,samples,min,mean,p90,p99,p999,p9999,max"
             //<< std::endl
      << std::setprecision(4) << std::fixed << threads << "," << offered_rps
      << "," << rps << "," << cpu_usage << "," << w.size() << "," << min << ","
      << mean << "," << p90 << "," << p99 << "," << p999 << "," << p9999 << ","
      << max << "," << start_wct << std::endl;
}

void SteadyStateExperiment(int threads, double offered_rps,
                           double service_time) {
  double rps, cpu_usage;
  uint64_t start_wct;
  std::vector<work_unit> w = RunExperiment(threads, &rps, &cpu_usage, &start_wct, [=] {
    std::mt19937 rg(rand());
    std::mt19937 dg(rand());
    std::exponential_distribution<double> rd(
        1.0 / (1000000.0 / (offered_rps / static_cast<double>(threads))));
    std::uniform_int_distribution<size_t> wd(0.0, total_block_count);
    return GenerateWork(std::bind(rd, rg), std::bind(wd, dg), 0, us_per_sample);
  });

  // Print the results.
  PrintStatResults(w, offered_rps, rps, cpu_usage, start_wct);
}

void ClientHandler(void *arg) {
  double max_pps = 600000;
  double step = max_pps / nsamples;
  for (double i = step; i <= max_pps; i += step) {
    SteadyStateExperiment(threads, i, 0);
  }
}

}  // anonymous namespace

int main(int argc, char *argv[]) {
  int ret;

  if (argc < 7) {
    std::cerr << "usage: [cfg_file] [#threads] [block_count] [pct_set]"
              << " [us_per_sample] [nsamples]"
              << std::endl;
    return -EINVAL;
  }

  threads = std::stoi(argv[2], nullptr, 0);
  block_count = std::stoi(argv[3], nullptr, 0);
  pct_set = std::stoi(argv[4], nullptr, 0);
  us_per_sample = std::stoi(argv[5], nullptr, 0);
  nsamples = std::stoi(argv[6], nullptr, 0);

  ret = runtime_init(argv[1], ClientHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
