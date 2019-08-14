// interference.cc - interference related experiments

#include "runtime.h"
#include "sync.h"
#include "thread.h"
#include "timer.h"

#include "distribution.h"
#include "synthetic_worker.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <vector>

barrier_t barrier;

namespace {

// number of iterations required for 1us on target server
constexpr double kIterationsPerUS = 65.0;  // 83
// the size of each queue in entries
constexpr uint64_t kQueueSize = 4096;
// number of measurement steps to take
constexpr double kSteps = 30.0;
// measurement duration in us
constexpr double kDuration = 2000000.0;

class Queue {
 public:
  Queue(uint32_t capacity)
      : closed_(false), head_(0), tail_(0), size_(capacity), m_(), cv_() {
    q_.reserve(capacity);
  }
  ~Queue(){};

  // Enqueues a work unit into the queue (returns true if queue is nonfull).
  bool Enqueue(work_unit w) {
    rt::ScopedLock<rt::Mutex> l(&m_);
    if (head_ - tail_ >= size_) return false;
    q_[head_++ % size_] = w;
    cv_.Signal();
    return true;
  }

  // Dequeues a work unit from the queue (returns true if not closed).
  bool Dequeue(work_unit *w, bool block) {
    rt::ScopedLock<rt::Mutex> l(&m_);
    while (block && head_ == tail_ && !closed_) cv_.Wait(&m_);
    if (head_ == tail_ && (!block || closed_)) return false;
    *w = q_[tail_++ % size_];
    return true;
  }

  // Closes the queue, waking all threads blocked in Dequeue().
  void Close() {
    rt::ScopedLock<rt::Mutex> l(&m_);
    closed_ = true;
    cv_.SignalAll();
  }

