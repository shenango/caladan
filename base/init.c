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

struct init_level {
	const char *name;
	struct init_handler *start, *end;
};

#define INIT_LEVEL(name, level)						\
	{__cstr(name), &__initcall ## level ## _start,			\
	 &__initcall ## level ## _end}

#define DECLARE_INIT_LEVEL_SYMS(level)					\
	extern struct init_handler __initcall ## level ## _start;	\
	extern struct init_handler __initcall ## level ## _end

DECLARE_INIT_LEVEL_SYMS(0);
DECLARE_INIT_LEVEL_SYMS(1);
DECLARE_INIT_LEVEL_SYMS(2);
DECLARE_INIT_LEVEL_SYMS(t);

static int init_one_level(const struct init_level *level)
{
	const struct init_handler *pos;
	int ret;

	log_debug("init: entering '%s' init", level->name);
	for (pos = level->start; pos < level->end; pos++) {
		log_debug("init: -> %s", pos->name);
		ret = pos->init();
		if (ret) {
			log_debug("init: failed, ret = %d", ret);
			return ret;
		}
	}

	return 0;
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

static const struct init_level init_base_levels[] = {
	INIT_LEVEL(early, 0),
	INIT_LEVEL(normal, 1),
	INIT_LEVEL(late, 2),
};

/**
 * base_init - initializes the base library
 *
 * Call this function before using the library.
 * Returns 0 if successful, otherwise fail.
 */
int base_init(void)
{
	int ret, i;

	ret = init_internal();
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(init_base_levels); i++) {
		ret = init_one_level(&init_base_levels[i]);
		if (ret)
			return ret;
	}

	base_init_done = true;
	return 0;
}

extern int thread_init_perthread(void);
static const struct init_level init_thread_level = INIT_LEVEL(thread, t);

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

	ret = init_one_level(&init_thread_level);
	if (ret)
		return ret;

	thread_init_done = true;
	return 0;
}
