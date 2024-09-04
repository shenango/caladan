/*
 * ias.h - the shared header for the IAS scheduler
 */

#pragma once

/*
 * Constant tunables
 */

/* the maximum number of processes */
#define IAS_NPROC			32
/* the memory bandwidth limit */
#define IAS_BW_LIMIT			25000.0
/* the interval that each subcontroller polls */
#define IAS_POLL_INTERVAL_US		10
/* the time before the core-local cache is assumed to be evicted */
#define IAS_LOC_EVICTED_US		100
/* the debug info printing interval */
#define IAS_DEBUG_PRINT_US		1000000
#define IAS_QUANTA_US			2000


/*
 * Data structures
 */

struct ias_data {
	struct proc		*p;
	unsigned int		is_congested:1;
	unsigned int       	has_core_resv:1;
	unsigned int		is_bwlimited:1;
	unsigned int		is_lc:1;
	unsigned int		waking:1;
	uint64_t		qdelay_us;
	uint64_t		last_run_us;

	/* thread usage limits */
	int16_t			threads_guaranteed;/* the number promised */
	int16_t			threads_max;	/* the most possible */
	int16_t			threads_limit;	/* the most allowed */
	int16_t			threads_active;	/* the number active */

	struct list_node	congested_link;
	struct list_node	starved_link;

	DEFINE_BITMAP(reserved_cores, NCPU);

	/* locality subcontroller */
	uint64_t		loc_last_us[NCPU];

	/* the hyperthread subcontroller */
	uint64_t		ht_punish_us;
	uint64_t		ht_punish_count;

	/* the time sharing subcontroller */
	uint64_t		quantum_us;

	/* memory bandwidth subcontroller */
	struct list_node	all_link;
	float			bw_llc_miss_rate;
};

extern struct list_head all_procs;
extern struct ias_data *cores[NCPU];
extern uint64_t ias_gen[NCPU];
extern uint64_t now_us;
extern struct list_head congested_procs[NCPU + 1];
extern uint64_t congested_lc_procs_nr;

/**
 * ias_for_each_proc - iterates through all processes
 * @proc: a pointer to the current process in the list
 */
#define ias_for_each_proc(proc) \
	list_for_each(&all_procs, proc, all_link)

extern int ias_idle_placeholder_on_core(struct ias_data *sd, unsigned int core);
extern int ias_idle_on_core(unsigned int core);
extern bool ias_can_add_kthread(struct ias_data *sd, bool ignore_ht_punish_cores);
extern int ias_add_kthread(struct ias_data *sd);
extern int ias_add_kthread_on_core(unsigned int core);


/*
 * Hyperthread (HT) subcontroller definitions
 */

DECLARE_BITMAP(ias_ht_punished_cores, NCPU);

struct ias_ht_data {
	/* the fraction of the punish budget used so far */
	float		budget_used;
};

extern struct ias_ht_data ias_ht_percore[NCPU];

extern void ias_ht_poll(void);
extern unsigned int ias_ht_relinquish_core(struct ias_data *sd);

static inline float ias_ht_budget_used(unsigned int core)
{
	return ias_ht_percore[core].budget_used;
}


/*
 * Bandwidth (BW) subcontroller definitions
 */

extern void ias_bw_poll(void);
extern int ias_bw_init(void);
extern float ias_bw_estimate_multiplier;


/*
 * Time sharing (TS) subcontroller definitions
 */

extern void ias_ts_poll(void);
extern void ias_core_ts_poll(void);


/*
 * Counters
 */

extern uint64_t ias_bw_punish_count;
extern uint64_t ias_bw_relax_count;
extern float    ias_bw_estimate;
extern uint64_t	ias_bw_sample_failures;
extern uint64_t ias_bw_sample_aborts;
extern uint64_t ias_ht_punish_count;
extern uint64_t ias_ht_relax_count;
extern uint64_t ias_ts_yield_count;
