/*
 * test_multiple_runtimes.c - tests initialization of multiple runtimes
 */

#include <stdio.h>
#include <unistd.h>

#include <base/log.h>
#include <runtime/runtime.h>
#include <runtime/timer.h>

#define N_RUNTIMES	2
#define SLEEP_S		5

static void main_handler(void *arg)
{
	int i;

	for (i = 0; i < SLEEP_S; i++)
		timer_sleep(1000*1000);

	log_info("exiting runtime");
}

int main(int argc, char *argv[])
{
	int i, pid, ret;

	if (argc < 1 + N_RUNTIMES) {
		printf("arg must provide a config file for each runtime\n");
		return -EINVAL;
	}

	for (i = 0; i < N_RUNTIMES; i++) {
		pid = fork();
		BUG_ON(pid == -1);

		if (pid == 0) {
			ret = runtime_init(argv[1 + i], main_handler, NULL);
			BUG_ON(ret < 0);
		}

		sleep(1);
	}

	return 0;
}
