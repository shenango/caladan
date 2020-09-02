extern "C" {
#include <base/cpu.h>
#include <base/log.h>
#include <base/mem.h>
}

#include <sys/shm.h>
#include <unistd.h>

#include <chrono>
#include <iostream>

#include "runtime.h"
#include "sync.h"
#include "synthetic_worker.h"
#include "thread.h"
#include "timer.h"
#include "util.h"

struct ShmMonitor {
  key_t key;
  int64_t frequency_us;
  uint64_t last_total = 0;
  int64_t next_us;
  uint64_t nlines;
  volatile uint64_t *mem;
  int64_t Poll(int64_t now);
};

#define LINESTRIDE (CACHE_LINE_SIZE / sizeof(uint64_t))

int64_t ShmMonitor::Poll(int64_t now) {
  if (now < next_us) return next_us;

  uint64_t total = 0;
  for (uint64_t i = 0; i < nlines; i++) total += mem[i * LINESTRIDE];
  printf("%x %lu %lu\n", key, total - last_total, rdtsc());
  last_total = total;
  next_us = now + frequency_us;
  return next_us;
}

void run(std::vector<ShmMonitor> &shms) {
  int64_t earliest_us, now = microtime();
  while (true) {
    earliest_us = UINT64_MAX;
    for (auto &shm : shms) earliest_us = MIN(earliest_us, shm.Poll(now));
    now = microtime();
    if (earliest_us > now + 20) {
      struct timespec req = {0, 1000L * earliest_us - now};
      nanosleep(&req, NULL);
      now = microtime();
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "usage: [<shm_key:freq_us:lines>]..." << std::endl;
    return -EINVAL;
  }

  int ret = base_init();
  if (ret) return ret;
  ret = base_init_thread();
  if (ret) return ret;

  std::vector<ShmMonitor> shms;
  for (int i = 1; i < argc; i++) {
    std::vector<std::string> conf = split(std::string(argv[i]), ':');
    if (conf.size() != 3) {
      fprintf(stderr, "Invalid conf: %s\n", argv[i]);
      return -EINVAL;
    }

    ShmMonitor shm;
    shm.key = std::stoi(conf[0], nullptr, 0);
    shm.frequency_us = shm.next_us = std::stoi(conf[1], nullptr, 0);
    shm.nlines = std::stoi(conf[2], nullptr, 0);
    shm.mem = static_cast<volatile uint64_t *>(mem_map_shm(
        shm.key, nullptr, CACHE_LINE_SIZE * shm.nlines, PGSIZE_4KB, false));
    if (shm.mem == MAP_FAILED) {
      fprintf(stderr, "Failed to map shm: %s (%d)", argv[i], errno);
      return -EINVAL;
    }
    shms.push_back(shm);
  }
  run(shms);
}
