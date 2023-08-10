#ifdef DIRECTPATH

#include <base/bitmap.h>
#include <util/mmio.h>

#include <unistd.h>

#include "mlx5_ifc.h"
#include "../defs.h"

#include "defs.h"

#define NR_CMD_SLOTS 30
#define VFIO_RESERVED_SLOTS ((1 << 0) | (1 << 31))

enum mlx5_cmd_type {
	MLX5_CMD_EMPTY = 0,
	MLX5_CMD_RSS_UPDATE,
};

struct mlx5_cmd_layout {
	uint8_t		type;
	uint8_t		rsvd0[3];
	__be32		ilen;
	__be64		iptr;
	__be32		in[4];
	__be32		out[4];
	__be64		optr;
	__be32		olen;
	uint8_t		token;
	uint8_t		sig;
	uint8_t		rsvd1;
	uint8_t		status_own;
};

struct cmd_slot {
	struct mlx5_cmd_layout *cmd_lay;
	struct directpath_ctx *cur_ctx;
	int cmd_type;
	size_t submitted_rss_gen;
};

static struct cmd_slot slots[MLX5_MAX_COMMANDS];
static unsigned long free_slot_bitmap;
static unsigned long pending_slots;

static LIST_HEAD(slot_waiters);

static void command_submit_final(uint32_t *in, size_t ilen, size_t olen,
                                 struct cmd_slot *s, int cmd_type)
{
	int ret;
	ret = mlx5_vfio_post_cmd_fast(vfcontext, in, ilen, olen, s - slots);
	BUG_ON(ret);

	s->cmd_type = cmd_type;
	bitmap_set(&pending_slots, s - slots);
}

static void command_rss_update(struct directpath_ctx *ctx, struct cmd_slot *s)
{
	static uint32_t in[DEVX_ST_SZ_DW(modify_rqt_in) + DEVX_ST_SZ_DW(rq_num) * NCPU];
	uint32_t active_qs[ctx->nr_qs];
	unsigned int i, j = 0, inlen, nr_entries, nr_active = 0;

	nr_entries = u16_round_pow2(ctx->nr_qs);
	inlen = DEVX_ST_SZ_BYTES(modify_rqt_in);
	inlen += DEVX_ST_SZ_BYTES(rq_num) * nr_entries;

	DEVX_SET(modify_rqt_in, in, opcode, MLX5_CMD_OP_MODIFY_RQT);
	DEVX_SET(modify_rqt_in, in, rqtn, ctx->rqtn);
	DEVX_SET(modify_rqt_in, in, ctx.rqt_actual_size, nr_entries);
	DEVX_SET(modify_rqt_in, in, bitmask.rqn_list, 1);

	/* active queues get identity steering */
	bitmap_for_each_set(ctx->active_rx_queues, ctx->nr_qs, i) {
		active_qs[nr_active++] = i;
		DEVX_SET(modify_rqt_in, in, ctx.rq_num[i], ctx->rqns[i]);
	}

	/* inactive slots get mapped to active queues */
	bitmap_for_each_cleared(ctx->active_rx_queues, ctx->nr_qs, i) {
		DEVX_SET(modify_rqt_in, in, ctx.rq_num[i],
		         ctx->rqns[active_qs[j++ % nr_active]]);
	}

	/* RSS table is a power of two, fill in extra entries as needed */
	for (i = ctx->nr_qs; i < nr_entries; i++) {
		DEVX_SET(modify_rqt_in, in, ctx.rq_num[i],
		         DEVX_GET(modify_rqt_in, in, ctx.rq_num[i - ctx->nr_qs]));
	}

	s->submitted_rss_gen = ctx->sw_rss_gen;
	command_submit_final(in, inlen, DEVX_ST_SZ_BYTES(modify_rqt_out), s,
	                     MLX5_CMD_RSS_UPDATE);
}

static struct cmd_slot *command_slot_alloc(struct directpath_ctx *ctx)
{
	int slot;
	struct cmd_slot *s;

	BUG_ON(ctx->command_slot >= 0);

	slot = bitmap_find_next_set(&free_slot_bitmap, MLX5_MAX_COMMANDS, 0);
	if (slot == MLX5_MAX_COMMANDS)
		return NULL;

	s = &slots[slot];
	bitmap_clear(&free_slot_bitmap, slot);
	ctx->command_slot = slot;
	s->cur_ctx = ctx;
	s->cmd_type = MLX5_CMD_EMPTY;
	return s;
}

