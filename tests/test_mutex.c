#include <stdio.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <runtime/runtime.h>
#include <runtime/sync.h>
#include <runtime/timer.h>

mutex_t m;
uint64_t start;
static void mutex_one(void *v) {
    mutex_t *t = (mutex_t*) v;
    mutex_lock(t);
    printf("[%lu] locked one\n", microtime() - start);
    timer_sleep(100);
    mutex_unlock(t);
    printf("[%lu] unlocked one\n", microtime() - start);
}

static void main_handler(void *arg)
{
    start = microtime();
    mutex_init(&m);

    thread_spawn(&mutex_one, (void*) &m);
    timer_sleep(10);
    if (mutex_try_lock(&m)) {
        printf("try lock should not work\n");
    } else {
        printf("[%lu] try lock ok\n", microtime() - start);
    }

    mutex_lock(&m);
    printf("[%lu] done\n", microtime() - start);
    mutex_unlock(&m);
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
