#pragma once

#include <dlfcn.h>
#include <base/assert.h>
#include <base/init.h>
#include <runtime/sync.h>

static inline bool shim_active(void)
{
	return base_init_done && thread_self() != NULL;
}

static inline void shim_preempt_enable(void)
{
	if (likely(shim_active()))
		preempt_enable();
}

static inline void shim_preempt_disable(void)
{
	if (likely(shim_active()))
		preempt_disable();
}


static inline void shim_spin_unlock_np(spinlock_t *l)
{
	spin_unlock(l);
	shim_preempt_enable();
}

static inline void shim_spin_lock_np(spinlock_t *l)
{
	shim_preempt_disable();
	spin_lock(l);
}

#define NOTSELF(name, ...)                                                     \
        if (unlikely(!shim_active())) {                                        \
                static typeof(name) *fn;                                       \
                if (!fn) {                                                     \
                        fn = dlsym(RTLD_NEXT, #name);                          \
                        BUG_ON(!fn);                                           \
                }                                                              \
                return fn(__VA_ARGS__);                                        \
        }
