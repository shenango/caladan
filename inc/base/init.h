/*
 * init.h - support for initialization
 */

#pragma once

#include <base/stddef.h>
#include <base/thread.h>

struct init_handler {
	const char *name;
	int (*init)(void);
};

#define __REGISTER_INIT_HANDLER(func, level)				\
	static struct init_handler __init_call##level##func __used	\
	__attribute__((section(".initcall" #level))) =			\
	{__str(func), func}

/* normal initialization */
#define REGISTER_EARLY_INIT(func)	__REGISTER_INIT_HANDLER(func, 0)
#define REGISTER_NORMAL_INIT(func)	__REGISTER_INIT_HANDLER(func, 1)
#define REGISTER_LATE_INIT(func)	__REGISTER_INIT_HANDLER(func, 2)

/* per-thread initialization */
#define REGISTER_THREAD_INIT(func)	__REGISTER_INIT_HANDLER(func, t)

extern int base_init(void);
extern int base_init_thread(void);
extern void init_shutdown(int status) __noreturn;

extern bool base_init_done;
DECLARE_PERTHREAD(bool, thread_init_done);
