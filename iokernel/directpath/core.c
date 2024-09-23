#ifdef DIRECTPATH

#include <base/log.h>
#include <base/time.h>
#include <base/thread.h>
#include <net/mbuf.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <iokernel/directpath.h>
#include <net/ip.h>

#include "../defs.h"
#include "mlx5_ifc.h"
#include <infiniband/mlx5dv.h>
#include <util/udma_barrier.h>
#include <util/mmio.h>

#include "defs.h"

static off_t bar_offs;
static size_t bar_map_size;
static int bar_fd;

#define MAX_PREALLOC 16384
static struct directpath_ctx *preallocated_ctx[MAX_PREALLOC];
static unsigned int nr_prealloc;

struct ibv_context *vfcontext;
struct cq_map_entry cqn_to_cq_map[MAX_CQ];
struct mlx5dv_devx_uar *admin_uar;

static unsigned char rss_key[40] = {
	0x82, 0x19, 0xFA, 0x80, 0xA4, 0x31, 0x06, 0x59, 0x3E, 0x3F, 0x9A,
	0xAC, 0x3D, 0xAE, 0xD6, 0xD9, 0xF5, 0xFC, 0x0C, 0x63, 0x94, 0xBF,
	0x8F, 0xDE, 0xD2, 0xC5, 0xE2, 0x04, 0xB1, 0xCF, 0xB1, 0xB1, 0xA1,
	0x0D, 0x6D, 0x86, 0xBA, 0x61, 0x78, 0xEB};

static void cq_fill_spec(struct shm_region *reg,
	                      struct cq *cq, struct directpath_ring_q_spec *spec)
{
	spec->stride = sizeof(struct mlx5_cqe64);
	spec->nr_entries = cq->cqe_cnt;
	spec->buf = ptr_to_shmptr(reg, cq->buf, spec->stride * spec->nr_entries);
	spec->dbrec = ptr_to_shmptr(reg, cq->dbrec, CACHE_LINE_SIZE);
}

static void wq_fill_spec(struct shm_region *reg,
	                      struct wq *wq, struct directpath_ring_q_spec *spec)
{
	spec->stride = wq->stride;
	spec->nr_entries = wq->wqe_cnt;
	spec->buf = ptr_to_shmptr(reg, wq->buf, spec->stride * spec->nr_entries);
	spec->dbrec = ptr_to_shmptr(reg, wq->dbrec, CACHE_LINE_SIZE);
}

static void qp_fill_spec(struct directpath_ctx *ctx, struct qp *qp,
	                       struct directpath_queue_spec *spec)
{
	spec->sqn = qp->sqn;
	spec->uarn = qp->uarn;
	spec->uar_offset = qp->uar_offset;

	if (!ctx->use_rmp)
		wq_fill_spec(&ctx->region, &qp->rx_wq, &spec->rx_wq);

	cq_fill_spec(&ctx->region, &qp->rx_cq, &spec->rx_cq);
	wq_fill_spec(&ctx->region, &qp->tx_wq, &spec->tx_wq);
	cq_fill_spec(&ctx->region, &qp->tx_cq, &spec->tx_cq);
}

static int directpath_query_port_mtu(int port)
{
	int ret;

	uint32_t in[DEVX_ST_SZ_DW(pmtu_reg)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(pmtu_reg)] = {0};

	DEVX_SET(pmtu_reg, in, local_port, port);
	ret = mlx5_access_reg(vfcontext, in, sizeof(in), out,
	                      sizeof(out), MLX5_REG_PMTU, 0, 0);
	if (ret) {
		log_err("failed to query port mtu (err %d)", ret);
		return ret;
	}

	log_info("directpath: max mtu %u", DEVX_GET(pmtu_reg, out, max_mtu));
	log_info("directpath: oper mtu %u", DEVX_GET(pmtu_reg, out, oper_mtu));
	log_info("directpath: admin mtu %u", DEVX_GET(pmtu_reg, out, admin_mtu));

	return 0;
}

static int directpath_set_port_mtu(int port, uint16_t mtu)
{
	uint32_t in[DEVX_ST_SZ_DW(pmtu_reg)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(pmtu_reg)] = {0};

	DEVX_SET(pmtu_reg, in, admin_mtu, mtu);
	DEVX_SET(pmtu_reg, in, local_port, port);
	return mlx5_access_reg(vfcontext, in, sizeof(in), out,
	                       sizeof(out), MLX5_REG_PMTU, 0, 1);
}

