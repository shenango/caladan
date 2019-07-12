#include <stdio.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <runtime/runtime.h>
#include <runtime/sync.h>

#define N		50000
#define NCORES	4

static void work_handler(void *arg)
{
	waitgroup_t *wg_parent = (waitgroup_t *)arg;
	waitgroup_done(wg_parent);
	waitgroup_wait(wg_parent);
}

static void main_handler(void *arg)
{
	waitgroup_t wg;
	double threads_per_second;
	uint64_t start_us;
	int i, ret;

	log_info("started main_handler() thread");

	waitgroup_init(&wg);
	waitgroup_add(&wg, N);
	start_us = microtime();
	for (i = 0; i < N; i++) {
		ret = thread_spawn(work_handler, &wg);
		BUG_ON(ret);
		thread_yield();
	}

	waitgroup_wait(&wg);
	threads_per_second = (double)N /
			     ((microtime() - start_us) * 0.000001);
	log_info("spawned %f threads / second", threads_per_second);
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc < 2) {
		printf("arg must be config file\n");
		return -EINVAL;
	}

	ret = runtime_init(argv[1], main_handler, NULL);
	if (ret) {
		printf("failed to start runtime");
		return ret;
	}

	return 0;
}
