#include <time.h>

#include <base/time.h>
#include <runtime/thread.h>
#include <runtime/timer.h>

#include "common.h"

int usleep(useconds_t usec)
{
	NOTSELF(usleep, usec);
	timer_sleep(usec);
	return 0;
}

unsigned int sleep(unsigned int seconds)
{
	NOTSELF(sleep, seconds);
	timer_sleep(seconds * ONE_SECOND);
	return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
	NOTSELF(nanosleep, req, rem);

	timer_sleep(req->tv_sec * ONE_SECOND + req->tv_nsec / 1000);

	if (rem) {
		rem->tv_sec = 0;
		rem->tv_nsec = 0;
	}

	return 0;
}