/*
 * test_kthread_attach.c - tests kthread attach and detach
 */

#include <stdlib.h>
#include <stdio.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <runtime/rcu.h>
#include <runtime/sync.h>
#include <runtime/runtime.h>
#include <runtime/timer.h>

#define N		1000
#define NTHREADS	32
#define NROUNDS		100
#define FIRST_VAL	0x1000000
#define SECOND_VAL	0x2000000

static waitgroup_t release_wg;

struct test_obj {
	int		foo;
	struct rcu_head	rcu;
};

static __rcu struct test_obj *test_ptr;

static void test_release(struct rcu_head *head)
{
	struct test_obj *o = container_of(head, struct test_obj, rcu);
	free(o);
	waitgroup_done(&release_wg);
}

static void rcu_read_handler(void *arg)
{
	bool ptr_swapped;
	struct test_obj *o;
	int i;

	for (i = 0; i < N; i++) {
		ptr_swapped = false;
		rcu_read_lock();
		o = rcu_dereference(test_ptr);
		if (o->foo == SECOND_VAL)
			ptr_swapped = true;
		BUG_ON(o->foo != (ptr_swapped ? SECOND_VAL : FIRST_VAL));
		rcu_read_unlock();
		thread_yield();
	}
	waitgroup_t *wg_parent = (waitgroup_t *)arg;
	waitgroup_done(wg_parent);
}

static void timer_handler(void *arg)
{
	waitgroup_t *wg_parent = (waitgroup_t *)arg;
	int i;

	for (i = 0; i < N; i++) {
		/* make each sleep long enough for a kthread to accumulate multiple
		 * timers */
		timer_sleep(10);
	}

	waitgroup_done(wg_parent);
}

static void main_handler(void *arg)
{
	struct test_obj *o, *o2;
	waitgroup_t wg;
	int i, j, ret;

	log_info("started main_handler() thread");
	waitgroup_init(&release_wg);
	o = malloc(sizeof(*o));
	BUG_ON(!o);
	o->foo = FIRST_VAL;
	RCU_INIT_POINTER(test_ptr, o);

	/* test RCU reclamation as kthreads attach and detach */
	/* perform several rounds in which we spawn reader threads, write many
	 * times to the test object, and then wait for all threads to finish. */
	for (i = 0; i < NROUNDS; i++) {
		waitgroup_init(&wg);
		waitgroup_add(&wg, NTHREADS);
		for (j = 0; j < NTHREADS; j++) {
			ret = thread_spawn(rcu_read_handler, &wg);
			BUG_ON(ret);
		}

		thread_yield();

		for (j = 0; j < N; j++) {
			waitgroup_init(&release_wg);
			waitgroup_add(&release_wg, 1);
			o2 = malloc(sizeof(*o));
			o2->foo = SECOND_VAL;
			rcu_assign_pointer(test_ptr, o2);
			rcu_free(&o->rcu, test_release);

			waitgroup_wait(&release_wg);

			waitgroup_init(&release_wg);
			waitgroup_add(&release_wg, 1);

			thread_yield();
			o = malloc(sizeof(*o));
			o->foo = FIRST_VAL;
			rcu_assign_pointer(test_ptr, o);
			rcu_free(&o2->rcu, test_release);

			waitgroup_wait(&release_wg);
		}

		waitgroup_wait(&wg);
	}
	log_info("finished %d rounds of spawning threads to test RCU", NROUNDS);

	/* test timer merging as kthreads attach and detach */
	for (i = 0; i < NROUNDS; i++) {
		waitgroup_init(&wg);
		waitgroup_add(&wg, NTHREADS);
		for (j = 0; j < NTHREADS; j++) {
			ret = thread_spawn(timer_handler, &wg);
			BUG_ON(ret);
		}
		waitgroup_wait(&wg);
	}
	log_info("finished %d rounds of spawning threads to test timer merging",
			NROUNDS);
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
