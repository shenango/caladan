/*
 * ias.h - the shared header for the IAS scheduler
 */

#pragma once

/* constant tunables */
#define IAS_NPROC		         32  /* the maximum number of processes */
#define IAS_BW_UPPER_LIMIT	         0.09 /* the upper limit on mem bandwidth */
#define IAS_BW_LOWER_LIMIT	         0.08 /* the lower limit on mem bandwidth */
#define IAS_BW_POLL_US		         5   /* time to poll memory bandwidth usage */
#define IAS_HT_POLL_US		         5   /* time to poll for HT contention */
#define IAS_PRIORITY_WEIGHT	         10000.0 /* how heavily to weigh LC priority */
#define IAS_HT_WEIGHT		         100.0 /* how heavily to weigh the HT score */
#define IAS_LOC_EVICTED_US	         100 /* us before all cache is evicted */
#define IAS_EWMA_FACTOR		         0.001 /* the moving average update rate */
#define IAS_DEBUG_PRINT_US	         100000 /* time to print out debug info */
#define IAS_KICK_OUT_FACTOR              0.2 /* used to calc the number of victims */
#define IAS_HT_LC_MAX_IPC_DEGRADE_RATIO  0.3 /* used to judge the bad pairing */
#define IAS_HT_BE_MAX_IPC_DEGRADE_RATIO  0.5 /* used to judge the bad pairing */
#define IAS_BAN_DURATION_US              500 /* the duration of banning a bad pairing */
#define IAS_HT_RANDOM_PAIRING_CNT        1000 /* used to form a pairing randomly */

#define GET_MAX_IPC_DEGRADE_RATIO(sd)                  \
	(is_lc(sd) ? IAS_HT_LC_MAX_IPC_DEGRADE_RATIO : \
         IAS_HT_BE_MAX_IPC_DEGRADE_RATIO)

struct ias_data {
	struct proc		*p;
	unsigned int		is_congested:1;
	unsigned int		is_bwlimited:1;
	unsigned int		idx; /* a unique index */
	struct list_node	all_link;
	DEFINE_BITMAP(claimed_cores, NCPU);

	/* thread usage limits */
	int			threads_guaranteed;/* the number promised */
	int			threads_max;	/* the most possible */
	int			threads_limit;	/* the most allowed */
	int			threads_active;	/* the number active */

	/* locality subcontroller */
	uint64_t		loc_last_us[NCPU];

	/* hyperthread subcontroller */
	uint64_t		ht_last_gen[NCPU];
	uint64_t		ht_last_tsc[NCPU];
	uint64_t		ht_last_instr[NCPU];
	uint64_t                ht_start_running_tsc[NCPU];
	uint64_t                ht_last_banned_tsc[IAS_NPROC];
	float			ht_pairing_ipc[IAS_NPROC];
	float			ht_unpaired_ipc;
	float			ht_max_ipc;

	/* memory bandwidth subcontroller */
	int			bw_threads_monitored;
	int			ok_bw_threads_monitored;
	uint64_t		bw_llc_misses;
};

/**
 * ias_has_priority - does this process have priority access to this core
 * @sd: the process to check
 * @core: the core to check
 *
 * Returns true if the process has priority on this core, allowing it to preempt
 * whatever is currently running there.
 */
static inline bool ias_has_priority(struct ias_data *sd, unsigned int core)
{
	return bitmap_test(sd->claimed_cores, core);
}

extern struct list_head all_procs;
extern struct ias_data *cores[NCPU];
extern uint64_t cores_idle_tsc[NCPU];
extern uint64_t ias_gen[NCPU];

/**
 * ias_for_each_proc - iterates through all processes
 * @proc: a pointer to the current process in the list
 */
#define ias_for_each_proc(proc) \
	list_for_each(&all_procs, proc, all_link)

/**
 * ias_ewma - updates an exponentially-weighted moving average variable
 * @curp: a pointer to the variable to update
 * @newv: the new sampled value
 * @factor: how heavily to weigh the new sample
 */
static inline void ias_ewma(float *curp, float newv, float factor)
{
	if ((*curp) < 1E-3)
		*curp = newv;
	else
		*curp = newv * factor + *curp * (1 - factor);
}

extern int ias_idle_on_core(unsigned int core);
extern int ias_add_kthread_on_core(unsigned int core, uint64_t now_tsc);
extern void ias_migrate_kthread_on_core(int core);

static inline bool is_lc(struct ias_data *sd)
{
        return sd && sd->threads_guaranteed &&
		sd->threads_limit <= sd->threads_guaranteed;
}

/*
 * Locality (LOC) subcontroller definitions
 */

/**
 * ias_loc_score - estimates the amount of cache locality in the core's cache
 * @sd: the process to check
 * @core: the core to check
 * @now_us: the current time in microseconds
 *
 * Returns a locality score, higher is better.
 */
static inline float ias_loc_score(struct ias_data *sd, unsigned int core,
				  uint64_t now_us)
{
	uint64_t delta_us;
	unsigned int sib = sched_siblings[core];

	/* if the sibling is already running the process, locality is perfect */
	if (cores[sib] == sd)
		return 1.1;

	delta_us = now_us - MAX(sd->loc_last_us[core],
				sd->loc_last_us[sib]);

	if (delta_us >= IAS_LOC_EVICTED_US)
		return 0.0;
	return (float)(IAS_LOC_EVICTED_US - delta_us)  / IAS_LOC_EVICTED_US;
}

/*
 * Hyperthread (HT) subcontroller definitions
 */
/**
 * ias_ht_pairing_score - estimates how effective a process pairing is
 *
 * Returns a pairing score, higher is better.
 */
static inline float ias_ht_pairing_score(struct ias_data *sd,
					  struct ias_data *sib_sd)
{
	double ipc =
		sib_sd ? sd->ht_pairing_ipc[sib_sd->idx] :
		sd->ht_unpaired_ipc;
	if (sd->ht_max_ipc < 1E-3)
		return 1.0;
	if (ipc < 1E-3)
		return 1.0;
	return ipc / sd->ht_max_ipc;
}


static inline bool is_banned(struct ias_data *sd, struct ias_data *sib_sd,
			     uint64_t now_tsc)
{
	return sib_sd &&
		(now_tsc - sd->ht_last_banned_tsc[sib_sd->idx] <=
		(uint64_t)cycles_per_us * IAS_BAN_DURATION_US);
}

extern void ias_ht_poll(uint64_t now_us);

/*
 * Bandwidth (BW) subcontroller definitions
 */

extern void ias_bw_poll(uint64_t now_us);

/*
 * Counters
 */

extern uint64_t ias_count_bw_punish;
extern uint64_t ias_count_bw_relax;
extern float    ias_bw_estimate;
extern bool     ias_bw_punish_triggered;
