/*
 * test_cq_fuzz.c - fuzz test for the iokernel's completion-queue monitoring
 * logic (iokernel/cq_mon.h)
 *
 * Drives a seeded random interleaving of a simulated device (doneness-bit
 * writes), runtime (submit/reap/publish, using the store orderings of the
 * real paths), and iokernel (polls via the real cq_mon.h code), checking
 * every observation against per-entry ground truth. Fuzzed dimensions:
 * doneness mode, ring/slot geometry, done-byte offset/mask (with garbage
 * in unmasked bits), gated vs exogenous queues, out-of-order completion,
 * 16-bit-truncated tail publishers, and counters that cross the 32-bit
 * wrap.
 *
 * Usage: tests/test_cq_fuzz [seed]   (env: CONFIGS=n STEPS=n)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <base/stddef.h>

#include "../iokernel/cq_mon.h"

#define MAX_DESC	1024
#define MAX_SLOT	64
#define THRESH		10000	/* standing-queue delay threshold (cycles) */

/* ground-truth state of one live ring entry */
enum {
	ST_FREE = 0,	/* not in the [t, s) window */
	ST_SUBMITTED,	/* request issued, device hasn't completed it */
	ST_COMPLETED,	/* device wrote doneness bits, runtime hasn't reaped */
	ST_CLEARED,	/* runtime cleared the bits mid-reap (pre-publish) */
};

/* in-progress multi-step runtime operation */
enum {
	OP_NONE = 0,
	OP_SUBMIT,
	OP_REAP,
};

static struct world {
	/* config */
	uint8_t		mode;		/* CQ_DONE_PARITY or CQ_DONE_NONZERO */
	bool		gated;		/* maintains a cq_outstanding byte */
	bool		ooo;		/* out-of-order completion (NONZERO) */
	bool		pub16;		/* publisher truncates tails to 16 bits */
	uint32_t	nr;		/* ring slots, power of two */
	uint32_t	slot;		/* slot size, power of two */
	uint32_t	done_off;
	uint8_t		done_mask;

	/* simulated shared memory */
	uint8_t		ring[MAX_DESC * MAX_SLOT];
	uint32_t	published_tail;	/* models q_ptrs->cq_tails[i] */
	uint8_t		outstanding;	/* models q_ptrs->cq_outstanding[i] */

	/* code under test */
	struct cq_mon	mon;

	/* ground truth; 64-bit counters whose low bits are the wire values */
	uint64_t	t;		/* reaped */
	uint64_t	s;		/* submitted (arrived, if exogenous) */
	uint64_t	f;		/* in-order completion frontier */
	uint64_t	g_pub;		/* 64-bit value of published_tail */
	uint8_t		st[MAX_DESC];

	int		op;
	int		op_stage;

	/* oracle mirror of the detection state machine */
	uint64_t	model_busy;	/* UINT64_MAX = observed empty */
	uint64_t	last_pub_seen;	/* tail at last non-empty poll (PARITY) */

	uint64_t	now;
} w;

static uint64_t seed, steps_done;

static uint64_t rnd64(void)
{
	/* xorshift64* */
	seed ^= seed >> 12;
	seed ^= seed << 25;
	seed ^= seed >> 27;
	return seed * 0x2545F4914F6CDD1DULL;
}

static uint32_t rnd(uint32_t n)
{
	return rnd64() % n;
}

