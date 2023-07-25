/*
 * init.h - support for initialization
 */

#pragma once

#include <base/stddef.h>
#include <base/thread.h>

extern int base_init(void);
extern int base_init_thread(void);
extern void init_shutdown(int status) __noreturn;

extern bool base_init_done;
DECLARE_PERTHREAD(bool, thread_init_done);
