
#pragma once

#include <base/list.h>
#include <base/log.h>
#include <iokernel/shm.h>
#include <net/ethernet.h>

#include <infiniband/mlx5dv.h>

#define MLX5_EQE_SIZE (sizeof(struct mlx5_eqe))
#define LOG_EQ_SIZE 10

#define DEFAULT_CQ_LOG_SZ 8
#define DEFAULT_RQ_LOG_SZ 10
#define DEFAULT_SQ_LOG_SZ 7

#define DIRECTPATH_PORT 1
#define DIRECTPATH_MAX_MTU ETH_MAX_LEN_JUMBO

#define POLL_EQ_BATCH_SIZE 32

extern struct ibv_context *vfcontext;
extern int page_cmd_efd;

extern int events_init(void);

struct mlx5_eqe;
struct directpath_ctx;
extern void directpath_handle_cmd_eqe(struct mlx5_eqe *eqe);
extern void directpath_handle_completion_eqe_batch(struct mlx5_eqe **eqe, unsigned int nr);
extern void directpath_handle_cq_error_eqe(struct mlx5_eqe *eqe);
extern bool directpath_commands_poll(void);
extern bool directpath_events_poll(void);
extern int directpath_commands_init(void);
extern void directpath_run_commands(struct directpath_ctx *ctx);
extern int directpath_setup_steering(void);
extern int directpath_steering_attach(struct directpath_ctx *ctx);
extern void directpath_steering_teardown(struct directpath_ctx *ctx);

extern int alloc_raw_ctx(unsigned int nrqs, bool use_rmp,
	struct directpath_ctx **dp_out, bool is_admin);

#define MAX_CQ 65536 // TODO FIX

struct cq_map_entry {
	struct directpath_ctx *ctx;
	uint32_t qp_idx;
};

extern struct cq_map_entry cqn_to_cq_map[MAX_CQ];
extern struct mlx5dv_devx_uar *admin_uar;

extern struct mlx5dv_devx_obj *root_flow_tbl;
extern struct mlx5dv_devx_obj *root_flow_group;

// Flow steering
#define FLOW_TBL_TYPE 0x0

struct eq {
	uint32_t eqn;
	uint32_t cons_idx;
	uint32_t nent;
	struct mlx5dv_devx_eq *eq;
	struct mlx5dv_devx_msi_vector *vec;
};

extern bool directpath_arp_poll(void);
extern int directpath_arp_server_init(void);


enum {
	RXQ_STATE_ACTIVE, /* packets may be flowing to this queue, needs polling */
	RXQ_STATE_DISABLING, /* RSS update in flight to disable this queue */
	RXQ_STATE_DISABLED, /* queue fully disabled */
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
	struct proc		*p;

	uint16_t		nr_armed;
	uint16_t		nr_qs;
	uint16_t		active_rx_count;
	uint16_t		disabled_rx_count;

	uint64_t		sw_rss_gen;
	uint64_t		hw_rss_gen;

	DEFINE_BITMAP(armed_rx_queues, NCPU);

	/* semi hot data */
	/* command data */
	int8_t command_slot;
	struct list_node command_slot_wait_link;

	uint32_t *rqns;

	DEFINE_BITMAP(active_rx_queues, NCPU);

	unsigned int kill:1;
	unsigned int use_rmp:1;


	/* cold data */
	struct ibv_pd *pd;
	uint32_t pdn;

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
	struct mlx5dv_devx_obj *fte;

	struct ibv_mr *mreg;

	uint32_t rqtn;
	uint32_t ft_idx;

	ssize_t *uarns;
	size_t nr_alloc_uarn;

	struct mlx5dv_dr_action *fwd_action;
	struct mlx5dv_dr_rule		*fwd_rule;

	struct qp qps[];
};

BUILD_ASSERT(offsetof(struct directpath_ctx, command_slot) == CACHE_LINE_SIZE);

static inline bool directpath_command_queued(struct directpath_ctx *ctx)
{
	return ctx->command_slot >= COMMAND_SLOT_WAITING;
}

static inline struct mlx5_cqe64 *get_cqe(struct cq *cq, uint32_t idx)
{
	struct mlx5_cqe64 *cqe = &cq->buf[idx & (cq->cqe_cnt - 1)];

	if ((mlx5dv_get_cqe_opcode(cqe) != MLX5_CQE_INVALID) &
	    !((cqe->op_own & MLX5_CQE_OWNER_MASK) ^ !!(idx & (cq->cqe_cnt))))
		return cqe;

	return NULL;
}


#define u32_log(val)				\
	({assert(sizeof(val) == 4);		\
	  32 - __builtin_clz(val - 1);})

#define u32_round_pow2(val) 		\
	({assert(sizeof(val) == 4);		\
		 1U << u32_log(val);})

#define u16_round_pow2(val) 		\
	({assert(sizeof(val) == 2);		\
		 1U << u32_log((uint32_t)val);})

#define LOG_CMD_FAIL(msg, cmdtype, out)                      \
  do {                                                       \
    log_err("%s %d %x", msg, DEVX_GET(cmdtype, out, status), \
            DEVX_GET(cmdtype, out, syndrome));               \
  } while (0);


extern struct eq main_eq;
