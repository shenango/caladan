
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

int pthread_key_create(pthread_key_t* key_out, void (*destructor)(void*))
{
	NOTSELF_2ARG(int, __func__, key_out, destructor);

	spin_lock_np(&key_lock);
	if (nr_alloc >= MAX_KEYS) {
		spin_unlock_np(&key_lock);
		return -ENOMEM;
	}

	unsigned int key = bitmap_find_next_cleared(allocated_keys, MAX_KEYS, 0);
	BUG_ON(key == MAX_KEYS);
	bitmap_set(allocated_keys, key);
	store_release(&key_gens[key], key_gens[key] + 1);
	nr_alloc++;
	destructors[key] = destructor;
	spin_unlock_np(&key_lock);

	*key_out = key;
	return 0;
}

static struct key_data *get_ts_struct(int key)
{
	struct key_data *arr = (struct key_data *)get_uthread_specific();

	if (!arr) {
		arr = calloc(MAX_KEYS, sizeof(struct key_data));
		BUG_ON(!arr);
		set_uthread_specific((uint64_t)arr);
	}

	uint64_t keygen = load_acquire(&key_gens[key]);

	if (arr[key].gen != keygen) {
		arr[key].data = NULL;
		arr[key].gen = keygen;
	}

	return &arr[key];
}

void* pthread_getspecific(pthread_key_t key)
{
	NOTSELF_1ARG(void*, __func__, key);

	if (key >= MAX_KEYS)
		return NULL;

	struct key_data *kd = get_ts_struct(key);
	return kd->data;
}

int pthread_key_delete(pthread_key_t key)
{
	NOTSELF_1ARG(int, __func__, key);

	if (key >= MAX_KEYS)
		return -EINVAL;

	spin_lock_np(&key_lock);
	assert(bitmap_test(allocated_keys, key));
	bitmap_clear(allocated_keys, key);
	nr_alloc--;
	store_release(&key_gens[key], key_gens[key] + 1);
	if (destructors[key])
		log_warn_ratelimited("unimplemented: pthread_key_delete with destructor");
	spin_unlock_np(&key_lock);
	return 0;
}

int pthread_setspecific(pthread_key_t key, const void* value)
{
	NOTSELF_2ARG(int, __func__, key, value);

	if (key >= MAX_KEYS)
		return -EINVAL;

	struct key_data *kd = get_ts_struct(key);
	kd->data = (void *)value;
	return 0;
}
