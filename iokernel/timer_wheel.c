/*
 * timer_wheel.c - timer wheel to manage procs that need to be polled by the
 * scheduler
 *
 * Code/design borrowed from IX.
 *
 * The design is inspired by "Hashed and Hierarchical Timing Wheels: Data
 * Structures for the Efficient Implementation of a Timer Facility" by
 * George Varghese and Tony Lauck. SOSP 87.
 *
 * Specificially, we use Scheme 7 described in the paper, where
 * hierarchical sets of buckets are used.
 */

#include <base/log.h>
#include <base/list.h>
#include <base/time.h>

#include "defs.h"

/*
 * Right now we have the following wheels:
 *
 * high precision wheel: 256 x 16 us increments
 * medium precision wheel: 256 x 4 ms increments
 * low precision wheel: 256 x 1 second increments
 *
 * Total range 0 to 256 seconds...
 */

#define WHEEL_SHIFT_LOG2	3UL
#define WHEEL_SHIFT		(1UL << WHEEL_SHIFT_LOG2)
#define WHEEL_SIZE		(1UL << WHEEL_SHIFT)
#define WHEEL_MASK		(WHEEL_SIZE - 1)
#define WHEEL_COUNT		3UL

#define MIN_DELAY_SHIFT		4UL
#define MIN_DELAY_US		(1UL << MIN_DELAY_SHIFT)
#define MIN_DELAY_MASK		(MIN_DELAY_US - 1)
#define MAX_DELAY_US \
	(MIN_DELAY_US * (1UL << (WHEEL_COUNT * WHEEL_SHIFT)))

#define WHEEL_IDX_TO_SHIFT(idx) \
	((idx) * WHEEL_SHIFT + MIN_DELAY_SHIFT)
#define WHEEL_OFFSET(val, idx) \
	(((val) >> WHEEL_IDX_TO_SHIFT(idx)) & WHEEL_MASK)

/* current timer position (in microtime) */
uint64_t timer_pos;
/* timer wheels */
static struct list_head wheels[WHEEL_COUNT][WHEEL_SIZE];

static void proc_timer_insert(struct proc *p)
{
	uint64_t expire_us, delay_us;
	uint64_t index, offset;

	/*
	 * Round up to the next bucket because part of the time
	 * between buckets has likely already passed.
	 */
	expire_us = p->timer_pos_us + MIN_DELAY_US;
	assert(expire_us >= timer_pos);
	delay_us = MIN(MAX_DELAY_US - 1, expire_us - timer_pos);

	/*
	 * This code looks a little strange because it was optimized to
	 * calculate the correct bucket placement without using any
	 * branch instructions, instead using count last zero (CLZ).
	 *
	 * NOTE: This assumes MIN_DELAY_US was added above. Otherwise
	 * the index might be calculated as negative, so be careful
	 * about this constraint when modifying this code.
	 */
	index = ((63 - __builtin_clzll(delay_us) - MIN_DELAY_SHIFT)
		 >> WHEEL_SHIFT_LOG2);
	offset = WHEEL_OFFSET(expire_us, index);
	assert(index < WHEEL_COUNT);
	assert(offset < WHEEL_SIZE);

	list_add(&wheels[index][offset], &p->link);
}

static void proc_timer_run_bucket(struct list_head *h)
{
	list_append_list(&poll_list, h);
}

static void proc_timer_reinsert_bucket(struct list_head *h)
{
	struct proc *p, *tmp;

	list_for_each_safe(h, p, tmp, link) {

		prefetchnta(tmp);

		list_del_from(h, &p->link);

		if (p->timer_pos_us <= timer_pos) {
			proc_enable_sched_poll_nocheck(p);
			continue;
		}

		proc_timer_insert(p);
	}
}

static void proc_timer_collapse(uint64_t pos)
{
	int wheel;

	for (wheel = 1; wheel < WHEEL_COUNT; wheel++) {
		int off = WHEEL_OFFSET(pos, wheel);

		proc_timer_reinsert_bucket(&wheels[wheel][off]);

		// only need to go to the next wheel if offset is zero
		if (off)
			break;
	}
}

void proc_timer_add(struct proc *p, uint64_t next_poll_tsc)
{
	p->timer_pos_us = (next_poll_tsc - start_tsc) / cycles_per_us;

	if (unlikely(p->timer_pos_us <= timer_pos)) {
		proc_enable_sched_poll_nocheck(p);
		return;
	}

	proc_timer_insert(p);
}

void proc_timer_run(uint64_t now)
{
	for (; timer_pos <= now; timer_pos += MIN_DELAY_US) {
		int high_off = WHEEL_OFFSET(timer_pos, 0);

		if (!high_off)
			proc_timer_collapse(timer_pos);

		prefetch(&wheels[0][WHEEL_OFFSET(timer_pos + MIN_DELAY_US, 0)]);

		proc_timer_run_bucket(&wheels[0][high_off]);
	}
}

int proc_timer_init(void)
{
	int i, j;

	for (i = 0; i < WHEEL_COUNT; i++)
		for (j = 0; j < WHEEL_SIZE; j++)
			list_head_init(&wheels[i][j]);

	timer_pos = microtime();

	return 0;
}
