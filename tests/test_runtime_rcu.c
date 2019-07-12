/*
 * test_runtime_rcu.c - tests RCU
 */

#include <stdlib.h>
#include <stdio.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <runtime/runtime.h>
#include <runtime/sync.h>
#include <runtime/rcu.h>

#define N		1000000
#define NTHREADS	100
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

static void read_handler(void *arg)
{
	bool ptr_swapped = false;
	struct test_obj *o;
	int i;

	for (i = 0; i < N; i++) {
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

static void spawn_rcu_readers(waitgroup_t *wg, int readers)
{
	int ret, i;

	log_info("creating %d threads to read an RCU object.", readers);

	waitgroup_add(wg, readers);
	for (i = 0; i < readers; i++) {
		ret = thread_spawn(read_handler, wg);
		BUG_ON(ret);
	}

	thread_yield();
}

static void main_handler(void *arg)
{
	struct test_obj *o, *o2;
	waitgroup_t wg;

	log_info("started main_handler() thread");
	waitgroup_init(&release_wg);
	waitgroup_add(&release_wg, 1);
	waitgroup_init(&wg);

	o = malloc(sizeof(*o));
	BUG_ON(!o);
	o->foo = FIRST_VAL;
	RCU_INIT_POINTER(test_ptr, o);

	/* test rcu_free() */
	log_info("testing rcu_free()...");
	spawn_rcu_readers(&wg, NTHREADS);
	o2 = malloc(sizeof(*o));
	o2->foo = SECOND_VAL;
	rcu_assign_pointer(test_ptr, o2);
	rcu_free(&o->rcu, test_release);
	waitgroup_wait(&wg);
	log_info("readers finished.");
	waitgroup_wait(&release_wg);
	log_info("RCU release finished.");

	free(o2);
	o = malloc(sizeof(*o));
	BUG_ON(!o);
	o->foo = FIRST_VAL;
	RCU_INIT_POINTER(test_ptr, o);

	/* test synchronize_rcu() */
	log_info("testing synchronize_rcu()...");
	spawn_rcu_readers(&wg, NTHREADS);
	o2 = malloc(sizeof(*o));
	o2->foo = SECOND_VAL;
	rcu_assign_pointer(test_ptr, o2);
	synchronize_rcu();
	o->foo = FIRST_VAL;
	free(o);
	waitgroup_wait(&wg);
	log_info("readers finished.");
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
