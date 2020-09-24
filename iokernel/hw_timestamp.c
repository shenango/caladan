/*
 * hw_timestamp.c - methods for tracking hardware timestamps in MLX5
 */

#ifdef MLX5

#include <asm/ops.h>
#include <base/log.h>
#include <base/stddef.h>
#include <base/time.h>

#include <infiniband/mlx5dv.h>

#include "defs.h"
#include "hw_timestamp.h"

/* microeconds per hw tick */
double device_us_per_cycle;
/* current hardware clock time */
uint32_t curr_hw_time;
/* address of hardware clock MMIO registers */
void *hca_core_clock;

static struct ibv_context *context;

int hw_timestamp_init(void)
{
	int i, ret;
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	struct mlx5dv_context_attr mlx5_context_attr = {0};
	struct ibv_device_attr_ex ib_dev_attr;
	struct mlx5dv_context mlx5_ctx = {0};

	if (cfg.no_hw_qdel)
		return 0;

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return -1;
	}

	i = 0;
	while ((ib_dev = dev_list[i])) {
		// TODO: make this user configurable
		if (strncmp(ibv_get_device_name(ib_dev), "mlx5", 4) == 0)
			break;
		i++;
	}

	if (!ib_dev) {
		log_err("hw_timestamp_init: IB device not found");
		return -1;
	}

	context = mlx5dv_open_device(ib_dev, &mlx5_context_attr);
	if (!context) {
		log_err("hw_timestamp_init: Couldn't get context for %s (errno %d)",
			ibv_get_device_name(ib_dev), errno);
		return -1;
	}

	ret = ibv_query_device_ex(context, 0, &ib_dev_attr);
	if (ret) {
		log_err("hw_timestamp_init: Could not query device (errno %d)", ret);
		return -ret;
	}

	if (!ib_dev_attr.hca_core_clock) {
		log_err("hw_timestamp_init: device frequency unavailable");
		return -EOPNOTSUPP;
	}

	/* hca_core_clock is device tick rate in KHz */
	device_us_per_cycle = 1000.0 / (double)ib_dev_attr.hca_core_clock;
	log_info("mlx5: device cycles / us: %.4f", 1.0 / device_us_per_cycle);

	/* get address of MMIO clock page */
	mlx5_ctx.comp_mask = MLX5DV_CONTEXT_MASK_HCA_CORE_CLOCK;
	ret = mlx5dv_query_device(context, &mlx5_ctx);
	if (ret || !(mlx5_ctx.comp_mask & MLX5DV_CONTEXT_MASK_HCA_CORE_CLOCK)) {
		log_err("hw_timestamp_init: Could not extract clock page");
		return -EOPNOTSUPP;
	}

	hca_core_clock = mlx5_ctx.hca_core_clock;
	return 0;
}

#else

int hw_timestamp_init(void)
{
	return 0;
}

#endif


