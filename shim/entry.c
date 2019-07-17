
#include <stdio.h>

#include <runtime/runtime.h>

int __real_main(int, char **);

static int main_argc, main_ret;
static char **main_argv;

static void runtime_entry(void *arg)
{
	main_ret = __real_main(main_argc, main_argv);
}

int __wrap_main(int argc, char **argv)
{
	int ret;

	if (argc < 2) {
		fprintf(stderr, "Error: missing shenango config argument\n");
		return 0;
	}

	char *cfg = argv[1];
	argv[1] = argv[0];
	main_argv = &argv[1];
	main_argc = argc - 1;

	ret = runtime_init(cfg, runtime_entry, NULL);
	if (ret) {
		fprintf(stderr, "failed to start runtime\n");
		return ret;
	}

	return main_ret;
}
