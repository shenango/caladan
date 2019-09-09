extern "C" {
#include <base/log.h>
}

#include "runtime.h"
#include "sync.h"
#include "thread.h"
#include "timer.h"

#include <chrono>
#include <iostream>
#include <sys/shm.h>
#include <unistd.h>

barrier_t barrier;

#define SHM_KEY (0x123)
#define CACHELINE 64

int main(int argc, char *argv[]) {
	if (argc != 2) {
		std::cerr << "usage: [#threads]" << std::endl;
		return -EINVAL;
	}

	int threads = std::stoi(argv[1], nullptr, 0);

	int ret = base_init();
	if (ret) {
		fprintf(stderr, "failed to init base, ret = %d\n", ret);
		return ret;
	}

	volatile double *cnt;
	int shmid = shmget((key_t)SHM_KEY, CACHELINE * threads, 0666 | IPC_CREAT);
	void *shm = NULL;
	shm = shmat(shmid, 0, 0);
	cnt = (volatile double *)shm;

	uint64_t last_total = 0;
	auto last = std::chrono::steady_clock::now();
	while (1) {
		sleep(1);
		auto now = std::chrono::steady_clock::now();
		uint64_t total = 0;
		double duration =
			std::chrono::duration_cast<std::chrono::duration<double>>(now - last)
			.count();
		for (int i = 0; i < threads; i++) {
			total += cnt[i * CACHELINE / sizeof(double)];
		}
		log_info("mops: %lf, timestamp: %lu",
			 static_cast<double>(total - last_total) / 1E6 / duration,
			 (unsigned long)time(NULL));
		last_total = total;
		last = now;
	}

	return 0;
}
