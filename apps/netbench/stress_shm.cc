extern "C" {
#include <base/log.h>
}

#include "runtime.h"
#include "sync.h"
#include "synthetic_worker.h"
#include "thread.h"
#include "timer.h"

#include <sys/shm.h>
#include <unistd.h>
#include <chrono>
#include <iostream>

#define SHM_KEY (0x123)

barrier_t barrier;
bool use_barrier = false;
bool synth_barrier_wait() { return barrier_wait(&barrier); }
namespace {

int threads;
uint64_t n;
std::string worker_spec;

void MainHandler(void *arg) {
  uint64_t *cnt;
  int shmid =
      shmget((key_t)SHM_KEY, sizeof(uint64_t) * threads * 8, 0777 | IPC_CREAT);
  void *shm = NULL;
  shm = shmat(shmid, 0, 0);
  cnt = (uint64_t *)shm;

  rt::WaitGroup wg(1);
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
        cnt[i * 8]++;
        if (use_barrier)
          synth_barrier_wait();
        else
          rt::Yield();
      }
    });
  }

  // never returns
  wg.Wait();
}

}  // anonymous namespace

void PrintUsage() {
  std::cerr
      << "usage: [config_file] [#threads] [#n] [worker_spec] <use_barrier>"
      << std::endl;
}

int main(int argc, char *argv[]) {
  int ret;

  if (argc < 5) {
    PrintUsage();
    return -EINVAL;
  }

  threads = std::stoi(argv[2], nullptr, 0);
  n = std::stoul(argv[3], nullptr, 0);
  worker_spec = std::string(argv[4]);

  if (argc > 5) {
    if (std::string(argv[5]) != "use_barrier") {
      PrintUsage();
      return -EINVAL;
    }
    use_barrier = true;
  }

  ret = runtime_init(argv[1], MainHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
