/*
 * test_kthread_wakeup.c - tests waking of kthreads
 */

#include <stdio.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <base/atomic.h>
#include <runtime/runtime.h>
#include <runtime/sync.h>

#define NTHREADS	6
#define N		500000
#define SPAWN_LIMIT	5

atomic_t n_threads;
atomic_t n_spawned;
waitgroup_t wg;

static void work_handler(void *arg)
{
	int i, ret, n_to_spawn;
	waitgroup_t *wg_parent = &wg;

	/* do some busy work */
	delay_us(100);

	if (atomic_read(&n_threads) < NTHREADS) {
		/* we have too few threads, spawn more */
		n_to_spawn = rand() % SPAWN_LIMIT;

		if (atomic_dec_and_test(&n_threads) && n_to_spawn == 0)
			n_to_spawn = 1;

		for (i = 0; i < n_to_spawn; i++) {
			if (atomic_add_and_fetch(&n_spawned, 1) <= N) {
				atomic_inc(&n_threads);
				ret = thread_spawn(work_handler, NULL);
				BUG_ON(ret);
			}
		}
	} else {
		/* don't spawn any more */
		atomic_dec(&n_threads);
	}
	waitgroup_done(wg_parent);
}

static void main_handler(void *arg)
{
	int i, ret;

	log_info("started main_handler() thread");

	atomic_write(&n_threads, 0);
	atomic_write(&n_spawned, 0);
	waitgroup_init(&wg);
	waitgroup_add(&wg, N);
	for (i = 0; i < NTHREADS; i++) {
		atomic_inc(&n_spawned);
		atomic_inc(&n_threads);
		ret = thread_spawn(work_handler, NULL);
		BUG_ON(ret);
	}

	waitgroup_wait(&wg);
	log_info("ran %d threads", N);
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