static bool command_ctx_run(struct directpath_ctx *ctx)
{
	struct cmd_slot *s;

	if (unlikely(ctx->kill))
		return false;

	if (ctx->hw_rss_gen < ctx->sw_rss_gen) {
		s = command_slot_alloc(ctx);
		assert(s);
		command_rss_update(ctx, s);
		return true;
	}

	return false;
}

static void command_slot_cleanup(struct cmd_slot *s)
{
	struct directpath_ctx *ctx = s->cur_ctx;

	BUG_ON(!ctx);

	if (s->cmd_type == MLX5_CMD_RSS_UPDATE)
		ctx->hw_rss_gen = s->submitted_rss_gen;

	s->cur_ctx = NULL;
	bitmap_set(&free_slot_bitmap, s - slots);

	/* place context at back of queue */
	list_add_tail(&slot_waiters, &ctx->command_slot_wait_link);
	ctx->command_slot = COMMAND_SLOT_WAITING;
}

void directpath_run_commands(struct directpath_ctx *ctx)
{
	if (directpath_command_queued(ctx))
		return;

	list_add_tail(&slot_waiters, &ctx->command_slot_wait_link);
	ctx->command_slot = COMMAND_SLOT_WAITING;
	proc_get(ctx->p);
}

void directpath_dataplane_notify_kill(struct proc *p)
{
	struct directpath_ctx *ctx = (struct directpath_ctx *)p->directpath_data;
	assert(ctx);
	assert(!ctx->kill);
	ctx->kill = true;
	directpath_run_commands(ctx);
}

void directpath_dataplane_attach(struct proc *p)
{
	struct directpath_ctx *ctx = (struct directpath_ctx *)p->directpath_data;
	assert(ctx);
	directpath_run_commands(ctx);
}

void directpath_handle_cmd_eqe(struct mlx5_eqe *eqe)
{
	int pos;
	ssize_t ret;
	struct cmd_slot *s;
	struct mlx5_eqe_cmd *cmd_eqe = &eqe->data.cmd;
	unsigned long vector = be32toh(cmd_eqe->vector);

	/* notify vfio driver if any of its commands completed */
	if (unlikely(vector & VFIO_RESERVED_SLOTS)) {
		if (vector & (1 << 0))
			mlx5_vfio_deliver_event(vfcontext, 0);

		if (vector & (1 << 31)) {
			ret = write(page_cmd_efd, &(uint64_t){ 1 }, sizeof(uint64_t));
			WARN_ON_ONCE(ret != sizeof(uint64_t));
		}

		log_debug_ratelimited("proxying commands to rdma-core vfio driver");
	}

	/* mask slots used by vfio driver */
	vector &= ~VFIO_RESERVED_SLOTS;

	bitmap_for_each_set(&vector, MLX5_MAX_COMMANDS, pos) {
		s = &slots[pos];

		if (unlikely(DEVX_GET(mbox_out, s->cmd_lay->out, status) !=
		             MLX5_CMD_STAT_OK))
			LOG_CMD_FAIL("handle_cmd_eqe: failed", mbox_out, s->cmd_lay->out);

		command_slot_cleanup(s);
	}
}

bool directpath_commands_poll(void)
{
	bool work_done = false;
	struct directpath_ctx *ctx;

	// bounded by NR_CMD_SLOTS
	while (!list_empty(&slot_waiters) && free_slot_bitmap != 0) {
		ctx = list_pop(&slot_waiters, struct directpath_ctx, command_slot_wait_link);
		if (command_ctx_run(ctx)) {
			work_done = true;
		} else {
			ctx->command_slot = COMMAND_SLOT_UNALLOCATED;
			proc_put(ctx->p);
		}
	}

	// write doorbell vector with pending command slots
	if (pending_slots != 0) {
		mlx5_vfio_post_cmd_db(vfcontext, pending_slots);
		pending_slots = 0;
		work_done = true;
	}

	return work_done;
}

int directpath_commands_init(void)
{
	int i, ret;
	uint32_t slot;
	struct mlx5_cmd_layout *cmd_lay;
	struct cmd_slot *s;

	for (i = 0; i < NR_CMD_SLOTS; i++) {
		ret = mlx5_vfio_cmd_slot_alloc(vfcontext, &slot, &cmd_lay);
		if (ret) {
			log_err("failed to alloc slot");
			return ret;
		}

		BUG_ON(slot >= MLX5_MAX_COMMANDS);
		BUG_ON((1 << slot) & VFIO_RESERVED_SLOTS);

		s = &slots[slot];
		s->cmd_lay = cmd_lay;
		bitmap_set(&free_slot_bitmap, slot);
	}

	return 0;
}

#endif
