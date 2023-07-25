/*
 * test_base_thread.c - this base support for threads
 */

#include <pthread.h>

#include <base/init.h>
#include <base/log.h>
#include <base/assert.h>
#include <base/cpu.h>
#include <base/thread.h>

#define PERTHREAD_VAL	10
static DEFINE_PERTHREAD(int, blah);

static int init_thread(void)
{
	int ret;

	ret = base_init_thread();
	if (ret) {
		log_err("base_init_thread() failed, ret = %d", ret);
		return 1;
	}
	BUG_ON(!perthread_read(thread_init_done));
	BUG_ON(perthread_get(blah) != 0);

	perthread_get(blah) = PERTHREAD_VAL;
	BUG_ON(perthread_get(blah) != PERTHREAD_VAL);

	return ret;
}

static void *test_thread(void *data)
{
	int ret;

	ret = init_thread();
	BUG_ON(ret);
	log_info("hello thread %d", this_thread_id());

	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t tid[NCPU];
	int ret, i;

	ret = base_init();
	if (ret) {
		log_err("base_init() failed, ret = %d", ret);
		return 1;
	}
	BUG_ON(!base_init_done);
	BUG_ON(cpu_count < 1);

	init_thread();

	for (i = 1; i < cpu_count; i++) {
		ret = pthread_create(&tid[i], NULL, test_thread, NULL);
		BUG_ON(ret);
	}

	for (i = 1; i < cpu_count; i++) {
		ret = pthread_join(tid[i], NULL);
		BUG_ON(ret);
	}

	log_info("joined all threads");
	return 0;
}
