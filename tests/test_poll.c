#include <stdio.h>

#include <base/stddef.h>
#include <base/log.h>
#include <base/time.h>
#include <runtime/runtime.h>
#include <runtime/sync.h>
#include <runtime/poll.h>
#include <runtime/timer.h>


static void short_work(void *v) {
    poll_trigger_t *t = (poll_trigger_t*) v;
    timer_sleep(100);
    poll_trigger(t->waiter, t);
}

static void long_work(void *v) {
    poll_trigger_t *t = (poll_trigger_t*) v;
    timer_sleep(1000);
    poll_trigger(t->waiter, t);
}

static void main_handler(void *arg)
{
    poll_waiter_t w;
    poll_init(&w);

    poll_trigger_t short_trigger;
    poll_trigger_init(&short_trigger);
    poll_arm(&w, &short_trigger, 1);

    poll_trigger_t long_trigger;
    poll_trigger_init(&long_trigger);
    poll_arm(&w, &long_trigger, 0);

    thread_spawn(&short_work, (void*) &short_trigger);
    thread_spawn(&long_work, (void*) &long_trigger);

    unsigned long res = poll_wait(&w);
    if (res) {
        printf("ok\n");
    } else {
        printf("error\n");
    }
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
