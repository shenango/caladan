
#include <base/bitmap.h>
#include <base/lock.h>
#include <base/log.h>
#include <runtime/sync.h>

#include <pthread.h>
#include <stdlib.h>

#include "common.h"

#define MAX_KEYS 1024

typedef void (*destfn)(void*);

static uint64_t key_gens[MAX_KEYS];
static DEFINE_BITMAP(allocated_keys, MAX_KEYS);
static size_t nr_alloc;
static DEFINE_SPINLOCK(key_lock);
static destfn destructors[MAX_KEYS];

struct key_data {
	void *data;
	uint64_t gen;
};

static __thread struct key_data *kd_noruntime;

int pthread_key_create(pthread_key_t* key_out, void (*destructor)(void*))
{
	unsigned int key;

	shim_spin_lock_np(&key_lock);
	if (unlikely(nr_alloc >= MAX_KEYS)) {
		shim_spin_unlock_np(&key_lock);
		return -ENOMEM;
	}

	key = bitmap_find_next_cleared(allocated_keys, MAX_KEYS, 0);
	BUG_ON(key == MAX_KEYS);
	bitmap_set(allocated_keys, key);
	store_release(&key_gens[key], key_gens[key] + 1);
	nr_alloc++;
	destructors[key] = destructor;
	shim_spin_unlock_np(&key_lock);

	*key_out = key;
	return 0;
}

static struct key_data *get_ts_struct(int key)
{
	struct key_data *arr;
	uint64_t keygen;

	if (likely(shim_active()))
		arr = (struct key_data *)get_uthread_specific();
	else
		arr = kd_noruntime;

	if (unlikely(!arr)) {
		arr = calloc(MAX_KEYS, sizeof(struct key_data));
		BUG_ON(!arr);
		if (shim_active())
			set_uthread_specific((uint64_t)arr);
		else
			kd_noruntime = arr;
	}

	keygen = load_acquire(&key_gens[key]);
	if (unlikely(arr[key].gen != keygen)) {
		arr[key].data = NULL;
		arr[key].gen = keygen;
	}

	return &arr[key];
}

void* pthread_getspecific(pthread_key_t key)
{
	struct key_data *kd;

	if (unlikely(key >= MAX_KEYS))
		return NULL;

	kd = get_ts_struct(key);
	return kd->data;
}

int pthread_key_delete(pthread_key_t key)
{
	if (unlikely(key >= MAX_KEYS))
		return -EINVAL;

	shim_spin_lock_np(&key_lock);
	assert(bitmap_test(allocated_keys, key));
	bitmap_clear(allocated_keys, key);
	nr_alloc--;
	store_release(&key_gens[key], key_gens[key] + 1);
	if (destructors[key])
		log_warn_ratelimited("unimplemented: pthread_key_delete with destructor");
	shim_spin_unlock_np(&key_lock);
	return 0;
}

int pthread_setspecific(pthread_key_t key, const void* value)
{
	struct key_data *kd;

	if (unlikely(key >= MAX_KEYS))
		return -EINVAL;

	kd = get_ts_struct(key);
	kd->data = (void *)value;
	return 0;
}
