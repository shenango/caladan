/*
 * runtime.h - runtime initialization and metrics
 */

#pragma once

#include <base/stddef.h>
#include <base/time.h>
#include <runtime/thread.h>


/* main initialization */
typedef int (*initializer_fn_t)(void);

extern int runtime_set_initializers(initializer_fn_t global_fn,
				    initializer_fn_t perthread_fn,
				    initializer_fn_t late_fn);
extern int runtime_init(const char *cfgpath, thread_fn_t main_fn, void *arg);


extern struct congestion_info *runtime_congestion;

extern unsigned int maxks;
extern unsigned int guaranteedks;
extern atomic_t runningks;

/**
 * runtime_standing_queue_us - returns the us a queue has been left standing
 */
static inline uint64_t runtime_standing_queue_us(void)
{
	return ACCESS_ONCE(runtime_congestion->standing_queue_us);
}

/**
 * runtime_queue_us - returns the us of packet queueing delay + runtime queueing
 * delay
 */
static inline uint64_t runtime_queue_us(void)
{
	uint64_t now = rdtsc();
	uint64_t rq_oldest_tsc = ACCESS_ONCE(runtime_congestion->rq_oldest_tsc);
	uint64_t pkq_oldest_tsc = ACCESS_ONCE(runtime_congestion->pkq_oldest_tsc);
	uint64_t rq_delay = 0;
	uint64_t pkq_delay = 0;

	if ((unsigned int)atomic_read(&runningks) < maxks)
		return 0;

	if (now > rq_oldest_tsc)
		rq_delay = now - rq_oldest_tsc;

	if (now > pkq_oldest_tsc)
		pkq_delay = now - pkq_oldest_tsc;

	return (rq_delay + pkq_delay) / cycles_per_us;
}

/**
 * runtime_load - returns the current CPU usage load
 */
static inline float runtime_load(void)
{
	return ACCESS_ONCE(runtime_congestion->load);
}

/**
 * runtime_active_cores - returns the number of currently active cores
 *
 */
static inline int runtime_active_cores(void)
{
	return atomic_read(&runningks);
}

/**
 * runtime_max_cores - returns the maximum number of cores
 *
 * The runtime could be given at most this number of cores by the IOKernel.
 */
static inline int runtime_max_cores(void)
{
	return maxks;
}

/**
 * runtime_guaranteed_cores - returns the guaranteed number of cores
 *
 * The runtime will get at least this number of cores by the IOKernel if it
 * requires them.
 */
static inline int runtime_guaranteed_cores(void)
{
	return guaranteedks;
}
