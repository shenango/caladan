/*
 * test_base_smalloc.c - tests the base library allocator
 */

#include <stdlib.h>
#include <stdio.h>

#include <base/log.h>
#include <base/assert.h>
#include <base/slab.h>
#include <base/tcache.h>
#include <runtime/runtime.h>
#include <runtime/smalloc.h>
#include <asm/ops.h>

#define SAMPLES	200000
#define N	(1 << 10)
#define SIZE	256
static void *ptrs[N];

static void malloc_bench(int samples, int n)
{
	int i, j;

	for (j = 0; j < samples; j++) {
		for (i = 0; i < n; i++) {
			ptrs[i] = malloc(SIZE);
			BUG_ON(!ptrs[i]);
		}

		for (i = 0; i < n; i++) {
			free(ptrs[i]);
		}
	}
}

static void smalloc_bench(int samples, int n)
{
	int i, j;

	for (j = 0; j < samples; j++) {
		for (i = 0; i < n; i++) {
			ptrs[i] = smalloc(SIZE);
			BUG_ON(!ptrs[i]);
		}

		for (i = 0; i < n; i++) {
			sfree(ptrs[i]);
		}
	}
}


static void main_handler(void *arg)
{
	int i;
	uint64_t tsc, tsc_elapsed;

	log_info("testing BASE smalloc performance");

	for (i = 1; i <= N; i *= 2) {
		cpu_serialize();
		tsc = rdtsc();

		smalloc_bench(SAMPLES, i);

		tsc_elapsed = rdtscp(NULL) - tsc;
		log_info("smalloc %d took: %ld cycles / allocation",
			 i, tsc_elapsed / (SAMPLES * i));
	}

	log_info("testing BASE smalloc performance (warmed up)");

	for (i = 1; i <= N; i *= 2) {
		cpu_serialize();
		tsc = rdtsc();

		smalloc_bench(SAMPLES, i);

		tsc_elapsed = rdtscp(NULL) - tsc;
		log_info("smalloc %d took: %ld cycles / allocation",
			 i, tsc_elapsed / (SAMPLES * i));
	}


	log_info("testing GLIBC malloc performance");

	for (i = 1; i <= N; i *= 2) {
		cpu_serialize();
		tsc = rdtsc();

		malloc_bench(SAMPLES, i);

		tsc_elapsed = rdtscp(NULL) - tsc;
		log_info("malloc %d took: %ld cycles / allocation",
			 i, tsc_elapsed / (SAMPLES * i));
	}

	log_info("testing GLIBC malloc performance (warmed up)");

	for (i = 1; i <= N; i *= 2) {
		cpu_serialize();
		tsc = rdtsc();

		malloc_bench(SAMPLES, i);

		tsc_elapsed = rdtscp(NULL) - tsc;
		log_info("malloc %d took: %ld cycles / allocation",
			 i, tsc_elapsed / (SAMPLES * i));
	}


#ifdef DEBUG
	slab_print_usage();
	tcache_print_usage();
#endif /* DEBUG */
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
