#pragma once

#include <runtime/thread.h>

struct kthread;

#ifdef GC

/* External API */
typedef void (*stack_bounds_cb)(uint64_t bottom, uint64_t top);
extern void gc_stop_world(void);
extern void gc_start_world(void);

/* reports each active stack to discover_cb */
extern void gc_discover_all_stacks(stack_bounds_cb discover_cb);

/* Internal API */
extern volatile bool world_stopped;
extern volatile uint64_t gc_gen;

static inline bool is_world_stopped(void)
{
	return ACCESS_ONCE(world_stopped);
}

static inline uint64_t get_gc_gen(void)
{
	return ACCESS_ONCE(gc_gen);
}

extern int gc_register_thread(thread_t *th);
extern int gc_remove_thread(thread_t *th);
extern void gc_kthread_report(struct kthread *k);

#else
static inline int gc_register_thread(thread_t *th)
{
	return 0;
}
static inline int gc_remove_thread(thread_t *th)
{
	return 0;
}
static inline void gc_kthread_report(struct kthread *k) {}
static inline bool is_world_stopped(void)
{
	return false;
}

#endif
