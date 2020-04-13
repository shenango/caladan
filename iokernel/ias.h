/*
 * ias.h - the shared header for the IAS scheduler
 */

#pragma once


/*
 * Constant tunables
 */

/* the maximum number of processes */
#define IAS_NPROC		         32
/* the memory bandwidth limit */
#define IAS_BW_LIMIT		         0.09
/* the time before the core-local cache is assumed to be evicted */
#define IAS_LOC_EVICTED_US	         100
/* the debug info printing interval */
#define IAS_DEBUG_PRINT_US	         1000000


/*
 * Data structures
 */

struct ias_data {
	struct proc		*p;
	unsigned int		is_congested:1;
	unsigned int		is_bwlimited:1;
	unsigned int		is_lc:1;
	unsigned int		idx; /* a unique index */
	uint64_t		qdelay_us;
	struct list_node	all_link;
	DEFINE_BITMAP(reserved_cores, NCPU);

	/* thread usage limits */
	int			threads_guaranteed;/* the number promised */
	int			threads_max;	/* the most possible */
	int			threads_limit;	/* the most allowed */
	int			threads_active;	/* the number active */

	/* locality subcontroller */
	uint64_t		loc_last_us[NCPU];

	/* the hyperthread subcontroller */
	uint64_t		ht_last_rcugen[NCPU];
	uint64_t		ht_last_us[NCPU];
	uint64_t		ht_punish_us;
	uint64_t		ht_punish_count;

	/* memory bandwidth subcontroller */
	float			bw_llc_miss_rate;
};

extern struct list_head all_procs;
extern struct ias_data *cores[NCPU];
extern uint64_t ias_gen[NCPU];

/**
 * ias_for_each_proc - iterates through all processes
 * @proc: a pointer to the current process in the list
 */
#define ias_for_each_proc(proc) \
	list_for_each(&all_procs, proc, all_link)

extern int ias_idle_placeholder_on_core(struct ias_data *sd, unsigned int core);
extern int ias_idle_on_core(unsigned int core);
extern bool ias_can_add_kthread(struct ias_data *sd);
extern int ias_add_kthread(struct ias_data *sd);
extern int ias_add_kthread_on_core(unsigned int core);


/*
 * Hyperthread (HT) subcontroller definitions
 */

extern void ias_ht_poll(uint64_t now_us);
DECLARE_BITMAP(ias_ht_punished_cores, NCPU);


/*
 * Bandwidth (BW) subcontroller definitions
 */

extern void ias_bw_poll(uint64_t now_us);


/*
 * Counters
 */

extern uint64_t ias_bw_punish_count;
extern uint64_t ias_bw_relax_count;
extern float    ias_bw_estimate;
extern uint64_t	ias_bw_sample_failures;
extern uint64_t ias_ht_punish_count;
extern uint64_t ias_ht_relax_count;
