#ifdef DIRECTPATH

#include <signal.h>

#include <util/mmio.h>
#include <util/udma_barrier.h>

#include "../defs.h"
#include "../sched.h"
#include "../hw_timestamp.h"
#include "defs.h"
#include "mlx5_ifc.h"

#define QUEUE_DEMOTION_US 500
#define QUEUE_PROMOTION_US 100

static struct mlx5_cqe64 *get_cqe(struct cq *cq, uint32_t idx)
{
	struct mlx5_cqe64 *cqe = &cq->buf[idx & (cq->cqe_cnt - 1)];

	if ((mlx5dv_get_cqe_opcode(cqe) != MLX5_CQE_INVALID) &
	    !((cqe->op_own & MLX5_CQE_OWNER_MASK) ^ !!(idx & (cq->cqe_cnt))))
		return cqe;

	return NULL;
}

static inline uint32_t directpath_cq_cons_tail(struct cq *cq)
{
	return be32toh(ACCESS_ONCE(cq->dbrec[0]));
}

static void directpath_arm_queue(struct directpath_ctx *ctx, struct cq *cq, uint32_t cons_idx)
{
	uint64_t doorbell;
	uint32_t sn;
	uint32_t ci;
	uint32_t cmd;

	assert(!cq->armed);

	sn  = cq->arm_sn++ & 3;
	ci  = cons_idx & 0xffffff;
	cmd = MLX5_CQ_DB_REQ_NOT;

	doorbell = sn << 28 | cmd | ci;
	doorbell <<= 32;
	doorbell |= cq->cqn;

	cq->dbrec[1] = htobe32(sn << 28 | cmd | ci);

	barrier();
	mmio_write64_be(main_eq.uar->base_addr + MLX5_CQ_DOORBELL, htobe64(doorbell));
	barrier();
	cq->armed = true;
	ctx->nr_armed++;
}

static void directpath_enable_queue(struct directpath_ctx *ctx, unsigned int idx)
{
	struct cq *cq = &ctx->qps[idx].rx_cq;

	if (cq->state == RXQ_STATE_DISABLED)
		ctx->disabled_rx_count--;

	cq->state = RXQ_STATE_ACTIVE;
	ctx->active_rx_count++;
	bitmap_set(ctx->active_rx_queues, idx);
	++ctx->sw_rss_gen;
}

static void directpath_disable_queue(struct directpath_ctx *ctx, unsigned int idx)
{
	struct cq *cq = &ctx->qps[idx].rx_cq;

	cq->disable_gen = ++ctx->sw_rss_gen;
	cq->state = RXQ_STATE_DISABLING;
	ctx->active_rx_count--;
	bitmap_clear(ctx->active_rx_queues, idx);
}

static void directpath_queue_update_state(struct directpath_ctx *ctx,
                                          struct thread *th, unsigned int idx,
                                          uint64_t cur_tsc)
{
	struct cq *cq = &ctx->qps[idx].rx_cq;

	if (cfg.no_directpath_active_rss)
		return;

	if (cq->state == RXQ_STATE_DISABLING &&
	    cq->disable_gen <= ctx->hw_rss_gen) {
		cq->state = RXQ_STATE_DISABLED;
		ctx->disabled_rx_count++;
	}

	if (th->active) {
		if (cq->state != RXQ_STATE_ACTIVE &&
		    cur_tsc - th->change_tsc > QUEUE_PROMOTION_US * cycles_per_us)
			directpath_enable_queue(ctx, idx);
	} else {
		if (ctx->active_rx_count > 1 &&
		    cq->state == RXQ_STATE_ACTIVE &&
		    cur_tsc - th->change_tsc > QUEUE_DEMOTION_US * cycles_per_us)
			directpath_disable_queue(ctx, idx);
	}
}

static void directpath_poll_cq(struct directpath_ctx *ctx,
                               unsigned int thread_idx, struct thread *th,
                               uint64_t *delay, uint64_t cur_tsc)
{
	struct cq *cq = &ctx->qps[thread_idx].rx_cq;
	struct mlx5_cqe64 *cqe = NULL;
	uint32_t cons_idx;

	if (!cq->armed) {
		/* report the delay if a packet is waiting */
		cons_idx = directpath_cq_cons_tail(cq);
		cqe = get_cqe(cq, cons_idx);
		if (cqe)
			*delay = MAX(hw_timestamp_delay_us(cqe), *delay);
		else
			directpath_arm_queue(ctx, cq, cons_idx);
	}

	directpath_queue_update_state(ctx, th, thread_idx, cur_tsc);
}

bool directpath_poll_proc(struct proc *p, uint64_t *delay_cycles, uint64_t cur_tsc)
{
	struct directpath_ctx *ctx = (struct directpath_ctx *)p->directpath_data;
	struct thread *th;
	uint64_t delay = 0;
	unsigned int i;

	if (ctx->nr_armed == ctx->nr_qs && ctx->active_rx_count == 1)
		return true;

	for (i = 0; i < ctx->nr_qs; i++) {
		th = &p->threads[i];
		directpath_poll_cq(ctx, i, th, &delay, cur_tsc);
	}

	delay *= cycles_per_us;
	*delay_cycles = MAX(*delay_cycles, delay);

	if (ctx->hw_rss_gen < ctx->sw_rss_gen && !directpath_command_queued(ctx))
		directpath_run_commands(ctx);

	return false;

}

void directpath_handle_completion_eqe(struct mlx5_eqe *eqe)
{
	uint32_t cqn = be32toh(eqe->data.comp.cqn);
	struct cq *cq = cqn_to_cq_map[cqn];
	struct directpath_ctx *ctx = container_of(cq, struct directpath_ctx, qps[cq->qp_idx].rx_cq);
	struct proc *p = ctx->p;

	if (!sched_threads_active(p))
		sched_add_core(p);

	cq->armed = false;
	ctx->nr_armed--;
}

void directpath_handle_cq_error_eqe(struct mlx5_eqe *eqe)
{
	uint32_t cqn = be32toh(eqe->data.cq_err.cqn) & 0xffffff;
	struct cq *cq = cqn_to_cq_map[cqn];
	struct directpath_ctx *ctx = container_of(cq, struct directpath_ctx, qps[cq->qp_idx].rx_cq);
	struct proc *p = ctx->p;
	log_warn("killing proc with cq overrun");
	kill(p->pid, SIGINT);
}

void directpath_notify_waking(struct proc *p, struct thread *th) {}

#endif