#define fail(fmt, ...)							\
	do {								\
		fprintf(stderr, "FAIL (step %" PRIu64 "): " fmt "\n",	\
			steps_done, ##__VA_ARGS__);			\
		fprintf(stderr, "config: mode=%s gated=%d ooo=%d "	\
			"pub16=%d nr=%u slot=%u off=%u mask=%02x\n",	\
			w.mode == CQ_DONE_PARITY ? "parity" : "nonzero",\
			w.gated, w.ooo, w.pub16, w.nr, w.slot,		\
			w.done_off, w.done_mask);			\
		fprintf(stderr, "state: t=%" PRIx64 " s=%" PRIx64	\
			" f=%" PRIx64 " g_pub=%" PRIx64 " out=%u "	\
			"now=%" PRIu64 "\n",				\
			w.t, w.s, w.f, w.g_pub, w.outstanding, w.now);	\
		exit(1);						\
	} while (0)

static inline uint8_t *slot_byte(uint64_t g)
{
	return &w.ring[(g & (w.nr - 1)) * w.slot + w.done_off];
}

static inline uint8_t *entry_st(uint64_t g)
{
	return &w.st[g & (w.nr - 1)];
}

static inline bool parity(uint64_t g)
{
	return !!((uint32_t)g & w.nr);
}

/* garbage in the bits the monitor must ignore */
static inline uint8_t garbage(void)
{
	return (uint8_t)rnd64() & ~w.done_mask;
}

/* doneness bits for a completion of global index g */
static inline uint8_t done_bits(uint64_t g)
{
	if (w.mode == CQ_DONE_NONZERO) {
		uint8_t bits = (uint8_t)rnd64() & w.done_mask;
		return bits ?: w.done_mask;
	}
	/* PARITY: done iff !!(bits & mask) == parity(g) */
	return parity(g) ? w.done_mask : 0;
}

static inline uint8_t not_done_bits(uint64_t g)
{
	if (w.mode == CQ_DONE_NONZERO)
		return 0;
	return parity(g) ? 0 : w.done_mask;
}

/* publish the runtime's consumer tail, as the real publishers do (uint16
 * free-running counters for DSA/QAT, uint32 for storage/directpath) */
static void publish_tail(void)
{
	w.published_tail = w.pub16 ? (uint32_t)(w.t & 0xffff) : (uint32_t)w.t;
	w.g_pub = w.t;
}

/* ---------------------------- device model ---------------------------- */

static void device_step(void)
{
	uint64_t g;

	if (!w.gated) {
		/* exogenous arrival: an entry appears and completes at once */
		if (w.s - w.t >= w.nr)
			return;
		g = w.s++;
		*entry_st(g) = ST_COMPLETED;
		*slot_byte(g) = garbage() | done_bits(g);
		w.f = g + 1;
		return;
	}

	if (w.mode == CQ_DONE_PARITY || !w.ooo) {
		/* complete the oldest submitted entry */
		for (g = w.f; g < w.s; g++) {
			if (*entry_st(g) != ST_SUBMITTED)
				continue;
			*entry_st(g) = ST_COMPLETED;
			*slot_byte(g) = garbage() | done_bits(g);
			if (g == w.f)
				while (w.f < w.s &&
				       *entry_st(w.f) == ST_COMPLETED)
					w.f++;
			return;
		}
		return;
	}

	/* out-of-order: complete any submitted entry */
	uint64_t nlive = w.s - w.t;
	if (!nlive)
		return;
	uint64_t start = w.t + rnd(nlive);
	for (uint64_t i = 0; i < nlive; i++) {
		g = w.t + ((start - w.t + i) % nlive);
		if (*entry_st(g) != ST_SUBMITTED)
			continue;
		*entry_st(g) = ST_COMPLETED;
		*slot_byte(g) = garbage() | done_bits(g);
		while (w.f < w.s && *entry_st(w.f) == ST_COMPLETED)
			w.f++;
		return;
	}
}

/* --------------------------- runtime model ---------------------------- */

/*
 * Sub-step orderings mirror the real paths, so polls can land in the
 * transient windows: submit clears the doneness bits and issues to the
 * device (which may complete at any point after) before setting
 * cq_outstanding; reap clears the bits, then advances + publishes the
 * tail, and recomputes cq_outstanding only at batch end.
 */

static void runtime_step(void)
{
	if (w.op == OP_NONE) {
		/* start a new op: prefer reaping half the time */
		bool can_reap = w.s != w.t && *entry_st(w.t) == ST_COMPLETED;
		bool can_submit = w.gated && w.s - w.t < w.nr;

		if (can_reap && (!can_submit || rnd(2))) {
			w.op = OP_REAP;
			w.op_stage = 0;
		} else if (can_submit) {
			w.op = OP_SUBMIT;
			w.op_stage = 0;
		} else {
			return;
		}
	}

	if (w.op == OP_SUBMIT) {
		if (w.op_stage == 0) {
			/* clear bits, issue to device */
			*slot_byte(w.s) = garbage() | not_done_bits(w.s);
			*entry_st(w.s) = ST_SUBMITTED;
			w.s++;
			w.op_stage = 1;
		} else {
			ACCESS_ONCE(w.outstanding) = 1;
			w.op = OP_NONE;
		}
		return;
	}

	/* OP_REAP */
	if (w.op_stage == 0) {
		if (w.mode == CQ_DONE_NONZERO) {
			*slot_byte(w.t) = garbage();
			*entry_st(w.t) = ST_CLEARED;
		}
		w.op_stage = 1;
		return;
	}
	if (w.op_stage == 1) {
		*entry_st(w.t) = ST_FREE;
		w.t++;
		publish_tail();
		/* keep reaping in the same batch half the time */
		if (w.s != w.t && *entry_st(w.t) == ST_COMPLETED && rnd(2)) {
			w.op_stage = 0;
			return;
		}
		w.op_stage = 2;
		return;
	}
	/* batch end */
	if (w.gated)
		ACCESS_ONCE(w.outstanding) = w.t != w.s;
	w.op = OP_NONE;
}

/* --------------------------- iokernel model --------------------------- */

static bool poll_once(void)
{
	bool has_work = false, standing = false;
	uint64_t delay = 0;
	uint32_t prev_last_head, prev_last_tail;
	bool expected;

	/* gating decision, as in sched_measure_cq_delays() */
	if (w.gated && !ACCESS_ONCE(w.outstanding))
		return false;

	prev_last_head = w.mon.last_head;
	prev_last_tail = w.mon.last_tail;

	cq_measure_delay(&w.mon, ACCESS_ONCE(w.published_tail), w.now, THRESH,
			 &has_work, &standing, &delay);

	/*
	 * Oracle. NONZERO: work iff the oldest unreaped entry (at the
	 * published tail) has been completed and not yet cleared. PARITY:
	 * work iff the published tail hasn't reached the in-order
	 * completion frontier.
	 */
	if (w.mode == CQ_DONE_NONZERO)
		expected = w.g_pub != w.s &&
			   *entry_st(w.g_pub) == ST_COMPLETED;
	else
		expected = w.g_pub != w.f;

	if (has_work != expected)
		fail("has_work=%d, expected %d", has_work, expected);

	if (w.mode == CQ_DONE_PARITY) {
		/* the scan must land exactly on the completion frontier */
		if (w.mon.last_head != (uint32_t)w.f)
			fail("find_head=%x, frontier=%x",
			     w.mon.last_head, (uint32_t)w.f);
		if (w.mon.last_tail != (uint32_t)w.g_pub)
			fail("last_tail=%x, published=%x",
			     w.mon.last_tail, (uint32_t)w.g_pub);
	}

	/* mirror the busy_since state machine over ground truth */
	if (!expected) {
		w.model_busy = UINT64_MAX;
		if (delay != 0 || standing)
			fail("empty poll reported delay=%" PRIu64
			     " standing=%d", delay, standing);
	} else {
		bool progress = w.mode == CQ_DONE_PARITY &&
				(uint32_t)w.g_pub != prev_last_tail;
		if (w.model_busy == UINT64_MAX || progress)
			w.model_busy = w.now;
		if (delay != w.now - w.model_busy)
			fail("delay=%" PRIu64 ", expected %" PRIu64,
			     delay, w.now - w.model_busy);
		if (standing != (w.mode == CQ_DONE_PARITY ?
				 wraps_lt((uint32_t)w.g_pub, prev_last_head) :
				 delay >= THRESH))
			fail("standing=%d unexpected (delay=%" PRIu64 ")",
			     standing, delay);
	}

	return has_work;
}

/*
 * Liveness check: drain any in-progress runtime op, then verify that the
 * quiesced state is self-consistent - the outstanding byte matches the
 * in-flight count, and if a completion is waiting, a poll is not gated off
 * and reports it.
 */
static void quiesce_check(void)
{
	while (w.op != OP_NONE)
		runtime_step();

	if (w.gated && !!w.outstanding != (w.t != w.s))
		fail("quiesced outstanding=%u but in-flight=%" PRIu64,
		     w.outstanding, w.s - w.t);

	bool completion_waiting = w.s != w.t &&
				  *entry_st(w.t) == ST_COMPLETED;
	bool has_work = poll_once();

	if (completion_waiting && !has_work)
		fail("quiesced with a waiting completion but no work seen");
}

/* ------------------------------- driver -------------------------------- */

static void run_config(uint64_t nsteps)
{
	uint64_t base;
	uint32_t i;

	memset(&w, 0, sizeof(w));

	w.mode = rnd(2) ? CQ_DONE_PARITY : CQ_DONE_NONZERO;
	if (w.mode == CQ_DONE_NONZERO) {
		/* request/response rings are always outstanding-gated */
		w.gated = true;
		w.ooo = rnd(2);
		w.pub16 = rnd(2);
		w.done_mask = (uint8_t)rnd64() ?: 0xff;
	} else {
		/* parity rings: gated (storage) or exogenous (network) */
		w.gated = rnd(2);
		w.done_mask = 1 << rnd(8);
	}
	w.nr = 4 << rnd(9);			/* 4 .. 1024 */
	w.slot = 16 << rnd(3);			/* 16 .. 64 */
	w.done_off = rnd(w.slot);

	/* start close under a 32-bit wrap so every run crosses it */
	base = 0xFFFFFFFFull + 1 - w.nr * 2 - rnd(w.nr);
	w.t = w.s = w.f = base;
	publish_tail();

	/* simulated shm starts as garbage, with each slot's doneness bits
	 * in the not-done phase for the lap that will write it next */
	for (i = 0; i < w.nr * w.slot; i++)
		w.ring[i] = (uint8_t)rnd64();
	for (i = 0; i < w.nr; i++) {
		uint64_t g = base + ((i - (uint32_t)(base & (w.nr - 1)) +
				      w.nr) & (w.nr - 1));
		w.ring[i * w.slot + w.done_off] = garbage() | not_done_bits(g);
	}

	/* code under test starts as control_init_cq() would leave it for a
	 * queue whose counters are already at `base` */
	w.mon.ring = w.ring;
	w.mon.nr_descriptors = w.nr;
	w.mon.done_byte_offset = w.done_off;
	w.mon.done_bit_mask = w.done_mask;
	w.mon.done_mode = w.mode;
	w.mon.log_slot_size = __builtin_ctz(w.slot);
	w.mon.busy_since = UINT64_MAX;
	w.mon.last_head = w.mon.last_tail = (uint32_t)base;

	w.model_busy = UINT64_MAX;
	w.now = 1;

	for (uint64_t step = 0; step < nsteps; step++) {
		steps_done++;
		switch (rnd(10)) {
		case 0: case 1: case 2:
			device_step();
			break;
		case 3: case 4: case 5: case 6:
			runtime_step();
			break;
		case 7: case 8:
			poll_once();
			break;
		default:
			w.now += 1 + rnd(5000);
			break;
		}
		if (step % 1024 == 1023)
			quiesce_check();
	}
	quiesce_check();
}

/* check cq_due_mask() against the obvious byte-scan loop. Its input is 0 or
 * 1 per byte by contract (q_ptrs::cq_outstanding), so the whole domain is
 * just 256 words and the check can be exhaustive rather than sampled. */
static void due_mask_check(void)
{
	uint64_t n, v;
	uint8_t ref;
	int i;

	for (n = 0; n < 256; n++) {
		v = 0;
		for (i = 0; i < 8; i++)
			if (n & BIT(i))
				v |= 1ull << (i * 8);

		ref = 0;
		for (i = 0; i < 8; i++)
			if (v & (0xffull << (i * 8)))
				ref |= BIT(i);

		if (cq_due_mask(v) != ref)
			fail("cq_due_mask(%" PRIx64 ")=%02x, expected %02x",
			     v, cq_due_mask(v), ref);
	}
}

int main(int argc, char *argv[])
{
	uint64_t nconfigs = 256, nsteps = 20000, initial_seed;
	const char *env;

	initial_seed = argc > 1 ? strtoull(argv[1], NULL, 0) : 0xC0FFEE;
	if ((env = getenv("CONFIGS")))
		nconfigs = strtoull(env, NULL, 0);
	if ((env = getenv("STEPS")))
		nsteps = strtoull(env, NULL, 0);

	seed = initial_seed;
	due_mask_check();

	for (uint64_t c = 0; c < nconfigs; c++) {
		seed = initial_seed + c * 0x9E3779B97F4A7C15ULL;
		run_config(nsteps);
	}

	printf("cq fuzz: ok (%" PRIu64 " configs x %" PRIu64
	       " steps, seed 0x%" PRIx64 ")\n",
	       nconfigs, nsteps, initial_seed);
	return 0;
}
