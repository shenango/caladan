#ifdef DIRECTPATH

#include <signal.h>

#include <util/mmio.h>
#include <util/udma_barrier.h>

#include "../defs.h"
#include "../hw_timestamp.h"
#include "defs.h"
#include "mlx5_ifc.h"

#define QUEUE_DEMOTION_US 500

static struct mlx5_cqe64 *get_cqe(struct cq *cq, uint32_t idx)
{
	struct mlx5_cqe64 *cqe = &cq->buf[idx & (cq->cqe_cnt - 1)];

	if ((mlx5dv_get_cqe_opcode(cqe) != MLX5_CQE_INVALID) &
	    !((cqe->op_own & MLX5_CQE_OWNER_MASK) ^ !!(idx & (cq->cqe_cnt))))
		return cqe;

	return NULL;
}

static void directpath_arm_queue(struct cq *cq, uint32_t cons_idx)
{
	uint64_t doorbell;
	uint32_t sn;
	uint32_t ci;
	uint32_t cmd;

	BUG_ON(cq->state != RXQ_STATE_ACTIVE);

	sn  = cq->arm_sn++ & 3;
	ci  = cons_idx & 0xffffff;
	cmd = MLX5_CQ_DB_REQ_NOT;

	doorbell = sn << 28 | cmd | ci;
	doorbell <<= 32;
	doorbell |= cq->cqn;

	cq->dbrec[1] = htobe32(sn << 28 | cmd | ci);

	/*
	 * Make sure that the doorbell record in host memory is
	 * written before ringing the doorbell via PCI WC MMIO.
	 */
	mmio_wc_start();
	mmio_write64_be(main_eq.uar->base_addr + MLX5_CQ_DOORBELL, htobe64(doorbell));
	mmio_flush_writes();

	cq->state = RXQ_STATE_ARMED;
}

static uint32_t directpath_cq_cons_tail(struct cq *cq)
{
	return be32toh(ACCESS_ONCE(cq->dbrec[0]));
}

static void directpath_poll_cq(struct directpath_ctx *ctx,
                               unsigned int thread_idx, uint64_t parked_tsc,
                               uint64_t *delay, uint64_t cur_tsc)
{
	uint32_t cur_tail;
	struct cq *cq = &ctx->qps[thread_idx].rx_cq;
	struct mlx5_cqe64 *cqe;

	if (cq->state == RXQ_STATE_DISABLED || cq->state == RXQ_STATE_ARMED)
		return;

	cur_tail = directpath_cq_cons_tail(cq);
	cqe = get_cqe(cq, cur_tail);

	/* report the delay if a packet is waiting */
	if (cqe) {
		*delay = MAX(hw_timestamp_delay_us(cqe), *delay);
		return;
	}

	/* thread is active; no changes to polling state */
	if (!parked_tsc || cfg.no_directpath_active_rss)
		return;

	directpath_arm_queue(cq, cur_tail);
	ctx->nr_armed++;
	return;

#if 0

	/* check if an RSS update completed that disables this queue */
	if (cq->state == RXQ_STATE_DISABLING) {
		if (cq->disable_gen <= ctx->hw_rss_gen) {
			cq->state = RXQ_STATE_DISABLED;
			ctx->disabled_rx_count++;
		}
		return;
	}

	/*
	 * disable this rx queue if:
	 *	(1) it is not the last active queue
	 *  (2) the corresponding thread has been asleep for QUEUE_DEMOTION_US
	 */

	if (ctx->active_rx_count == 1)
		return;

	if (parked_tsc + QUEUE_DEMOTION_US * cycles_per_us > cur_tsc)
		return;

	cq->disable_gen = ++ctx->sw_rss_gen;
	cq->state = RXQ_STATE_DISABLING;
	ctx->active_rx_count--;
	bitmap_clear(ctx->active_rx_queues, thread_idx);
#endif
}

void directpath_poll_proc(struct proc *p, uint64_t *delay_cycles, uint64_t cur_tsc)
{
	struct directpath_ctx *ctx = (struct directpath_ctx *)p->directpath_data;
	struct thread *th;
#if 0
	struct cq *cq;
	struct thread *lastth;
	uint32_t cons_idx;
#endif
	uint64_t delay = 0;
	unsigned int i;

	if (ctx->nr_armed == ctx->nr_qs)
		return;

	for (i = 0; i < ctx->nr_qs; i++) {
		th = &p->threads[i];
		directpath_poll_cq(ctx, i, th->active ? 0 : th->park_tsc, &delay, cur_tsc);
	}

	delay *= cycles_per_us;
	*delay_cycles = MAX(*delay_cycles, delay);

	if (ctx->hw_rss_gen < ctx->sw_rss_gen) {
		if (!directpath_command_queued(ctx))
			directpath_run_commands(ctx);
		return;
	}

#if 0
	if (p->active_thread_count)
		return;

	if (ctx->disabled_rx_count != ctx->nr_qs - 1)
		return;

	lastth = p->active_threads[0];
	if (lastth->park_tsc + QUEUE_DEMOTION_US * cycles_per_us > cur_tsc)
		return;

	return;

	cq = &ctx->qps[lastth - p->threads].rx_cq;
	cons_idx = ACCESS_ONCE(lastth->q_ptrs->directpath_rx_tail);
	directpath_arm_queue(cq, cons_idx);
	ctx->nr_armed++;
#endif
}

void directpath_handle_completion_eqe(struct mlx5_eqe *eqe)
{
	uint32_t cqn = be32toh(eqe->data.comp.cqn);
	struct cq *cq = cqn_to_cq_map[cqn];
	struct directpath_ctx *ctx = container_of(cq, struct directpath_ctx, qps[cq->qp_idx].rx_cq);
#if 0
	struct proc *p = ctx->p;
	struct thread *th = &p->threads[cq->qp_idx];
	struct hwq *h = &th->directpath_hwq;
	BUG_ON(!hwq_busy(h, ACCESS_ONCE(*h->consumer_idx)));
#endif

	// BUG_ON(cq->state != RXQ_STATE_ARMED);
	cq->state = RXQ_STATE_ACTIVE;
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

void directpath_notify_waking(struct proc *p, struct thread *th)
{
	uint32_t tidx = th - p->threads;
	struct directpath_ctx *ctx = (struct directpath_ctx *)p->directpath_data;
	struct cq *cq = &ctx->qps[tidx].rx_cq;

	if (cq->state == RXQ_STATE_ACTIVE || cq->state == RXQ_STATE_ARMED)
		return;

	if (cq->state == RXQ_STATE_DISABLED)
		ctx->disabled_rx_count--;

	cq->state = RXQ_STATE_ACTIVE;
	ctx->active_rx_count++;
	bitmap_set(ctx->active_rx_queues, tidx);
	++ctx->sw_rss_gen;
}



#endif
