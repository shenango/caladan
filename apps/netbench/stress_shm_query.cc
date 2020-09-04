extern "C" {
#include <base/cpu.h>
#include <base/log.h>
#include <base/mem.h>
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <iostream>

#include "../../iokernel/ksched.h"
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

struct MemBwMonitor {
  int pci_cfg_fd = -1;
  volatile unsigned int *low_cas_count_all_ptr;
  int64_t next_us;
  int64_t frequency_us;
  unsigned int last_read;
  double mbps_mult;
  volatile uint64_t *mem;
  int64_t Poll(int64_t now);
  int Init(uint64_t freq_us);
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

int64_t MemBwMonitor::Poll(int64_t now) {
  if (pci_cfg_fd < 0) return INT64_MAX;
  if (now < next_us) return next_us;

  unsigned int cur = *low_cas_count_all_ptr;
  printf("mem %.2f %lu\n", (double)(cur - last_read) * mbps_mult, rdtsc());
  last_read = cur;

  next_us = now + frequency_us;
  return next_us;
}

int MemBwMonitor::Init(uint64_t freq_us) {
  pci_cfg_fd = open("/dev/pcicfg", O_RDWR);
  if (pci_cfg_fd < 0) return -errno;
  char *pci_cfg_base_ptr = (char *)mmap(
      NULL, PCI_CFG_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, pci_cfg_fd, 0);
  unsigned int *ctrl_ptr =
      (unsigned int *)(pci_cfg_base_ptr +
                       pci_cfg_index(SOCKET0_IMC_BUS_NO, SOCKET0_IMC_DEVICE_NO,
                                     SOCKET0_CHANNEL0_IMC_FUNC_NO,
                                     MC_CHy_PCI_PMON_CTL0));
  *ctrl_ptr = (EVENT_CODE_CAS_COUNT << MC_CHy_PCI_PMON_CTL_ENV_SEL_SHIFT) |
              (UMASK_CAS_COUNT_ALL << MC_CHy_PCI_PMON_CTL_UMASK_SHIFT) |
              (1 /* enabled */ << MC_CHy_PCI_PMON_CTL_ENABLE_SHIFT);
  low_cas_count_all_ptr =
      (unsigned int *)(pci_cfg_base_ptr +
                       pci_cfg_index(SOCKET0_IMC_BUS_NO, SOCKET0_IMC_DEVICE_NO,
                                     SOCKET0_CHANNEL0_IMC_FUNC_NO,
                                     MC_CHy_PCI_PMON_CTR0_LOW));

  frequency_us = next_us = freq_us;
  mbps_mult = 2.0 * 64.0 / (double)freq_us;
  return 0;
}

void run(std::vector<ShmMonitor> &shms, MemBwMonitor &mem) {
  int64_t earliest_us, now = microtime();
  while (true) {
    earliest_us = UINT64_MAX;
    for (auto &shm : shms) earliest_us = MIN(earliest_us, shm.Poll(now));
    earliest_us = MIN(earliest_us, mem.Poll(now));
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
    std::cerr << "usage: [membw:freq_us] [shm_key:freq_us:lines]..."
              << std::endl;
    return -EINVAL;
  }

  int ret = base_init();
  if (ret) return ret;
  ret = base_init_thread();
  if (ret) return ret;

  std::vector<ShmMonitor> shms;
  MemBwMonitor mem;
  for (int i = 1; i < argc; i++) {
    std::vector<std::string> conf = split(std::string(argv[i]), ':');

    if (conf[0] == "membw") {
      mem.Init(std::stoi(conf[1], nullptr, 0));
      continue;
    }

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
  run(shms, mem);
}
