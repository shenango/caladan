#ifdef DIRECTPATH

#include <signal.h>

#include <util/mmio.h>
#include <util/udma_barrier.h>

#include "../defs.h"
#include "../sched.h"
#include "../hw_timestamp.h"
#include "defs.h"
#include "mlx5_ifc.h"

#define QUEUE_DEMOTION_US 1500
#define QUEUE_PROMOTION_US 300

static void directpath_arm_queue(struct directpath_ctx *ctx, struct cq *cq, uint32_t cons_idx)
{
	uint64_t doorbell;
	uint32_t sn;
	uint32_t ci;
	uint32_t cmd;

	assert(!bitmap_test(ctx->armed_rx_queues, cq->qp_idx));

	sn  = cq->arm_sn++ & 3;
	ci  = cons_idx & 0xffffff;
	cmd = MLX5_CQ_DB_REQ_NOT;

	doorbell = sn << 28 | cmd | ci;
	doorbell <<= 32;
	doorbell |= cq->cqn;

	cq->dbrec[1] = htobe32(sn << 28 | cmd | ci);

	barrier();
	mmio_write64_be(admin_uar->base_addr + MLX5_CQ_DOORBELL, htobe64(doorbell));
	barrier();
	bitmap_set(ctx->armed_rx_queues, cq->qp_idx);
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

static uint64_t directpath_poll_cq_delay(struct directpath_ctx *ctx,
	                                     struct thread *th, struct cq *cq)
{
	uint32_t cons_idx;
	struct mlx5_cqe64 *cqe;

	cons_idx = ACCESS_ONCE(th->q_ptrs->directpath_rx_tail);
	cqe = get_cqe(cq, cons_idx);
	if (!cqe) {
		directpath_arm_queue(ctx, cq, cons_idx);
		return 0;
	}

	return hw_timestamp_delay_us(cqe);
}

void directpath_poll_proc_prefetch(struct proc *p)
{
	if (p->directpath_data)
		prefetch((struct directpath_ctx *)p->directpath_data);
}

void *directpath_poll_proc_prefetch_th0(struct proc *p, uint32_t qidx)
{
	struct directpath_ctx *ctx = (struct directpath_ctx *)p->directpath_data;
	struct cq *cq;

	if (!cfg.vfio_directpath)
		return NULL;

	if (bitmap_test(ctx->armed_rx_queues, qidx))
		return NULL;

	cq = &ctx->qps[qidx].rx_cq;
	prefetch(cq);
	return cq;
}

void directpath_poll_proc_prefetch_th1(void *cqp, uint32_t cons_idx)
{
	struct cq *cq = (struct cq *)cqp;
	struct mlx5_cqe64 *cqe;

	if (cqp == NULL)
		return;

	cqe = &cq->buf[cons_idx & (cq->cqe_cnt - 1)];
	prefetch(cqe);
}

bool directpath_poll_proc(struct proc *p, uint64_t *delay_cycles, uint64_t cur_tsc)
{
	struct directpath_ctx *ctx = (struct directpath_ctx *)p->directpath_data;
	struct cq *cq;
	struct thread *th;
	uint64_t delay = 0;
	int i;

	if (ctx->nr_armed == ctx->nr_qs &&
	    (!cfg.directpath_active_rss || ctx->active_rx_count == 1))
		return true;

	for (i = 0; i < ctx->nr_qs; i++) {
		th = &p->threads[i];
		cq = &ctx->qps[i].rx_cq;

		if (!bitmap_test(ctx->armed_rx_queues, i))
			delay = MAX(directpath_poll_cq_delay(ctx, th, cq), delay);

		if (!cfg.directpath_active_rss)
			continue;

		directpath_queue_update_state(ctx, th, i, cur_tsc);
	}


	delay *= cycles_per_us;
	*delay_cycles = MAX(*delay_cycles, delay);

	if (ctx->hw_rss_gen < ctx->sw_rss_gen && !directpath_command_queued(ctx))
		directpath_run_commands(ctx);

	return false;

}

static void directpath_handle_completion_eqe(struct mlx5_eqe *eqe)
{
	uint32_t cqn = be32toh(eqe->data.comp.cqn);
	struct directpath_ctx *ctx = cqn_to_cq_map[cqn].ctx;
	struct proc *p = ctx->p;

	if (likely(!ctx->kill) && !sched_threads_active(p))
		sched_add_core(p);

	bitmap_clear(ctx->armed_rx_queues, cqn_to_cq_map[cqn].qp_idx);
	ctx->nr_armed--;
}


void directpath_handle_completion_eqe_batch(struct mlx5_eqe **eqe, unsigned int nr)
{
	struct proc *ps[nr];
	struct cq_map_entry entries[nr];

	unsigned int i;

	if (nr < 2) {
		for (i = 0; i < nr; i++)
			directpath_handle_completion_eqe(eqe[i]);
		return;
	}

	for (i = 0; i < nr + 4; i++) {

		/* prefetch cqn to cq entry */
		if (i < nr)
			prefetch(&cqn_to_cq_map[be32toh(eqe[i]->data.comp.cqn)]);

		/* dereference cqn to cq entry, prefetch cq */
		if (i > 0 && i < nr + 1) {
			entries[i - 1] = cqn_to_cq_map[be32toh(eqe[i - 1]->data.comp.cqn)];
			__builtin_prefetch(entries[i - 1].ctx, 1, 0);
		}

		/* prefetch proc associated with context */
		if (i > 1 && i < nr + 2) {
			ps[i - 2] = entries[i - 2].ctx->p;
			__builtin_prefetch(ps[i - 2], 1, 0);

			/* decrement armed count in context */
			entries[i - 2].ctx->nr_armed--;
			bitmap_clear(entries[i - 2].ctx->armed_rx_queues, entries[i - 2].qp_idx);
		}

		/* add a core if the proc needs it */
		if (i > 2 && i < nr + 3) {
			if (!sched_threads_active(ps[i - 3]))
				prefetch((void *)ps[i - 3]->policy_data);
		}

		/* add a core if the proc needs it */
		if (i > 3) {
			if (!sched_threads_active(ps[i - 4]))
				sched_add_core(ps[i - 4]);
		}


	}
}


void directpath_handle_cq_error_eqe(struct mlx5_eqe *eqe)
{
	uint32_t cqn = be32toh(eqe->data.cq_err.cqn) & 0xffffff;
	struct directpath_ctx *ctx = cqn_to_cq_map[cqn].ctx;
	struct proc *p = ctx->p;
	log_warn("killing proc with cq overrun");
	kill(p->pid, SIGINT);
}

void directpath_notify_waking(struct proc *p, struct thread *th) {}

#endif
