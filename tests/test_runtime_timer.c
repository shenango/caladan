/*
 * test_runtime_thread.c - tests basic thread spawning
 */

#include <stdio.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <runtime/runtime.h>
#include <runtime/sync.h>
#include <runtime/timer.h>

#define WORKERS		1000
#define N		100000

static void work_handler(void *arg)
{
	waitgroup_t *wg_parent = (waitgroup_t *)arg;
	int i;

	for (i = 0; i < N; i++)
		timer_sleep(2);

	waitgroup_done(wg_parent);
}

static void main_handler(void *arg)
{
	waitgroup_t wg;
	double timeouts_per_second;
	uint64_t start_us;
	int i, ret;

	log_info("started main_handler() thread");

	waitgroup_init(&wg);
	waitgroup_add(&wg, WORKERS);
	start_us = microtime();
	for (i = 0; i < WORKERS; i++) {
		ret = thread_spawn(work_handler, &wg);
		BUG_ON(ret);
	}

	waitgroup_wait(&wg);
	timeouts_per_second = (double)(WORKERS * N) /
		((microtime() - start_us) * 0.000001);
	log_info("handled %f timeouts / second", timeouts_per_second);
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
		printf("failed to start runtime\n");
		return ret;
	}

	return 0;
}
