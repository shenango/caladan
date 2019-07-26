/*
 * test_storage_iops.c - tests write IOPS for storage device using shenango runtime
 */

#include <stdio.h>

#include <base/atomic.h>
#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <runtime/runtime.h>
#include <runtime/thread.h>
#include <runtime/sync.h>
#include <runtime/timer.h>
#include <runtime/storage.h>


#define WORKERS		100
#define N		100000

static void work_handler(void *arg)
{
	static atomic_t thread_counter;
	waitgroup_t *wg_parent = (waitgroup_t *)arg;
	int i, tid;
	char *p;


	p = malloc(4096);
	BUG_ON(!p);

	tid = atomic_fetch_and_add(&thread_counter, 1);

	for (i = 0; i < N; i++)
		BUG_ON(storage_write(p, 8 * (tid * N + i), 8));

	waitgroup_done(wg_parent);
}

static void main_handler(void *arg)
{
	waitgroup_t wg;
	double iops;
	uint64_t start_us;
	int i, ret;

	log_info("started main_handler() thread");

	BUG_ON(8 * (N + 1) * WORKERS > storage_num_blocks());

	waitgroup_init(&wg);
	waitgroup_add(&wg, WORKERS);
	start_us = microtime();
	for (i = 0; i < WORKERS; i++) {
		ret = thread_spawn(work_handler, &wg);
		BUG_ON(ret);
	}

	waitgroup_wait(&wg);
	iops = (double)(WORKERS * N) /
		((microtime() - start_us) * 0.000001);
	log_info("handled %f IOPS", iops);
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
