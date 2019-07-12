#include <stdio.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <runtime/runtime.h>
#include <runtime/sync.h>

#define N		20000
#define ITERS   500000
#define NCORES	4

struct bucket {
	mutex_t lock;
	condvar_t cv;
	int message;
};

mutex_t next_bucket_lock;
int next_bucket = 0;
struct bucket buckets[N];

mutex_t messages_received_lock;
condvar_t messages_received_cv;
int messages_received;

mutex_t start_lock;
condvar_t start_cv;
bool start;

static void work_handler(void *arg)
{
	int bucket;

	mutex_lock(&start_lock);
	while (!start)
		condvar_wait(&start_cv, &start_lock);
	mutex_unlock(&start_lock);

	mutex_lock(&next_bucket_lock);
	bucket = next_bucket++;
	mutex_unlock(&next_bucket_lock);

	mutex_lock(&buckets[bucket].lock);
	while (1) {
		while (buckets[bucket].message == 0) {
			condvar_wait(&buckets[bucket].cv, &buckets[bucket].lock);
		}

		if (buckets[bucket].message == 2) {
			break;
		}

		mutex_lock(&messages_received_lock);
		messages_received += 1;
		condvar_signal(&messages_received_cv);
		mutex_unlock(&messages_received_lock);
		buckets[bucket].message = 0;
	}
	mutex_unlock(&buckets[bucket].lock);

	waitgroup_t *wg_parent = (waitgroup_t *)arg;
	waitgroup_done(wg_parent);
}

static void main_handler(void *arg)
{
	waitgroup_t wg;
	double messages_per_second;
	uint64_t start_us;
	int i, ret, bucket;

	log_info("started main_handler() thread");

	mutex_init(&next_bucket_lock);
	for (i = 0; i < N; i++) {
		mutex_init(&buckets[i].lock);
		condvar_init(&buckets[i].cv);
		buckets[i].message = 0;
	}

	mutex_init(&messages_received_lock);
	condvar_init(&messages_received_cv);
	messages_received = 0;

	mutex_init(&start_lock);
	condvar_init(&start_cv);
	start = false;

	waitgroup_init(&wg);
	waitgroup_add(&wg, N);
	for (i = 0; i < N; i++) {
		ret = thread_spawn(work_handler, &wg);
		BUG_ON(ret);
		thread_yield();
	}
	log_info("spawned threads");

	mutex_lock(&start_lock);
	start = true;
	condvar_broadcast(&start_cv);
	mutex_unlock(&start_lock);
	thread_yield();

	// Send messages to randomly chosen buckets, and wait for confirmation for
	// each one in sequence.
	start_us = microtime();
	for (i = 0; i < ITERS; i++) {
		bucket = rand() % N;
		mutex_lock(&buckets[bucket].lock);
		assert(buckets[bucket].message == 0);
		buckets[bucket].message = 1;
		condvar_signal(&buckets[bucket].cv);
		mutex_unlock(&buckets[bucket].lock);

		mutex_lock(&messages_received_lock);
		while (messages_received <= i)
			condvar_wait(&messages_received_cv, &messages_received_lock);
		mutex_unlock(&messages_received_lock);
	}
	messages_per_second = (double)ITERS /
		((microtime() - start_us) * 0.000001);

	log_info("Done sending messages");

	// Send close message to all buckets.
	for (i = 0; i < N; i++) {
		mutex_lock(&buckets[i].lock);
		assert(buckets[i].message == 0);
		buckets[i].message = 2;
		condvar_signal(&buckets[i].cv);
		mutex_unlock(&buckets[i].lock);
	}

	waitgroup_wait(&wg);
	log_info("%f messages / second", messages_per_second);
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
