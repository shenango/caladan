#ifdef DIRECTPATH

#include <base/bitmap.h>
#include <poll.h>
#include <util/udma_barrier.h>
#include <util/mmio.h>

#include "defs.h"
#include "mlx5_ifc.h"

struct eq main_eq;

static void *monitor_ev(void *arg)
{
	int pollfd = mlx5dv_vfio_get_events_fd(vfcontext);
	BUG_ON(pollfd < 0);

	struct pollfd fd =
		{ .fd = pollfd, .events = POLLIN };

	while (true) {
		if (poll(&fd, 1, -1) > 0) {
			mlx5dv_vfio_process_events(vfcontext);
			log_debug("vfio: polled some events");
		}
	}
}

static void free_eq(struct eq *eq)
{
	int ret;

	if (eq->eq) {
		ret = mlx5dv_devx_destroy_eq(eq->eq);
		if (ret)
			log_warn("couldn't free eq");
	}

	if (eq->uar)
		mlx5dv_devx_free_uar(eq->uar);

	if (eq->vec) {
		ret = mlx5dv_devx_free_msi_vector(eq->vec);
		if (ret)
			log_warn("couldn't free msi vector");
	}
}

static int create_eq(struct eq *eq)
{
	uint32_t i;
	uint32_t in[DEVX_ST_SZ_DW(create_eq_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_eq_out)] = {0};
	void *eqc;

	eq->vec = mlx5dv_devx_alloc_msi_vector(vfcontext);
	if (!eq->vec) {
		log_err("failed to alloc msi vec");
		return -1;
	}

	eq->uar = mlx5dv_devx_alloc_uar(vfcontext, MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC);
	if (!eq->uar) {
		log_err("failed to alloc UAR");
		return -1;
	}

	DEVX_SET(create_eq_in, in, opcode, MLX5_CMD_OP_CREATE_EQ);

	eqc = DEVX_ADDR_OF(create_eq_in, in, eq_context_entry);

	DEVX_SET(eqc, eqc, log_eq_size, LOG_EQ_SIZE);
	DEVX_SET(eqc, eqc, uar_page, eq->uar->page_id);
	DEVX_SET(eqc, eqc, intr, eq->vec->vector);

	#define EQ_MASK_NBITS 256
	DEFINE_BITMAP(events, EQ_MASK_NBITS);
	bitmap_init(events, EQ_MASK_NBITS, false);

	bitmap_set(events, MLX5_EVENT_TYPE_SQ_DRAINED);
	bitmap_set(events, MLX5_EVENT_TYPE_SRQ_LAST_WQE);
	bitmap_set(events, MLX5_EVENT_TYPE_SRQ_RQ_LIMIT);
	bitmap_set(events, MLX5_EVENT_TYPE_CQ_ERROR);
	bitmap_set(events, MLX5_EVENT_TYPE_WQ_CATAS_ERROR);
	bitmap_set(events, MLX5_EVENT_TYPE_WQ_INVAL_REQ_ERROR);
	bitmap_set(events, MLX5_EVENT_TYPE_WQ_ACCESS_ERROR);
	bitmap_set(events, MLX5_EVENT_TYPE_SRQ_CATAS_ERROR);
	bitmap_set(events, MLX5_EVENT_TYPE_DB_BF_CONGESTION);
	bitmap_set(events, MLX5_EVENT_TYPE_STALL_EVENT);
	bitmap_set(events, MLX5_EVENT_TYPE_CMD);

	for (i = 0; i < 4; i++)
		DEVX_ARRAY_SET64(create_eq_in, in, event_bitmask, i,
						 events[i]);

	eq->eq = mlx5dv_devx_create_eq(vfcontext, in, sizeof(in), out, sizeof(out));
	if (!eq->eq) {
		LOG_CMD_FAIL("failed to create eq", create_eq_out, out);
		return -1;
	}

	for (i = 0; i < (1U << LOG_EQ_SIZE); i++) {
		struct mlx5_eqe *eqe = eq->eq->vaddr + i * MLX5_EQE_SIZE;
		eqe->owner = 1;
	}

	eq->eqn = DEVX_GET(create_eq_out, out, eq_number);
	eq->nent = 1 << LOG_EQ_SIZE;

	return 0;
}

static struct mlx5_eqe *get_eqe(struct eq *eq, uint32_t idx)
{
	return eq->eq->vaddr + idx * MLX5_EQE_SIZE;
}

static struct mlx5_eqe *get_head_eqe(struct eq *eq)
{
	struct mlx5_eqe *eqe;

	eqe = get_eqe(eq, eq->cons_idx & (eq->nent - 1));
	if ((ACCESS_ONCE(eqe->owner) & 1) ^ !!(eq->cons_idx & eq->nent))
		return NULL;

	udma_from_device_barrier();

	return eqe;
}

static void eq_update_ci(struct eq *eq, int arm)
{
	__be32 *addr = (eq->uar->base_addr + MLX5_EQ_DOORBEL_OFFSET) + (arm ? 0 : 2);
	uint32_t val;

	val = (eq->cons_idx & 0xffffff) | (eq->eqn << 24);

	mmio_write32_be(addr, htobe32(val));
	udma_to_device_barrier();
}

bool directpath_events_poll(void)
{
	unsigned int i;
	struct eq *eq = &main_eq;
	struct mlx5_eqe *eqe;

	for (i = 0; i < POLL_EQ_BATCH_SIZE; i++) {
		eqe = get_head_eqe(eq);
		if (!eqe)
			break;

		switch (eqe->type) {
			case MLX5_EVENT_TYPE_CMD:
				directpath_handle_cmd_eqe(eqe);
				break;
			case MLX5_EVENT_TYPE_COMP:
				directpath_handle_completion_eqe(eqe);
				break;
			case MLX5_EVENT_TYPE_CQ_ERROR:
				directpath_handle_cq_error_eqe(eqe);
				break;
			default:
				log_err("got an eqe! eqe->type: %hhu (%u)", eqe->type, eq->cons_idx);
				break;
		}

		eq->cons_idx++;
	}

	if (i > 0)
		eq_update_ci(eq, 0);

	return i > 0;
}

int events_init(void)
{
	int ret;
	pthread_t mon_thread;

	ret = pthread_create(&mon_thread, NULL, monitor_ev, NULL);
	if (ret) {
		log_err("events_init: pthread_create failed");
		return ret;
	}

	ret = create_eq(&main_eq);
	if (ret) {
		log_err("couldn't create eq");
		free_eq(&main_eq);
		return ret;
	}

	return 0;
}

#endif
