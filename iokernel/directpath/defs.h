
#pragma once

#include <base/list.h>
#include <base/log.h>
#include <iokernel/shm.h>
#include <net/ethernet.h>

#include <infiniband/mlx5dv.h>

#define MLX5_EQE_SIZE (sizeof(struct mlx5_eqe))
#define LOG_EQ_SIZE 10

#define DEFAULT_CQ_LOG_SZ 8
#define DEFAULT_RQ_LOG_SZ 7
#define DEFAULT_SQ_LOG_SZ 7

#define DIRECTPATH_PORT 1
#define DIRECTPATH_MAX_MTU ETH_MAX_LEN_JUMBO

#define POLL_EQ_BATCH_SIZE 32

extern struct ibv_context *vfcontext;

extern int events_init(void);

struct mlx5_eqe;
struct directpath_ctx;
extern void directpath_handle_cmd_eqe(struct mlx5_eqe *eqe);
extern void directpath_handle_completion_eqe(struct mlx5_eqe *eqe);
extern void directpath_handle_cq_error_eqe(struct mlx5_eqe *eqe);
extern bool directpath_commands_poll(void);
extern bool directpath_events_poll(void);
extern int directpath_commands_init(void);
extern void directpath_run_commands(struct directpath_ctx *ctx);

#define MAX_CQ 65536 // TODO FIX
extern struct cq *cqn_to_cq_map[MAX_CQ];

// Flow steering
#define FLOW_TBL_TYPE 0x0
#define FLOW_TBL_LOG_ENTRIES 12
#define FLOW_TBL_NR_ENTRIES (1 << FLOW_TBL_LOG_ENTRIES)
extern uint32_t table_number;
extern uint32_t flow_group_number;

struct eq {
	uint32_t eqn;
	uint32_t cons_idx;
	uint32_t nent;
	struct mlx5dv_devx_eq *eq;
	struct mlx5dv_devx_msi_vector *vec;
	struct mlx5dv_devx_uar *uar;
};

enum {
	RXQ_STATE_ACTIVE, /* packets may be flowing to this queue, needs polling */
	RXQ_STATE_DISABLING, /* RSS update in flight to disable this queue */
	RXQ_STATE_DISABLED, /* queue fully disabled */
	RXQ_STATE_ARMED, /* queue enabled, first packet will generate an event */
};

struct cq {
	struct mlx5_cqe64 *buf;
	uint32_t *dbrec;
	uint32_t cqn;
	uint32_t cqe_cnt;
	uint64_t disable_gen;
	uint8_t arm_sn;
	uint8_t qp_idx;
	uint8_t state;
	struct mlx5dv_devx_obj *obj;
};

struct wq {
	void *buf;
	uint32_t *dbrec;
	uint32_t stride;
	uint32_t wqe_cnt;
	struct mlx5dv_devx_obj *obj;
};

struct qp {
	uint32_t sqn;
	uint32_t uarn;
	uint32_t uar_offset;
	struct cq rx_cq;
	struct wq rx_wq;
	struct cq tx_cq;
	struct wq tx_wq;
};

#define COMMAND_SLOT_WAITING -1
#define COMMAND_SLOT_UNALLOCATED -2

struct directpath_ctx {
	/* hot data */
	struct proc *p;

	unsigned int kill:1;
	unsigned int fully_armed:1;
	unsigned int use_rmp:1;
	unsigned int has_flow_rule:1;

	/* command data */
	int8_t command_slot;
	struct list_node command_slot_wait_link;

	uint64_t sw_rss_gen;
	uint64_t hw_rss_gen;
	uint32_t active_rx_count;
	uint32_t disabled_rx_count;
	DEFINE_BITMAP(active_rx_queues, NCPU);

	uint32_t *rqns;

	/* cold data */
	struct ibv_pd *pd;
	uint32_t pdn;
	uint32_t tdn;
	uint32_t tisn;
	uint32_t tirn;

	int memfd;
	struct shm_region region;
	size_t region_allocated;
	size_t max_doorbells;
	size_t doorbells_allocated;

	struct wq rmp;
	uint32_t rmpn;

	struct mlx5dv_devx_umem *mem_reg;
	struct mlx5dv_devx_obj *tir_obj;
	struct mlx5dv_devx_obj *tis_obj;
	struct mlx5dv_devx_obj *rqt_obj;
	struct mlx5dv_devx_obj *td_obj;
	int flow_tbl_index;

	struct ibv_mr *mreg;

	uint32_t rqtn;

	ssize_t *uarns;
	size_t nr_alloc_uarn;

	uint32_t nr_qs;
	struct qp qps[];
};

static inline bool directpath_command_queued(struct directpath_ctx *ctx)
{
	return ctx->command_slot >= COMMAND_SLOT_WAITING;
}


#define u32_log(val)				\
	({assert(sizeof(val) == 4);		\
	  32 - __builtin_clz(val - 1);})

#define u32_round_pow2(val) 		\
	({assert(sizeof(val) == 4);		\
		 1U << u32_log(val - 1);})

#define LOG_CMD_FAIL(msg, cmdtype, out)                      \
  do {                                                       \
    log_err("%s %d %x", msg, DEVX_GET(cmdtype, out, status), \
            DEVX_GET(cmdtype, out, syndrome));               \
  } while (0);


extern struct eq main_eq;