static int mlx5_modify_nic_vport_set_mac(uint8_t *mac)
{
	uint32_t out[DEVX_ST_SZ_DW(modify_nic_vport_context_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(modify_nic_vport_context_in) + DEVX_ST_SZ_DW(mac_address_layout)] = {};
	void *nic_vport_ctx;
	int ret;

	DEVX_SET(modify_nic_vport_context_in, in, opcode,  MLX5_CMD_OP_MODIFY_NIC_VPORT_CONTEXT);
	DEVX_SET(modify_nic_vport_context_in, in, field_select.addresses_list, 1);

	nic_vport_ctx = DEVX_ADDR_OF(modify_nic_vport_context_in, in, nic_vport_context);

	DEVX_SET(nic_vport_context, nic_vport_ctx, allowed_list_type, 0);
	DEVX_SET(nic_vport_context, nic_vport_ctx, allowed_list_size, 1);

	uint8_t *curr_mac = DEVX_ADDR_OF(nic_vport_context,
					    nic_vport_ctx,
					    current_uc_mac_address[0]) + 2;
	memcpy(curr_mac, mac, 6);

	ret = mlx5dv_devx_general_cmd(vfcontext, in, sizeof(in), out, sizeof(out));
	if (ret) {
		LOG_CMD_FAIL("vport set mac", modify_nic_vport_context_out, out);
		return ret;
	}

	return 0;
}

int directpath_query_cq(uint32_t cqn)
{
	uint32_t in[DEVX_ST_SZ_DW(query_cq_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(query_cq_out)];


	DEVX_SET(query_cq_in, in, opcode, MLX5_CMD_OP_QUERY_CQ);
	DEVX_SET(query_cq_in, in, cqn, cqn);

	int ret = mlx5dv_devx_general_cmd(vfcontext, in, sizeof(in), out, sizeof(out));
	if (ret) {
		LOG_CMD_FAIL("query cq", query_cq_out, out);
		return ret;
	}

	log_err("cq status is ok: %d", DEVX_GET(query_cq_out, out, cq_context.status) == 0);
	log_err("cq head: %x", DEVX_GET(query_cq_out, out, cq_context.consumer_counter));
	log_err("cq tail: %x", DEVX_GET(query_cq_out, out, cq_context.producer_counter));
	return 0;
}

static void print_wq_status(void *wq)
{
	log_err("wq head: %x", DEVX_GET(wq, wq, sw_counter));
	log_err("wq tail: %x", DEVX_GET(wq, wq, hw_counter));
}

int directpath_query_rq(uint32_t rqn)
{
	uint32_t in[DEVX_ST_SZ_DW(query_rq_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(query_rq_out)];


	DEVX_SET(query_rq_in, in, opcode, MLX5_CMD_OP_QUERY_RQ);
	DEVX_SET(query_rq_in, in, rqn, rqn);

	int ret = mlx5dv_devx_general_cmd(vfcontext, in, sizeof(in), out, sizeof(out));
	if (ret) {
		LOG_CMD_FAIL("query rq", query_rq_out, out);
		return ret;
	}

	log_err("rq status is ok: %d", DEVX_GET(query_rq_out, out, rq_context.state) == 1);
	print_wq_status(DEVX_ADDR_OF(query_rq_out, out, rq_context.wq));
	return 0;
}

int directpath_query_rmp(uint32_t rmpn)
{
	uint32_t in[DEVX_ST_SZ_DW(query_rmp_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(query_rmp_out)];


	DEVX_SET(query_rmp_in, in, opcode, MLX5_CMD_OP_QUERY_RMP);
	DEVX_SET(query_rmp_in, in, rmpn, rmpn);

	int ret = mlx5dv_devx_general_cmd(vfcontext, in, sizeof(in), out, sizeof(out));
	if (ret) {
		LOG_CMD_FAIL("query rmp", query_rmp_out, out);
		return ret;
	}

	log_err("rmp status is ok: %d", DEVX_GET(query_rmp_out, out, rmp_context.state) == 1);
	print_wq_status(DEVX_ADDR_OF(query_rmp_out, out, rmp_context.wq));
	return 0;
}


static int directpath_query_nic_vport(void)
{
	uint32_t in[DEVX_ST_SZ_DW(query_nic_vport_context_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(query_nic_vport_context_out)];
	int ret;
	void *nic_vport_ctx;

	DEVX_SET(query_nic_vport_context_in, in, opcode, 0x754);
	DEVX_SET(query_nic_vport_context_in, in, allowed_list_type, 0 /*current_uc_mac_address*/);

	ret = mlx5dv_devx_general_cmd(vfcontext, in, sizeof(in), out, sizeof(out));
	if (ret) {
		LOG_CMD_FAIL("query vport", query_nic_vport_context_out, out);
		return ret;
	}

	nic_vport_ctx = DEVX_ADDR_OF(query_nic_vport_context_out, out,
	                             nic_vport_context);
	uint8_t *mac_addr = DEVX_ADDR_OF(nic_vport_context,
	                        nic_vport_ctx,
	                        permanent_address) + 2;
	log_info("directpath: mac %02X:%02X:%02X:%02X:%02X:%02X", mac_addr[0], mac_addr[1],
		     mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

	memcpy(&iok_info->host_mac, mac_addr, sizeof(iok_info->host_mac));

	mlx5_modify_nic_vport_set_mac(mac_addr);

	log_info("directpath: promisc mc: %d", DEVX_GET(nic_vport_context, nic_vport_ctx, promisc_mc));
	log_info("directpath: mtu: %d", DEVX_GET(nic_vport_context, nic_vport_ctx, mtu));

	return 0;
}

static int directpath_setup_port(void)
{
	uint32_t out[DEVX_ST_SZ_DW(modify_nic_vport_context_out)] = {0};
	uint32_t in[DEVX_ST_SZ_DW(modify_nic_vport_context_in)] = {0};
	int ret;

	DEVX_SET(modify_nic_vport_context_in, in, opcode, MLX5_CMD_OP_MODIFY_NIC_VPORT_CONTEXT);
	DEVX_SET(modify_nic_vport_context_in, in, field_select.promisc, 1);
	DEVX_SET(modify_nic_vport_context_in, in, nic_vport_context.promisc_uc, 1);
	DEVX_SET(modify_nic_vport_context_in, in, nic_vport_context.promisc_mc, 1);
	DEVX_SET(modify_nic_vport_context_in, in, nic_vport_context.promisc_all, 1);



	DEVX_SET(modify_nic_vport_context_in, in, field_select.mtu, 1);
	DEVX_SET(modify_nic_vport_context_in, in, nic_vport_context.mtu, DIRECTPATH_MAX_MTU);

	ret = mlx5dv_devx_general_cmd(vfcontext, in, sizeof(in), out, sizeof(out));
	if (ret) {
		LOG_CMD_FAIL("setup port context", modify_nic_vport_context_out, out);
		return ret;
	}
	ret = directpath_set_port_mtu(DIRECTPATH_PORT, DIRECTPATH_MAX_MTU);
	if (ret)
		return ret;

	ret = directpath_query_port_mtu(DIRECTPATH_PORT);
	if (ret)
		return ret;

	ret = directpath_query_nic_vport();
	if (ret)
		return ret;

	return 0;
}

static void qp_fill_iokspec(struct thread *th, struct qp *qp)
{
	struct hwq *h = &th->directpath_hwq;

	h->hwq_type = HWQ_INVALID;
	h->enabled = false;
}

static unsigned int ctx_max_doorbells(struct directpath_ctx *dp)
{
	return !!dp->use_rmp + 4 * dp->nr_qs;
}

static size_t ctx_rx_buffer_region(struct directpath_ctx *dp)
{
	size_t reg_size;

	if (dp->use_rmp)
		reg_size = DIRECTPATH_STRIDE_RX_BUF_POOL_SZ;
	else
		reg_size = (1 << DEFAULT_RQ_LOG_SZ) * (1 + dp->nr_qs) * DIRECTPATH_MAX_MTU;

	return align_up(reg_size, PGSIZE_2MB);
}

static size_t ctx_tx_buffer_region(struct directpath_ctx *dp)
{
	/* 4 MB */
	return 2 * DIRECTPATH_STRIDE_RX_BUF_POOL_SZ;
}

static size_t estimate_region_size(struct directpath_ctx *dp)
{
	uint32_t i;
	size_t total = 0;
	size_t wasted = 0;
	size_t lg_rq_sz = DEFAULT_RQ_LOG_SZ;

	/* doorbells */
	total += CACHE_LINE_SIZE * ctx_max_doorbells(dp);

#define ALLOC(size, alignment) do {                     \
		wasted += align_up(total, (alignment)) - total; \
		total = align_up(total, (alignment));           \
		total += (size);                                \
	} while (0);

	if (dp->use_rmp) {
		ALLOC(DIRECTPATH_STRIDE_RQ_NUM_DESC * sizeof(struct mlx5_mprq_wqe), PGSIZE_4KB);
		lg_rq_sz = __builtin_ctzl(DIRECTPATH_TOTAL_RX_EL);
	}

	for (i = 0; i < dp->nr_qs; i++) {
		/* cq for rq */
		ALLOC((1UL << lg_rq_sz) * sizeof(struct mlx5_cqe64), PGSIZE_4KB);

		/* cq for sq */
		ALLOC((1UL << DEFAULT_SQ_LOG_SZ) * sizeof(struct mlx5_cqe64), PGSIZE_4KB);

		/* wq for rq */
		if (!dp->use_rmp)
			ALLOC((1UL << DEFAULT_RQ_LOG_SZ) * MLX5_SEND_WQE_BB, PGSIZE_4KB);

		/* wq for sq */
		ALLOC((1UL << DEFAULT_SQ_LOG_SZ) * MLX5_SEND_WQE_BB, PGSIZE_4KB);
	}

	/* rx buffer region */
	ALLOC(ctx_rx_buffer_region(dp), PGSIZE_2MB);
	log_debug("allocating %lu bytes for rx buf pool", ctx_rx_buffer_region(dp));

	/* tx buffer region */
	ALLOC(ctx_tx_buffer_region(dp), PGSIZE_2MB);
	log_debug("allocating %lu bytes for tx buf pool", ctx_tx_buffer_region(dp));

	log_debug("reg size %lu, wasted %lu", total, wasted);

#undef ALLOC

	return total;
}

static int alloc_from_uregion(struct directpath_ctx *dp, size_t size, size_t alignment, uint64_t *alloc_out)
{
	size_t aligned_start;

	if (!is_power_of_two(alignment))
		return -EINVAL;

	if (size == CACHE_LINE_SIZE) {

		if (dp->doorbells_allocated == dp->max_doorbells) {
			log_err("too many doorbells");
			return -ENOMEM;
		}

		*alloc_out = dp->doorbells_allocated++ * CACHE_LINE_SIZE;
		return 0;
	}

	aligned_start = align_up(dp->region_allocated, alignment);

	if (aligned_start > dp->region.len) {
		log_err("exhausted reg len");
		return -ENOMEM;
	}

	if (dp->region.len - aligned_start < size) {
		log_err("exhausted reg len");
		return -ENOMEM;
	}

	*alloc_out = aligned_start;
	dp->region_allocated = aligned_start + size;

	return 0;
}

static int activate_rq(struct qp *qp, uint32_t rqn)
{
	int ret;
	uint32_t in[DEVX_ST_SZ_DW(modify_rq_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(modify_rq_out)] = {0};
	void *rqc;

	DEVX_SET(modify_rq_in, in, opcode, MLX5_CMD_OP_MODIFY_RQ);
	DEVX_SET(modify_rq_in, in, rq_state, 0 /* currently RST */);
	DEVX_SET(modify_rq_in, in, rqn, rqn);

	rqc = DEVX_ADDR_OF(modify_rq_in, in, ctx);
	DEVX_SET(rqc, rqc, state, 1 /* RDY */);

	ret = mlx5dv_devx_obj_modify(qp->rx_wq.obj, in, sizeof(in), out, sizeof(out));
	if (ret) {
		log_err("failed to set RQ to RDY");
		return ret;
	}

	return 0;
}


static int activate_sq(struct qp *qp)
{
	int ret;
	uint32_t in[DEVX_ST_SZ_DW(modify_sq_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(modify_sq_out)] = {0};
	void *sqc;

	DEVX_SET(modify_sq_in, in, opcode, MLX5_CMD_OP_MODIFY_SQ);
	DEVX_SET(modify_sq_in, in, sq_state, 0 /* currently RST */);
	DEVX_SET(modify_sq_in, in, sqn, qp->sqn);

	sqc = DEVX_ADDR_OF(modify_sq_in, in, sq_context);
	DEVX_SET(sqc, sqc, state, 1 /* RDY */);

	ret = mlx5dv_devx_obj_modify(qp->tx_wq.obj, in, sizeof(in), out, sizeof(out));
	if (ret) {
		log_err("failed to set RQ to RDY");
		return ret;
	}

	return 0;
}

static int alloc_td(struct directpath_ctx *dp, uint32_t *tdn)
{
	uint32_t in[DEVX_ST_SZ_DW(alloc_transport_domain_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(alloc_transport_domain_out)] = {0};

	DEVX_SET(alloc_transport_domain_in, in, opcode, MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN);

	dp->td_obj = mlx5dv_devx_obj_create(vfcontext, in, sizeof(in), out, sizeof(out));
	if (!dp->td_obj) {
		log_err("couldn't create td obj");
		return -1;
	}

	*tdn = DEVX_GET(alloc_transport_domain_out, out, transport_domain);
	return 0;
}

static int create_tis(struct directpath_ctx *dp, uint32_t tdn, uint32_t *tisn)
{
	uint32_t in[DEVX_ST_SZ_DW(create_tis_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_tis_out)] = {0};
	void *ctx;

	DEVX_SET(create_tis_in, in, opcode, MLX5_CMD_OP_CREATE_TIS);
	ctx = DEVX_ADDR_OF(create_tis_in, in, ctx);

	DEVX_SET(tisc, ctx, tls_en, 0);
	DEVX_SET(tisc, ctx, transport_domain, tdn);

	dp->tis_obj = mlx5dv_devx_obj_create(vfcontext, in, sizeof(in), out, sizeof(out));
	if (!dp->tis_obj) {
		log_err("failed to create TIS");
		return -1;
	}

	*tisn = DEVX_GET(create_tis_out, out, tisn);

	return 0;
}

static int fill_wqc(struct directpath_ctx *dp, struct qp *qp, struct wq *wq,
	void *wqc, uint32_t log_nr_wqe, bool is_rq)
{
	int ret;
	size_t wqe_cnt = 1UL << log_nr_wqe;
	uint64_t bufs, dbr;
	size_t bbsz = MLX5_SEND_WQE_BB;

	if (!is_rq)
		bbsz = MLX5_SEND_WQE_BB;
	else if (dp->use_rmp)
		bbsz = sizeof(struct mlx5_mprq_wqe);
	else
		bbsz = sizeof(struct mlx5_wqe_data_seg);

	ret = alloc_from_uregion(dp, bbsz * wqe_cnt, PGSIZE_4KB, &bufs);
	if (ret)
		return -ENOMEM;

	ret = alloc_from_uregion(dp, CACHE_LINE_SIZE, CACHE_LINE_SIZE, &dbr);
	if (ret)
		return -ENOMEM;

	DEVX_SET(wq, wqc, wq_type, 1 /* WQ_CYCLIC */);

	if (is_rq && dp->use_rmp) {
		int16_t lg_nm_strides = __builtin_ctz(DIRECTPATH_NUM_STRIDES) - 9;
		DEVX_SET(wq, wqc, wq_type, 0x3 /* CYCLIC_STRIDING_WQ */);
		DEVX_SET(wq, wqc, single_wqe_log_num_of_strides, lg_nm_strides /* 512 ^ {2 ^ lg_nm_strides}*/);
		DEVX_SET(wq, wqc, two_byte_shift_en, 1);
		uint16_t lg_stride_size = __builtin_ctz(DIRECTPATH_STRIDE_SIZE / 64);
		DEVX_SET(wq, wqc, single_stride_log_num_of_bytes, lg_stride_size /* 64 * {2 ^ lg_stride_size} */);
	} else if (!is_rq) {
		DEVX_SET(wq, wqc, uar_page, qp->uarn);
	}

	DEVX_SET(wq, wqc, pd, dp->pdn);
	DEVX_SET64(wq, wqc, dbr_addr, dbr);
	DEVX_SET(wq, wqc, log_wq_stride, __builtin_ctz(bbsz));
	DEVX_SET(wq, wqc, log_wq_sz, log_nr_wqe);
	DEVX_SET(wq, wqc, dbr_umem_valid, 1);
	DEVX_SET(wq, wqc, wq_umem_valid, 1);
	DEVX_SET(wq, wqc, dbr_umem_id, dp->mem_reg->umem_id);
	DEVX_SET(wq, wqc, wq_umem_id, dp->mem_reg->umem_id);
	DEVX_SET64(wq, wqc, wq_umem_offset, bufs);

	wq->stride = bbsz;
	wq->buf = dp->region.base + bufs;
	wq->dbrec = dp->region.base + dbr;
	wq->wqe_cnt = wqe_cnt;

	return 0;
}

static int create_sq(struct directpath_ctx *dp, struct qp *qp,
	uint32_t log_nr_wqe, uint32_t tisn)
{
	int ret;
	uint32_t in[DEVX_ST_SZ_DW(create_sq_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_sq_out)] = {0};
	void *sqc, *wqc;

	DEVX_SET(create_sq_in, in, opcode, MLX5_CMD_OP_CREATE_SQ);
	sqc = DEVX_ADDR_OF(create_sq_in, in, ctx);

	DEVX_SET(sqc, sqc, cqn, qp->tx_cq.cqn);
	DEVX_SET(sqc, sqc, tis_lst_sz, 1);
	DEVX_SET(sqc, sqc, tis_num_0, tisn);

	wqc = DEVX_ADDR_OF(sqc, sqc, wq);
	ret = fill_wqc(dp, qp, &qp->tx_wq, wqc, log_nr_wqe, false);
	if (ret)
		return ret;

	qp->tx_wq.obj = mlx5dv_devx_obj_create(vfcontext, in, sizeof(in), out, sizeof(out));
	if (!qp->tx_wq.obj) {
		log_err("failed to create sq obj %d %x", DEVX_GET(create_sq_out, out, status), DEVX_GET(create_sq_out, out, syndrome));
		return -1;
	}

	BUILD_ASSERT(MLX5_SEND_WQE_BB == 1 << MLX5_SEND_WQE_SHIFT);

	qp->sqn = DEVX_GET(create_sq_out, out, sqn);

	return activate_sq(qp);
}


static int create_rq(struct directpath_ctx *dp, struct qp *qp, uint32_t log_nr_wqe, int idx)
{
	int ret;
	uint32_t in[DEVX_ST_SZ_DW(create_rq_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_rq_out)] = {0};
	void *rq_ctx, *wq_ctx;

	DEVX_SET(create_rq_in, in, opcode, MLX5_CMD_OP_CREATE_RQ);
	rq_ctx = DEVX_ADDR_OF(create_rq_in, in, ctx);

	DEVX_SET(rqc, rq_ctx, delay_drop_en, 0); // TODO: can this work?
	DEVX_SET(rqc, rq_ctx, mem_rq_type, 0 /* RQ_INLINE */);
	DEVX_SET(rqc, rq_ctx, state, 0 /* RST */);

	DEVX_SET(rqc, rq_ctx, cqn, qp->rx_cq.cqn);

	if (dp->use_rmp) {
		DEVX_SET(rqc, rq_ctx, mem_rq_type, 1 /* RQ_INLINE */);
		DEVX_SET(rqc, rq_ctx, rmpn, dp->rmpn);
		DEVX_SET(rqc, rq_ctx, user_index, 1);
	} else {
		wq_ctx = DEVX_ADDR_OF(rqc, rq_ctx, wq);
		ret = fill_wqc(dp, qp, &qp->rx_wq, wq_ctx, log_nr_wqe, true);
		if (ret)
			return ret;
	}

	qp->rx_wq.obj = mlx5dv_devx_obj_create(vfcontext, in, sizeof(in), out, sizeof(out));
	if (unlikely(!qp->rx_wq.obj)) {
		LOG_CMD_FAIL("rq", create_rq_out, out);
		return -1;
	}

	dp->rqns[idx] = DEVX_GET(create_rq_out, out, rqn);
	return activate_rq(qp, dp->rqns[idx]);
}

static int create_cq(struct directpath_ctx *dp, struct cq *cq, uint32_t log_nr_cq, bool monitored, uint32_t qp_idx)
{
	int i, ret;
	uint32_t in[DEVX_ST_SZ_DW(create_cq_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_cq_out)] = {0};
	uint64_t cqe_cnt = 1U << log_nr_cq;
	void *cq_ctx;
	uint64_t bufs, dbr;

	DEVX_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
	cq_ctx = DEVX_ADDR_OF(create_cq_in, in, cq_context);

	ret = alloc_from_uregion(dp, cqe_cnt * sizeof(struct mlx5_cqe64), PGSIZE_4KB, &bufs);
	if (ret)
		return -ENOMEM;

	ret = alloc_from_uregion(dp, CACHE_LINE_SIZE, CACHE_LINE_SIZE, &dbr);
	if (ret)
		return -ENOMEM;

	cq->buf = dp->region.base + bufs;
	cq->dbrec = dp->region.base + dbr;
	cq->cqe_cnt = cqe_cnt;

	for (i = 0; i < cqe_cnt; i++)
		mlx5dv_set_cqe_owner(&cq->buf[i], 1);

	DEVX_SET(cqc, cq_ctx, cqe_sz, 0 /* 64B CQE */); // TODO 1 for INDIRECT
	DEVX_SET(cqc, cq_ctx, cc, 0 /* not collapsed to first entry */); // TODO 1 for INDIRECT
	DEVX_SET(cqc, cq_ctx, scqe_break_moderation_en, 0); // solicited CQE does not break event moderation!
	DEVX_SET(cqc, cq_ctx, oi, 0 /* no overrun ignore */);

	enum {
		CQ_PERIOD_MODE_UPON_EVENT = 0,
		CQ_PERIOD_MODE_UPON_CQE = 1,
	};

	DEVX_SET(cqc, cq_ctx, cq_period_mode, 1); // TODO figure this out
	DEVX_SET(cqc, cq_ctx, cqe_comp_en, 0 /* no compression */); // TODO enable this
	// DEVX_SET(cqc, cq_ctx, mini_cqe_res_format, );
	// DEVX_SET(cqc, cq_ctx, cqe_comp_layout, 0 /* BASIC_CQE_COMPRESSION */);
	// DEVX_SET(cqc, cq_ctx, mini_cqe_res_format_ext, );
	DEVX_SET(cqc, cq_ctx, cq_timestamp_format, 0 /* INTERNAL_TIMER */);
	DEVX_SET(cqc, cq_ctx, log_cq_size, log_nr_cq);
	// TODO maybe set this to something else for non-monitored
	DEVX_SET(cqc, cq_ctx, uar_page, admin_uar->page_id);

	if (monitored) {
		DEVX_SET(cqc, cq_ctx, cq_period, 0);
		DEVX_SET(cqc, cq_ctx, cq_max_count, 1); // TODO?
	} else {
		DEVX_SET(cqc, cq_ctx, cq_period, 0);
		DEVX_SET(cqc, cq_ctx, cq_max_count, 0);
	}

	DEVX_SET(cqc, cq_ctx, c_eqn, main_eq.eqn);

	DEVX_SET(create_cq_in, in, cq_umem_valid, 1);
	DEVX_SET(create_cq_in, in, cq_umem_id, dp->mem_reg->umem_id);
	DEVX_SET64(create_cq_in, in, cq_umem_offset, bufs);

	DEVX_SET(cqc, cq_ctx, dbr_umem_valid, 1);
	DEVX_SET(cqc, cq_ctx, dbr_umem_id, dp->mem_reg->umem_id);
	DEVX_SET64(cqc, cq_ctx, dbr_addr, dbr);

	cq->obj = mlx5dv_devx_obj_create(vfcontext, in, sizeof(in), out, sizeof(out));
	if (!cq->obj) {
		log_err("create cq obj failed status %d syndrome %x", DEVX_GET(create_cq_out, out, status), DEVX_GET(create_cq_out, out, syndrome));
		return -1;
	}

	cq->cqn = DEVX_GET(create_cq_out, out, cqn);

	if (monitored) {
		cq->qp_idx = qp_idx;
		BUG_ON(cq->cqn >= MAX_CQ);
		cqn_to_cq_map[cq->cqn].ctx = dp;
		cqn_to_cq_map[cq->cqn].qp_idx = qp_idx;
		if (!cfg.directpath_active_rss || qp_idx == 0) {
			dp->active_rx_count++;
			bitmap_set(dp->active_rx_queues, qp_idx);
			cq->state = RXQ_STATE_ACTIVE;
		} else {
			dp->disabled_rx_count++;
			cq->state = RXQ_STATE_DISABLED;
		}
	}

	return 0;
}

static int create_rmp(struct directpath_ctx *dp, struct wq *wq,
                      uint32_t log_nr_wqe, uint32_t *rmpn)
{
	int ret;
	uint32_t in[DEVX_ST_SZ_DW(create_rmp_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_rmp_out)] = {0};
	void *rmp_ctx, *wq_ctx;

	DEVX_SET(create_rmp_in, in, opcode, MLX5_CMD_OP_CREATE_RMP);

	rmp_ctx = DEVX_ADDR_OF(create_rmp_in, in, ctx);
	DEVX_SET(rmpc, rmp_ctx, state, MLX5_RMPC_STATE_RDY);
	DEVX_SET(rmpc, rmp_ctx, basic_cyclic_rcv_wqe, 0); // TODO?

	wq_ctx = DEVX_ADDR_OF(rmpc, rmp_ctx, wq);
	ret = fill_wqc(dp, NULL, wq, wq_ctx, log_nr_wqe, true);
	if (ret) {
		log_err("failed to setup rmp wqc");
		return ret;
	}

	wq->obj = mlx5dv_devx_obj_create(vfcontext, in, sizeof(in), out, sizeof(out));
	if (unlikely(!wq->obj)) {
		LOG_CMD_FAIL("rmp", create_rmp_out, out);
		return -1;
	}

	*rmpn = DEVX_GET(create_rmp_out, out, rmpn);
	return 0;
}

bool directpath_poll(void)
{
	bool work_done;

	work_done = directpath_events_poll();

	work_done |= directpath_commands_poll();

	work_done |= directpath_arp_poll();

	return work_done;
}

static void *directpath_init_thread_poll(void *arg)
{
	volatile bool *stop = (volatile bool *)arg;

	while (!*stop) {
		directpath_events_poll();
		sched_yield();
	}

	return NULL;
}

static int create_rqt(struct directpath_ctx *dp)
{
	size_t inlen;
	uint32_t *in, i;
	uint32_t out[DEVX_ST_SZ_DW(create_rqt_out)] = {0};
	uint32_t nr_entries = u16_round_pow2(dp->nr_qs);
	void *rqtc;

	inlen = DEVX_ST_SZ_BYTES(create_rqt_in) + DEVX_ST_SZ_BYTES(rq_num) * nr_entries;
	in = calloc(1, inlen);
	if (!in)
		return -ENOMEM;

	DEVX_SET(create_rqt_in, in, opcode, MLX5_CMD_OP_CREATE_RQT);
	rqtc = DEVX_ADDR_OF(create_rqt_in, in, rqt_context);

	DEVX_SET(rqtc, rqtc, rqt_max_size, nr_entries);
	DEVX_SET(rqtc, rqtc, rqt_actual_size, nr_entries);

	for (i = 0; i < nr_entries; i++) {
		unsigned int idx = !cfg.directpath_active_rss ? (i % dp->nr_qs) : 0;
		DEVX_SET(rqtc, rqtc, rq_num[i], dp->rqns[idx]);
	}

	dp->rqt_obj = mlx5dv_devx_obj_create(vfcontext, in, inlen, out, sizeof(out));
	free(in);
	if (!dp->rqt_obj) {
		LOG_CMD_FAIL("create rqt", create_rqt_out, out);
		return -1;
	}

	dp->rqtn = DEVX_GET(create_rqt_out, out, rqtn);

	return 0;
}

static int create_tir(struct directpath_ctx *dp, uint32_t tdn)
{
	uint32_t in[DEVX_ST_SZ_DW(create_tir_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_tir_out)] = {0};
	void *tir_ctx, *hf;

	DEVX_SET(create_tir_in, in, opcode, MLX5_CMD_OP_CREATE_TIR);
	tir_ctx = DEVX_ADDR_OF(create_tir_in, in, ctx);

	DEVX_SET(tirc, tir_ctx, disp_type, 1 /* INDIRECT */);
	DEVX_SET(tirc, tir_ctx, lro_enable_mask, 0);

	DEVX_SET(tirc, tir_ctx, rx_hash_symmetric, 0);
	DEVX_SET(tirc, tir_ctx, indirect_table, dp->rqtn);
	DEVX_SET(tirc, tir_ctx, rx_hash_fn, 2 /* TOEPLITZ */);
	DEVX_SET(tirc, tir_ctx, self_lb_block, 0); // TODO?

	DEVX_SET(tirc, tir_ctx, transport_domain, tdn);

	BUILD_ASSERT(DEVX_FLD_SZ_BYTES(tirc, rx_hash_toeplitz_key) == sizeof(rss_key));
	memcpy(DEVX_ADDR_OF(tirc, tir_ctx, rx_hash_toeplitz_key), rss_key, sizeof(rss_key));

	hf = DEVX_ADDR_OF(tirc, tir_ctx, rx_hash_field_selector_outer);
	DEVX_SET(rx_hash_field_select, hf, l3_prot_type, 0 /* IPV4 */);
	DEVX_SET(rx_hash_field_select, hf, l4_prot_type, 1 /* UDP */); //TODO: TCP TIR also
	DEVX_SET(rx_hash_field_select, hf, selected_fields, 0xF /* L3 + L4, IP/PORT */);

	dp->tir_obj = mlx5dv_devx_obj_create(vfcontext, in, sizeof(in), out, sizeof(out));
	if (!dp->tir_obj) {
		log_err("create tis obj failed");
		return -1;
	}

	dp->fwd_action = mlx5dv_dr_action_create_dest_devx_tir(dp->tir_obj);
	if (!dp->fwd_action) {
		log_err("failed to create fwd action %d", errno);
		return -1;
	}

	return 0;
}

static ssize_t alloc_uar(void)
{
	uint32_t out[DEVX_ST_SZ_DW(alloc_uar_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(alloc_uar_in)] = {};
	int err;

	DEVX_SET(alloc_uar_in, in, opcode, MLX5_CMD_OP_ALLOC_UAR);
	err = mlx5dv_devx_general_cmd(vfcontext, in, sizeof(in), out, sizeof(out));
	if (err) {
		log_err("failed to alloc uar");
		return -1;
	}

	return DEVX_GET(alloc_uar_out, out, uar);
}

static void dealloc_uar(ssize_t uarn)
{
	uint32_t out[DEVX_ST_SZ_DW(dealloc_uar_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(dealloc_uar_in)] = {};
	int err;

	DEVX_SET(dealloc_uar_in, in, opcode, MLX5_CMD_OP_DEALLOC_UAR);
	DEVX_SET(dealloc_uar_in, in, uar, uarn);

	err = mlx5dv_devx_general_cmd(vfcontext, in, sizeof(in), out, sizeof(out));
	if (err)
		log_err("failed to free uarn %ld", uarn);
}

static int allocate_user_memory(struct directpath_ctx *dp)
{
	int ret;
	int flags = cfg.no_hugepages ? 0 : MFD_HUGETLB;

	dp->memfd = memfd_create("directpath", flags); // Maybe huge?
	if (dp->memfd < 0) {
		log_err("failed to create memfd!");
		return -errno;
	}

	dp->region.len = align_up(estimate_region_size(dp), PGSIZE_2MB);
	ret = ftruncate(dp->memfd, dp->region.len);
	if (ret) {
		log_err("failed to ftruncate");
		return -errno;
	}

	dp->region.base = mmap(NULL, dp->region.len, PROT_READ | PROT_WRITE,
	                       MAP_SHARED, dp->memfd, 0);
	if (dp->region.base == MAP_FAILED) {
		log_err("bad mmap");
		return -errno;
	}

	dp->max_doorbells = ctx_max_doorbells(dp);
	dp->doorbells_allocated = 0;
	dp->region_allocated = dp->max_doorbells * CACHE_LINE_SIZE;

	dp->mem_reg = mlx5dv_devx_umem_reg(vfcontext, dp->region.base,
	                                   dp->region.len, IBV_ACCESS_LOCAL_WRITE);
	if (!dp->mem_reg) {
		log_err("register mem failed");
		return -1;
	}

	return 0;
}

static void free_qp(struct qp *qp)
{
	int ret;

	if (qp->rx_wq.obj) {
		ret = mlx5dv_devx_obj_destroy(qp->rx_wq.obj);
		if (ret)
			log_warn("failed to destroy rx wq obj");
	}

	if (qp->rx_cq.obj) {
		ret = mlx5dv_devx_obj_destroy(qp->rx_cq.obj);
		if (ret)
			log_warn("failed to destroy rx cq obj");
	}

	if (qp->tx_wq.obj) {
		ret = mlx5dv_devx_obj_destroy(qp->tx_wq.obj);
		if (ret)
			log_warn("failed to destroy tx wq obj");
	}

	if (qp->tx_cq.obj) {
		ret = mlx5dv_devx_obj_destroy(qp->tx_cq.obj);
		if (ret)
			log_warn("failed to destroy tx cq obj");
	}

}

static void free_ctx(struct directpath_ctx *ctx)
{
	int i, ret;

	if (ctx->mreg) {
		ret = ibv_dereg_mr(ctx->mreg);
		if (ret)
			log_warn("couldn't free MR");
	}

	directpath_steering_teardown(ctx);

	if (ctx->tir_obj) {
		ret = mlx5dv_devx_obj_destroy(ctx->tir_obj);
		if (ret)
			log_warn("failed to destroy tir obj");
	}

	if (ctx->rqt_obj) {
		ret = mlx5dv_devx_obj_destroy(ctx->rqt_obj);
		if (ret)
			log_warn("failed to destroy rqt obj");
	}

	for (i = 0; i < ctx->nr_qs; i++)
		free_qp(&ctx->qps[i]);

	if (ctx->rmp.obj) {
		ret = mlx5dv_devx_obj_destroy(ctx->rmp.obj);
		if (ret)
			log_warn("failed to destroy rmp wq obj");
	}

	if (ctx->tis_obj) {
		ret = mlx5dv_devx_obj_destroy(ctx->tis_obj);
		if (ret)
			log_warn("failed to destroy tis obj");
	}

	if (ctx->td_obj) {
		ret = mlx5dv_devx_obj_destroy(ctx->td_obj);
		if (ret)
			log_warn("failed to destroy td obj");
	}

	if (ctx->mem_reg) {
		if (mlx5dv_devx_umem_dereg(ctx->mem_reg))
			log_warn("failed to unreg mem");
	}

	if (ctx->region.base != MAP_FAILED) {
		if (munmap(ctx->region.base, ctx->region.len))
			log_warn("failed to munmap region: %d", errno);
	}

	if (ctx->memfd >= 0)
		close(ctx->memfd);

	if (ctx->uarns) {
		for (i = 0; i < ctx->nr_alloc_uarn; i++)
			dealloc_uar(ctx->uarns[i]);
		free(ctx->uarns);
	}

	if (ctx->pd)
		ibv_dealloc_pd(ctx->pd);

	if (ctx->p)
		ctx->p->directpath_data = 0;

	free(ctx->rqns);
	free(ctx);
}

int directpath_get_clock(unsigned int *frequency_khz, void **core_clock)
{
	return mlx5_vfio_get_clock(vfcontext, frequency_khz, core_clock);
}

void directpath_get_spec(struct directpath_ctx *dp, struct directpath_spec *spec_out, int *memfd_out, int *barfd_out)
{
	size_t buf_sz;
	void *buf;
	int i;

	if (dp->use_rmp)
		wq_fill_spec(&dp->region, &dp->rmp, &spec_out->rmp);
	else
		spec_out->rmp.nr_entries = 0;

	for (i = 0; i < dp->nr_qs; i++)
		qp_fill_spec(dp, &dp->qps[i], &spec_out->qs[i]);

	buf = dp->region.base + align_up(dp->region_allocated, PGSIZE_2MB);
	spec_out->rx_buf_region_size = ctx_rx_buffer_region(dp);
	spec_out->tx_buf_region_size = ctx_tx_buffer_region(dp);

	buf_sz = spec_out->rx_buf_region_size + spec_out->tx_buf_region_size;
	spec_out->buf_region = ptr_to_shmptr(&dp->region, buf, buf_sz);

	*memfd_out = dp->memfd;
	*barfd_out = bar_fd;

	spec_out->memfd_region_size = dp->region.len;
	spec_out->mr = dp->mreg->lkey;
	spec_out->va_base = (uintptr_t)dp->region.base;
	spec_out->offs = bar_offs;
	spec_out->bar_map_size = bar_map_size;
}


int alloc_raw_ctx(unsigned int nrqs, bool use_rmp, struct directpath_ctx **dp_out, bool is_admin)
{
	int ret = 0;
	uint32_t i;
	uint32_t tisn, tdn;
	size_t lg_rq_sz;
	struct directpath_ctx *dp;
	struct mlx5dv_pd pd_out;
	struct mlx5dv_obj init_obj;

	BUG_ON(!vfcontext);

	dp = malloc(sizeof(*dp) + nrqs * sizeof(struct qp));
	if (!dp)
		return -ENOMEM;

	memset(dp, 0, sizeof(*dp) + nrqs * sizeof(struct qp));
	dp->memfd = -1;
	dp->command_slot = COMMAND_SLOT_UNALLOCATED;
	dp->region.base = MAP_FAILED;
	dp->nr_qs = nrqs;
	dp->ft_idx = -1;
	dp->use_rmp = use_rmp;
	dp->rqns = malloc(sizeof(*dp->rqns) * nrqs);
	if (unlikely(!dp->rqns))
		goto err;

	dp->pd = ibv_alloc_pd(vfcontext);
	if (!dp->pd)
		goto err;

	init_obj.pd.in = dp->pd;
	init_obj.pd.out = &pd_out;
	ret = mlx5dv_init_obj(&init_obj, MLX5DV_OBJ_PD);
	if (ret) {
		log_err("failed to get pdn");
		goto err;
	}
	dp->pdn = pd_out.pdn;

	ret = allocate_user_memory(dp);
	if (ret)
		goto err;

	ret = alloc_td(dp, &tdn);
	if (ret)
		goto err;

	ret = create_tis(dp, tdn, &tisn);
	if (ret)
		goto err;

	if (!is_admin) {
		dp->uarns = malloc(sizeof(*dp->uarns) * dp->nr_qs);
		if (!dp->uarns)
			goto err;

		/* For now allocate 1 UAR, may need more for scalability reasons ? */
		for (i = 0; i < 1; i++) {
			dp->uarns[i] = alloc_uar();
			if (dp->uarns[i] == -1)
				goto err;
			dp->nr_alloc_uarn++;
		}
	}

	if (use_rmp) {
		ret = create_rmp(dp, &dp->rmp, __builtin_ctz(DIRECTPATH_STRIDE_RQ_NUM_DESC), &dp->rmpn);
		if (unlikely(ret)) {
			log_err("failed to create rmp!");
			goto err;
		}

		lg_rq_sz = __builtin_ctzl(DIRECTPATH_TOTAL_RX_EL);
	} else {
		lg_rq_sz = DEFAULT_RQ_LOG_SZ;
	}

	for (i = 0; i < dp->nr_qs; i++) {

		/* round robin assign every pair of SQs to a UARN */
		if (!is_admin)
			dp->qps[i].uarn = dp->uarns[(i / 2) % dp->nr_alloc_uarn];
		else
			dp->qps[i].uarn = admin_uar->page_id;
		dp->qps[i].uar_offset = i % 2 == 0 ? 0x800 : 0xA00;

		ret = create_cq(dp, &dp->qps[i].rx_cq, lg_rq_sz, true, i);
		if (ret)
			goto err;

		ret = create_cq(dp, &dp->qps[i].tx_cq, DEFAULT_SQ_LOG_SZ, false, i);
		if (ret)
			goto err;

		ret = create_rq(dp, &dp->qps[i], lg_rq_sz, i);
		if (ret)
			goto err;

		ret = create_sq(dp, &dp->qps[i], DEFAULT_SQ_LOG_SZ, tisn);
		if (ret)
			goto err;
	}

	ret = create_rqt(dp);
	if (ret)
		goto err;

	ret = create_tir(dp, tdn);
	if (ret)
		goto err;

	dp->mreg = ibv_reg_mr(dp->pd, dp->region.base, dp->region.len,
	                      IBV_ACCESS_LOCAL_WRITE);
	if (!dp->mreg) {
		log_err("failed to create mr");
		goto err;
	}

	*dp_out = dp;

	return 0;

err:
	log_err("err in allocating directpath ctx (%d)", ret);
	free_ctx(dp);
	return ret ? ret : -1;
}

void release_directpath_ctx(struct proc *p)
{
	if (p->directpath_data)
		free_ctx((struct directpath_ctx *)p->directpath_data);
}

void directpath_preallocate(bool use_rmp, unsigned int nrqs, unsigned int cnt)
{
	int ret;
	unsigned int i;

	for (i = 0; i < cnt; i++) {
		if (nr_prealloc == MAX_PREALLOC)
			break;

		ret = alloc_raw_ctx(nrqs, use_rmp, &preallocated_ctx[nr_prealloc], false);
		if (ret)
			break;

		nr_prealloc++;
	}
}

int alloc_directpath_ctx(struct proc *p, bool use_rmp,
                         struct directpath_spec *spec_out, int *memfd_out,
                         int *barfd_out)
{
	int ret;
	unsigned int i;
	struct directpath_ctx *dp = NULL;

	for (i = 0; i < nr_prealloc; i++) {
		if (preallocated_ctx[i]->nr_qs == p->thread_count &&
		    preallocated_ctx[i]->use_rmp == use_rmp) {
			dp = preallocated_ctx[i];
			preallocated_ctx[i] = preallocated_ctx[--nr_prealloc];
			break;
		}
	}

	if (!dp) {
		log_warn_ratelimited("allocating fresh context on startup path");
		ret = alloc_raw_ctx(p->thread_count, use_rmp, &dp, false);
		if (unlikely(ret))
			return ret;
	}

	dp->p = p;
	p->directpath_data = (unsigned long)dp;

	/* fill info for iokernel polling */
	for (i = 0; i < p->thread_count; i++)
		qp_fill_iokspec(&p->threads[i], &dp->qps[i]);

	ret = directpath_steering_attach(dp);
	if (ret)
		return ret;

	directpath_get_spec(dp, spec_out, memfd_out, barfd_out);
	return 0;
}


int directpath_init(void)
{
	int ret;
	pthread_t temp_poll_thread;
	struct mlx5dv_vfio_context_attr vfattr;
	struct ibv_device **dev_list;
	volatile bool poll_thread_stop = false;

	if (!cfg.vfio_directpath)
		return 0;

	if (!nic_pci_addr_str) {
		log_err("please supply the pci address for the nic");
		return -EINVAL;
	}

	memset(&vfattr, 0, sizeof(vfattr));
	vfattr.pci_name = nic_pci_addr_str;
	dev_list = mlx5dv_get_vfio_device_list(&vfattr);
	if (!dev_list) {
		log_err("could not find matching vfio device for pci address %s", nic_pci_addr_str);
		return -1;
	}

	vfcontext = ibv_open_device(dev_list[0]);
	if (!vfcontext) {
		log_err("enable to initialize vfio context: %d", errno);
		return errno ? -errno : -1;
	}

	admin_uar = mlx5dv_devx_alloc_uar(vfcontext, MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC);
	if (!admin_uar) {
		log_err("failed to alloc UAR");
		return -1;
	}

	ret = directpath_commands_init();
	if (ret)
		return ret;

	ret = directpath_setup_port();
	if (ret) {
		log_err("failed to setup port");
		return ret;
	}

	ret = directpath_setup_steering();
	if (ret) {
		log_err("failed to setup direct steering");
		return ret;
	}

	/* after this point, events are routed to the dataplane eq */
	ret = events_init();
	if (ret)
		return ret;

	/* spawn a temporary thread to poll the eq so we can still issue commands */
	ret = pthread_create(&temp_poll_thread, NULL, directpath_init_thread_poll,
		                 (void *)&poll_thread_stop);
	if (ret)
		return ret;

	ret = directpath_arp_server_init();
	if (ret)
		return ret;

	if (nr_vfio_prealloc) {
		directpath_preallocate(vfio_prealloc_rmp, vfio_prealloc_nrqs, nr_vfio_prealloc);
		log_info("control: preallocated %u %u-thread directpath contexts", nr_vfio_prealloc, vfio_prealloc_nrqs);
	}

	poll_thread_stop = true;
	pthread_join(temp_poll_thread, NULL);

	export_fd(vfcontext, &bar_fd, &bar_offs, &bar_map_size);

	return 0;
}

#else

int directpath_init(void)
{
	return 0;
}

#endif

