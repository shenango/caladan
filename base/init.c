/*
 * init.c - support for initialization
 */

#include <stdlib.h>

#include <base/init.h>
#include <base/log.h>
#include <base/thread.h>

#include "init_internal.h"

bool base_init_done __aligned(CACHE_LINE_SIZE);

void __weak init_shutdown(int status)
{
	log_info("init: shutting down -> %s",
		 status == EXIT_SUCCESS ? "SUCCESS" : "FAILURE");
	exit(status);
}

/* we initialize these early subsystems by hand */
static int init_internal(void)
{
	int ret;

	ret = cpu_init();
	if (ret)
		return ret;

	ret = time_init();
	if (ret)
		return ret;

	ret = page_init();
	if (ret) {
		log_err("Could not intialize memory. Please ensure that hugepages are "
			    "enabled/available.");
		return ret;
	}

	return slab_init();
}


extern int thread_init_perthread(void);

/**
 * base_init - initializes the base library
 *
 * Call this function before using the library.
 * Returns 0 if successful, otherwise fail.
 */
int base_init(void)
{
	int ret;

	ret = thread_init_perthread();
	if (ret)
		return ret;

	ret = init_internal();
	if (ret)
		return ret;

	base_init_done = true;
	return 0;
}

static int init_thread_internal(void)
{
	return page_init_thread();
}

/**
 * base_init_thread - prepares a thread for use by the base library
 *
 * Returns 0 if successful, otherwise fail.
 */
int base_init_thread(void)
{
	int ret;

	ret = thread_init_perthread();
	if (ret)
		return ret;

	ret = init_thread_internal();
	if (ret)
		return ret;

	perthread_store(thread_init_done, true);
	return 0;
}

