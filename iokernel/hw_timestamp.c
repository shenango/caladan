/*
 * hw_timestamp.c - methods for tracking hardware timestamps in MLX5
 */

#ifdef MLX5

#include <asm/ops.h>
#include <base/log.h>
#include <base/stddef.h>
#include <base/time.h>

#include <stdio.h>

#include <infiniband/mlx5dv.h>
#include "../runtime/net/directpath/mlx5/mlx5_ifc.h"

#include "defs.h"
#include "hw_timestamp.h"

/* microeconds per hw tick */
double device_us_per_cycle;
/* current hardware clock time */
uint32_t curr_hw_time;
/* address of hardware clock MMIO registers */
void *hca_core_clock;

static struct ibv_context *context;

char device_name[DEVICE_NAME_MAX];

/* borrowed from DPDK */
static int ibv_device_to_pci_addr(const struct ibv_device *device,
			    struct pci_addr *pci_addr)
{
	FILE *file;
	char line[32];
	char path[strlen(device->ibdev_path) + strlen("/device/uevent") + 1];
	snprintf(path, sizeof(path), "%s/device/uevent", device->ibdev_path);

	file = fopen(path, "rb");
	if (!file)
		return -errno;

	while (fgets(line, sizeof(line), file) == line) {
		size_t len = strlen(line);
		int ret;

		/* Truncate long lines. */
		if (len == (sizeof(line) - 1))
			while (line[(len - 1)] != '\n') {
				ret = fgetc(file);
				if (ret == EOF)
					break;
				line[(len - 1)] = ret;
			}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
		/* Extract information. */
		if (sscanf(line,
			   "PCI_SLOT_NAME="
			   "%04hx:%02hhx:%02hhx.%hhd\n",
			   &pci_addr->domain,
			   &pci_addr->bus,
			   &pci_addr->slot,
			   &pci_addr->func) == 4) {
			break;
		}
#pragma GCC diagnostic pop
	}
	fclose(file);
	return 0;
}


int hw_timestamp_init(void)
{
	int i, ret;
	struct ibv_device **dev_list;
	struct mlx5dv_context_attr mlx5_context_attr = {0};
	struct ibv_device_attr_ex ib_dev_attr;
	struct mlx5dv_context mlx5_ctx = {0};
	struct pci_addr pci_addr;
	unsigned int freq_khz;

	if (cfg.no_hw_qdel)
		return 0;

	if (cfg.vfio_directpath) {
		ret = directpath_get_clock(&freq_khz, &hca_core_clock);
		if (ret) {
			log_err("Error getting clock");
			return ret;
		}

		device_us_per_cycle = 1000.0 / (double)freq_khz;
		log_info("mlx5: device cycles / us: %.4f", 1.0 / device_us_per_cycle);
		return 0;
	}

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return -1;
	}

	for (i = 0; dev_list[i]; i++) {
		if (strncmp(ibv_get_device_name(dev_list[i]), "mlx5", 4))
			continue;

		if (!nic_pci_addr_str)
			break;

		if (ibv_device_to_pci_addr(dev_list[i], &pci_addr)) {
			log_warn("failed to read pci addr for %s, skipping",
				     ibv_get_device_name(dev_list[i]));
			continue;
		}

		if (memcmp(&pci_addr, &nic_pci_addr, sizeof(pci_addr)) == 0)
			break;
	}

	if (!dev_list[i]) {
		log_err("hw_timestamp_init: IB device not found");
		return -1;
	}

	/* grab a copy of the device name */
	strncpy(device_name, dev_list[i]->name, DEVICE_NAME_MAX);

	context = mlx5dv_open_device(dev_list[i], &mlx5_context_attr);
	if (!context) {
		log_err("hw_timestamp_init: Couldn't get context for %s (errno %d)",
			ibv_get_device_name(dev_list[i]), errno);
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