 private:
  bool closed_;
  uint32_t head_;
  uint32_t tail_;
  const uint32_t size_;
  rt::Mutex m_;
  rt::CondVar cv_;
  std::vector<work_unit> q_;
};

std::vector<work_unit> RtcWorker(Queue *q, Timer *t, rt::WaitGroup *wg,
                                 int max_work) {
  std::unique_ptr<SyntheticWorker> worker(
      SyntheticWorkerFactory("stridedmem:3200000:64"));
  std::vector<work_unit> wv;
  wv.reserve(max_work);

  wg->Done();
  wg->Wait();

  work_unit w;
  while (q->Dequeue(&w, true)) {
    worker->Work(static_cast<uint64_t>(w.work_us * kIterationsPerUS));
    w.duration_us = t->Elapsed() - w.start_us;
    wv.emplace_back(w);
  }

  return wv;
}

std::vector<work_unit> GenerateWork(double offered_rps, double duration,
                                    Distribution *sd, int cpu) {
  std::mt19937 rg(rand());
  std::exponential_distribution<double> rd(1.0 / (1000000.0 / offered_rps));
  return GenerateWork(std::bind(rd, rg), sd, 0, duration, cpu);
}

std::vector<work_unit> RunExperiment(double offered_rps, Distribution *sd,
                                     int workers, bool dfcfs) {
  Timer t;
  rt::WaitGroup wg(workers + 1);
  rt::Thread threads[workers];
  std::vector<work_unit> wq;
  std::vector<work_unit> samples[workers];

  // initialize the queues
  std::vector<std::unique_ptr<Queue>> qs;
  if (!dfcfs) {
    qs.emplace_back(new Queue(kQueueSize * workers));
    wq = GenerateWork(offered_rps, kDuration, sd, 0);
  } else {
    for (int i = 0; i < workers; ++i) {
      qs.emplace_back(new Queue(kQueueSize));
      auto tmp = GenerateWork(offered_rps / static_cast<double>(workers),
                              kDuration, sd, i);
      wq.insert(wq.end(), tmp.begin(), tmp.end());
    }
    std::sort(wq.begin(), wq.end(), [](const work_unit &s1, work_unit &s2) {
      return s1.start_us < s2.start_us;
    });
  }
  int max_work = div_up(wq.size(), workers);

  // create a thread per worker
  for (int i = 0; i < workers; ++i) {
    Queue *q = dfcfs ? qs[i].get() : qs[0].get();
    threads[i] = rt::Thread(
        [&, q, i]() { samples[i] = RtcWorker(q, &t, &wg, max_work); });
  }

  // try to get a clean start time
  wg.Done();
  wg.Wait();
  t.Reset();

  // generate load
  for (auto w : wq) {
    t.SpinUntil(w.start_us);
    qs[dfcfs ? w.cpu : 0]->Enqueue(w);
  }
  for (auto &q : qs) q->Close();

  // wait for the workers to finish running
  for (int i = 0; i < workers; ++i) threads[i].Join();

  // aggregate all the samples together
  std::vector<work_unit> result;
  for (int i = 0; i < workers; ++i) {
    auto &v = samples[i];
    result.insert(result.end(), v.begin(), v.end());
  }
  return result;
}

void PrintResults(double offered_rps, double duration,
                  std::vector<work_unit> w) {
  // remove any request that didn't complete during the duration
  w.erase(std::remove_if(w.begin(), w.end(),
                         [duration](const work_unit &s) {
                           return s.duration_us + s.start_us > duration;
                         }),
          w.end());

  // sort all results by completion duration
  std::sort(w.begin(), w.end(), [](const work_unit &s1, work_unit &s2) {
    return s1.duration_us < s2.duration_us;
  });

  // calculate various statistics on the results
  double count = static_cast<double>(w.size());
  double achieved_rps = count / (duration / 1000000.0);
  double sum = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.duration_us; });
  double mean = sum / count;
  double p90 = w[count * 0.9].duration_us;
  double p99 = w[count * 0.99].duration_us;
  double p999 = w[count * 0.999].duration_us;
  double p9999 = w[count * 0.9999].duration_us;
  double min = w[0].duration_us;
  double max = w[w.size() - 1].duration_us;

  static bool first = true;
  if (first) {
	  first = false;
	  std::cout << "offered_rps,achieved_rps,count,min,mean,p90,p99,"
		  "p999,p9999,max" << std::endl;
  }

  // print out the results
  std::cout << std::setprecision(4) << std::fixed << offered_rps << ","
            << achieved_rps << "," << count << "," << min << "," << mean << ","
            << p90 << "," << p99 << "," << p999 << "," << p9999 << "," << max
            << std::endl;
}

int MainHandler(int argc, char *argv[]) {
  bool dfcfs;
  std::string tmp(argv[2]);
  if (tmp == "dFCFS")
    dfcfs = true;
  else if (tmp == "cFCFS")
    dfcfs = false;
  else
    return -EINVAL;

  std::unique_ptr<Distribution> sd(DistributionFactory(argv[3]));
  if (!sd) return -EINVAL;
  if (rt::RuntimeGuaranteedCores() < 2) return -EINVAL;

  int workers = rt::RuntimeGuaranteedCores() - 1;
  double max_rps = 1000000.0 / sd->Mean() * workers * 2.0;
  if (max_rps == 0) max_rps = 5000000.0;

  for (double rps = max_rps / kSteps; rps <= max_rps; rps += max_rps / kSteps) {
    auto w = RunExperiment(rps, sd.get(), workers, dfcfs);
    PrintResults(rps, kDuration, w);
  }

  return 0;
}

}  // anonymous namespace

int main(int argc, char *argv[]) {
  if (argc != 4) {
    std::cerr << "usage: [cfg_file] [dFCFS|cFCFS] [work_dist]" << std::endl;
    return -EINVAL;
  }

  int ret = rt::RuntimeInit(argv[1], [argc, argv] { MainHandler(argc, argv); });
  if (ret) {
    std::cerr << "failed to start runtime\n" << std::endl;
    return ret;
  }

  return 0;
}
