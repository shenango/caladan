/*
 * cq_mon.h - monitoring logic for device completion queues
 *
 * Kept free of iokernel globals and hardware formats so that
 * tests/test_cq_fuzz.c can exercise this logic directly.
 */

#pragma once

#include <base/stddef.h>
#include <iokernel/control.h>

/* a monitored device completion queue, materialized from a cq_spec;
 * doneness is described by data so new queue types need no iokernel code */
struct cq_mon {
	void			*ring;		/* NULL if slot unused */
	uint64_t		busy_since;
	uint32_t		last_tail;
	uint32_t		last_head;
	uint32_t		nr_descriptors;
	uint32_t		done_byte_offset;
	uint8_t			done_bit_mask;
	uint8_t			done_mode;	/* CQ_DONE_* */
	uint8_t			log_slot_size;
	uint8_t			hwq_type;	/* HWQ_* */
};

/* bit i set iff byte i of @outstanding is set. Each byte is 0 or 1 (see
 * q_ptrs::cq_outstanding), so bit 0 of every byte is the flag and one
 * multiply moves bit 8i to bit 56+i. The AND is not made redundant by the
 * assert: it bounds the damage if a byte ever does hold something else in
 * a release build, where that queue reads as not-due instead of carrying
 * into a neighbouring queue's bit. */
static inline uint8_t cq_due_mask(uint64_t outstanding)
{
	assert((outstanding & ~0x0101010101010101UL) == 0);

	return ((outstanding & 0x0101010101010101UL) *
		0x0102040810204080UL) >> 56;
}

static inline unsigned char *cq_slot_done_addr(struct cq_mon *m, uint32_t cq_idx)
{
	uint32_t idx = cq_idx & (m->nr_descriptors - 1);
	return (unsigned char *)m->ring + ((size_t)idx << m->log_slot_size) +
	       m->done_byte_offset;
}

/* is the completion-ring slot at cq_idx written by the device but not yet
 * consumed by the runtime? */
static inline bool cq_slot_done(struct cq_mon *m, uint32_t cq_idx)
{
	uint8_t bits = ACCESS_ONCE(*cq_slot_done_addr(m, cq_idx)) &
		       m->done_bit_mask;

	if (m->done_mode == CQ_DONE_NONZERO)
		return bits != 0;

	/* CQ_DONE_PARITY: the done bit's phase flips on each ring wrap */
	return !!bits == !!(cq_idx & m->nr_descriptors);
}

static inline uint32_t cq_find_head(struct cq_mon *m, uint32_t cur_tail,
				    uint32_t last_head)
{
	uint32_t i = 0;
	uint32_t start_idx = wraps_lt(cur_tail, last_head) ? last_head : cur_tail;
	uint32_t nr_desc = m->nr_descriptors - (start_idx - cur_tail);

	while (i < nr_desc) {
		if (!cq_slot_done(m, start_idx + i))
			break;
		i++;
	}

	return i + start_idx;
}

/*
 * Request/response (CQ_DONE_NONZERO) completion tracking: rather than
 * scanning the completion ring to find exactly how many entries the device
 * has finished (expensive, and only ever used to answer a yes/no question),
 * just peek at the single oldest outstanding slot (the one the runtime's
 * own tail points at). If it's done, something is queued waiting to be
 * drained.
 *
 * busy_since is reset only on the empty -> non-empty transition, not on
 * every tail movement: the runtime's softirq typically drains its whole
 * backlog in one budgeted pass, so tail can advance straight through
 * several still-valid slots between two iokernel polls (e.g. 0 -> 1, with
 * slot 1 already complete too) without ever being observed empty.
 * Resetting on tail movement alone would restart the clock right there and
 * understate how long the accelerator has actually been backed up -
 * confirmed empirically against real QAT hardware: a deliberately-induced
 * ~50us backlog was reported as ~20us of delay when resetting on tail
 * movement, because the poll that caught the drain landed mid-burst.
 * Resetting only on the transition to empty fixes this, and still
 * correctly reports a growing delay under sustained backlog, which is the
 * signal that actually matters for scheduling.
 */
static inline void
cq_measure_delay(struct cq_mon *m, uint32_t cur_tail, uint64_t now,
		 uint64_t standing_thresh, bool *has_work,
		 bool *standing_queue, uint64_t *delay_cycles)
{
	uint32_t cur_head, last_head, last_tail;
	uint64_t delay;

	/* request/response ring: peek the oldest outstanding slot only */
	if (m->done_mode == CQ_DONE_NONZERO) {
		if (!cq_slot_done(m, cur_tail)) {
			m->busy_since = UINT64_MAX;
			return;
		}

		if (m->busy_since == UINT64_MAX)
			m->busy_since = now;

		delay = now - m->busy_since;
		*has_work = true;
		*standing_queue |= delay >= standing_thresh;
		*delay_cycles = delay;
		return;
	}

	/*
	 * slow path - will use cq_find_head() to scan the queue
	 * to find the newest element
	 */
	last_head = m->last_head;
	last_tail = m->last_tail;

	cur_head = cq_find_head(m, cur_tail, last_head);
	m->last_tail = cur_tail;
	m->last_head = cur_head;

	/* check whether the queue is empty */
	if (cur_head == cur_tail) {
		m->busy_since = UINT64_MAX;
		return;
	}

	/* check whether there was any progress on draining the queue or a
	 * new element has arrived */
	if (cur_tail != last_tail || m->busy_since == UINT64_MAX)
		m->busy_since = now;

	*has_work = true;
	*standing_queue |= wraps_lt(cur_tail, last_head);
	*delay_cycles = now - m->busy_since;
}
