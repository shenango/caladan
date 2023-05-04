/*
 * arp_fwd.c - ARP forwarder that routes ARP packets to/from runtimes by
 * encapsulating them in IPv4 packets
 */

#ifdef DIRECTPATH

#include <rte_hash.h>
#include <util/mmio.h>
#include <util/udma_barrier.h>

#undef LIST_HEAD

#include <asm/chksum.h>
#include <net/arp.h>
#include <net/ethernet.h>
#include <net/mbuf.h>

#include "../defs.h"

#include "defs.h"
#include "mlx5_ifc.h"

#define BUFSIZE 512
#define BUF_RESERVE_SZ 64
#define ARP_POLL_BATCH 8

#define MLX5_BF_SIZE 256
#define MLX5_ETH_L2_INLINE_HEADER_SIZE 18

static struct directpath_ctx *arp_ctx;
static struct mlx5dv_devx_obj *arp_flow_rule;
static void **rx_buffers;
static void **tx_buffers;
static void *free_bufs;
static uint32_t rx_wq_head;
static uint32_t tx_wq_head;
static uint32_t rx_cq_head;
static uint32_t tx_cq_head;

static void buffer_free_list_push(void *buf)
{
	*(void **)buf = free_bufs;
	free_bufs = buf;
}

static void *buffer_free_list_pop(void)
{
	void *out = free_bufs;
	if (out)
		free_bufs = *(void **)out;
	return out;
}

static int arp_setup_steering(void)
{
	uint8_t *in_dests;
	uint32_t in[DEVX_ST_SZ_DW(set_fte_in) + DEVX_ST_SZ_DW(dest_format)] = {};
	uint32_t out[DEVX_ST_SZ_DW(set_fte_out)];
	void *in_flow_context;

	/* setup arp flow rule */
	DEVX_SET(set_fte_in, in, opcode, MLX5_CMD_OP_SET_FLOW_TABLE_ENTRY);
	DEVX_SET(set_fte_in, in, table_type, FLOW_TBL_TYPE);
	DEVX_SET(set_fte_in, in, table_id, mlx5_devx_get_obj_id(root_flow_tbl));
	DEVX_SET(set_fte_in, in, flow_index, 1);

	in_flow_context = DEVX_ADDR_OF(set_fte_in, in, flow_context);
	DEVX_SET(flow_context, in_flow_context, group_id,
		mlx5_devx_get_obj_id(root_flow_group));
	DEVX_SET(flow_context, in_flow_context, action,
		MLX5_FLOW_CONTEXT_ACTION_FWD_DEST);
	DEVX_SET(flow_context, in_flow_context, match_value.outer_headers.ethertype,
		ETHTYPE_ARP);
	DEVX_SET(flow_context, in_flow_context, destination_list_size, 1);
	in_dests = DEVX_ADDR_OF(flow_context, in_flow_context, destination);
	DEVX_SET(dest_format, in_dests, destination_type, MLX5_FLOW_DEST_TYPE_TIR);
	DEVX_SET(dest_format, in_dests, destination_id,
		mlx5_devx_get_obj_id(arp_ctx->tir_obj));

	arp_flow_rule = mlx5dv_devx_obj_create(vfcontext, in, sizeof(in), out,
		sizeof(out));
	if (!arp_flow_rule) {
		LOG_CMD_FAIL("arp flow rule", set_fte_out, out);
		return -1;
	}

	return 0;
}

static void set_cqe_owners(struct cq *cq)
{
	uint32_t i;

	for (i = 0; i < cq->cqe_cnt; i++)
		mlx5dv_set_cqe_owner(&cq->buf[i], 1);
}

static void flush_txq(void)
{
	struct wq *tx_wq;
	static uint32_t tx_wq_head_last_flush;
	static uint32_t bf_offset;
	void *ctrl;
	uint32_t idx;

	if (tx_wq_head_last_flush == tx_wq_head)
		return;

	tx_wq = &arp_ctx->qps[0].tx_wq;
	idx = (tx_wq_head - 1) & (tx_wq->wqe_cnt - 1);
	ctrl = tx_wq->buf + idx * tx_wq->stride;

	udma_to_device_barrier();
	tx_wq->dbrec[MLX5_SND_DBR] = htobe32(tx_wq_head & 0xffff);

	/* ring bf doorbell */
	barrier();
	mmio_write64_be(admin_uar->reg_addr + bf_offset, *(__be64 *)ctrl);
	barrier();

	bf_offset ^= MLX5_BF_SIZE;
	tx_wq_head_last_flush = tx_wq_head;
}

static int arp_setup_queues(void)
{
	struct mlx5_wqe_data_seg *seg;
	struct wq *rx_wq = &arp_ctx->qps[0].rx_wq;
	unsigned int i;
	void *next_buf;

	set_cqe_owners(&arp_ctx->qps[0].rx_cq);
	set_cqe_owners(&arp_ctx->qps[0].tx_cq);

	rx_buffers = malloc(sizeof(*rx_buffers) * rx_wq->wqe_cnt);
	if (!rx_buffers)
		return -ENOMEM;

	next_buf = arp_ctx->region.base;
	next_buf += align_up(arp_ctx->region_allocated, PGSIZE_2MB);

	for (i = 0; i < rx_wq->wqe_cnt; i++) {
		seg = rx_wq->buf + i * rx_wq->stride;
		seg->byte_count = htobe32(BUFSIZE - BUF_RESERVE_SZ);
		seg->lkey = htobe32(arp_ctx->mreg->lkey);
		seg->addr = htobe64((uint64_t)next_buf + BUF_RESERVE_SZ);
		rx_buffers[i] = next_buf;
		next_buf += BUFSIZE;
	}

	rx_wq_head = rx_wq->wqe_cnt;
	udma_to_device_barrier();
	rx_wq->dbrec[0] = htobe32(rx_wq_head & 0xffff);

	tx_buffers = malloc(sizeof(*tx_buffers) * arp_ctx->qps[0].tx_wq.wqe_cnt);
	if (!tx_buffers)
		return -ENOMEM;

	// allocate additional free buffers (enough to fill tx queue)
	for (i = 0; i < arp_ctx->qps[0].tx_wq.wqe_cnt; i++) {
		buffer_free_list_push(next_buf);
		next_buf += BUFSIZE;
	}

	return 0;
}

static bool handle_arp_packet(struct mbuf *m)
{
	int ret;
	uint32_t target_ip;
	struct arp_hdr *arp_hdr;
	struct arp_hdr_ethip *arp_hdr_ethip;
	struct ip_hdr *iphdr;
	struct eth_hdr *llhdr, *eth_hdr_outer;
	struct proc *p;

	llhdr = mbuf_pull_hdr_or_null(m, *llhdr);
	arp_hdr = mbuf_pull_hdr_or_null(m, *arp_hdr);
	arp_hdr_ethip = mbuf_pull_hdr_or_null(m, *arp_hdr_ethip);

	if (unlikely(!llhdr || !arp_hdr || !arp_hdr_ethip))
		return false;

	if (unlikely(ntoh16(llhdr->type) != ETHTYPE_ARP))
		return false;

	if (unlikely(ntoh16(arp_hdr->htype) != ARP_HTYPE_ETHER ||
	    ntoh16(arp_hdr->ptype) != ETHTYPE_IP ||
	    arp_hdr->hlen != sizeof(struct eth_addr) ||
	    arp_hdr->plen != sizeof(uint32_t)))
		return false;

	target_ip = ntoh32(arp_hdr_ethip->target_ip);
	ret = rte_hash_lookup_data(dp.ip_to_proc, &target_ip, (void **)&p);
	if (ret == -ENOENT)
		return false;

	/* put back the original ARP message */
	mbuf_push_hdr(m, *arp_hdr_ethip);
	mbuf_push_hdr(m, *arp_hdr);

	/* populate IP header */
	iphdr = mbuf_push_hdr(m, *iphdr);
	iphdr->version = IPVERSION;
	iphdr->header_len = 5;
	iphdr->tos = IPTOS_DSCP_CS0 | IPTOS_ECN_NOTECT;
	iphdr->len = hton16(mbuf_length(m));
	iphdr->id = 0; /* see RFC 6864 */
	iphdr->off = hton16(IP_DF);
	iphdr->ttl = 64;
	iphdr->proto = IPPROTO_DIRECTPATH_ARP_ENCAP;
	iphdr->chksum = 0;
	iphdr->saddr = hton32(target_ip);
	iphdr->daddr = hton32(target_ip);

	/* populate ethernet header */
	eth_hdr_outer = mbuf_push_hdr(m, *eth_hdr_outer);
	eth_hdr_outer->shost = llhdr->shost;
	eth_hdr_outer->dhost = iok_info->host_mac;
	eth_hdr_outer->type = hton16(ETHTYPE_IP);

	return true;
}

static void arp_send_pkt(struct mbuf *m)
{
	int size;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_eth_seg *eseg;
	struct mlx5_wqe_data_seg *dpseg;
	struct wq *tx_wq = &arp_ctx->qps[0].tx_wq;
	void *segment;

	void *buf = mbuf_data(m);
	size_t buflen = mbuf_length(m);

	uint16_t inln_sz = MIN(buflen, MLX5_ETH_L2_INLINE_HEADER_SIZE);
	uint32_t idx;

	idx = tx_wq_head & (tx_wq->wqe_cnt - 1);
	segment = tx_wq->buf + idx * tx_wq->stride;
	ctrl = segment;
	eseg = segment + sizeof(*ctrl);
	dpseg = (void *)eseg + ((offsetof(struct mlx5_wqe_eth_seg, inline_hdr) +
	                         MLX5_ETH_L2_INLINE_HEADER_SIZE) & ~0xf);

	size = (sizeof(*ctrl) / 16) +
	       ((offsetof(struct mlx5_wqe_eth_seg, inline_hdr)) +
	         MLX5_ETH_L2_INLINE_HEADER_SIZE) / 16 +
	       sizeof(struct mlx5_wqe_data_seg) / 16;

	/* set ctrl segment */
	*(uint32_t *)(segment + 8) = 0;
	ctrl->imm = 0;
	ctrl->fm_ce_se = MLX5_WQE_CTRL_CQ_UPDATE;
	ctrl->qpn_ds = htobe32(size | (arp_ctx->qps[0].sqn << 8));
	ctrl->opmod_idx_opcode = htobe32(((tx_wq_head & 0xffff) << 8) |
					       MLX5_OPCODE_SEND);

	/* set eseg */
	memset(eseg, 0, sizeof(struct mlx5_wqe_eth_seg));
	eseg->cs_flags |= MLX5_ETH_WQE_L3_CSUM;
	eseg->inline_hdr_sz = htobe16(inln_sz);
	memcpy(eseg->inline_hdr_start, buf, inln_sz);

	/* set dpseg */
	dpseg->lkey = htobe32(arp_ctx->mreg->lkey);
	dpseg->byte_count = htobe32(buflen - inln_sz);
	dpseg->addr = htobe64((uint64_t)buf + inln_sz);

	tx_buffers[idx] = m->head;
	tx_wq_head++;
}

bool directpath_arp_poll(void)
{
	bool did_work = false;
	unsigned int i, budget;
	struct qp *qp = &arp_ctx->qps[0];
	struct mbuf m;
	struct mlx5_cqe64 *cqe;
	struct mlx5_wqe_data_seg *seg;
	uint16_t wqe_idx;
	void *buf;

	/* poll TX completions */
	budget = ARP_POLL_BATCH;
	for (i = 0; i < budget; i++) {
		cqe = get_cqe(&qp->tx_cq, tx_cq_head);
		if (!cqe)
			break;

		tx_cq_head++;
		wqe_idx = be16toh(cqe->wqe_counter) & (qp->tx_wq.wqe_cnt - 1);
		buffer_free_list_push(tx_buffers[wqe_idx]);
	}

	if (i > 0) {
		did_work = true;
		qp->tx_cq.dbrec[0] = htobe32(tx_cq_head & 0xffffff);
	}

	/* poll RX completions, but not more than free TX descriptor slots */
	budget = MIN(ARP_POLL_BATCH, qp->tx_wq.wqe_cnt - (tx_wq_head - tx_cq_head));
	for (i = 0; i < budget; i++) {
		cqe = get_cqe(&qp->rx_cq, rx_cq_head);
		if (!cqe)
			break;

		wqe_idx = be16toh(cqe->wqe_counter) & (qp->rx_wq.wqe_cnt - 1);
		buf = rx_buffers[wqe_idx];
		rx_cq_head++;

		mbuf_init(&m, buf, BUFSIZE, BUF_RESERVE_SZ);
		m.len = be32toh(cqe->byte_cnt);

		bool do_send = handle_arp_packet(&m);
		if (do_send) {
			arp_send_pkt(&m);
			buf = buffer_free_list_pop();
			BUG_ON(!buf);
		}

		seg = qp->rx_wq.buf + wqe_idx * qp->rx_wq.stride;
		seg->addr = htobe64((unsigned long)buf + BUF_RESERVE_SZ);
		rx_buffers[wqe_idx] = buf;
		rx_wq_head++;
	}

	if (i > 0) {
		/* send any packets */
		flush_txq();

		udma_to_device_barrier();
		qp->rx_wq.dbrec[0] = htobe32(rx_wq_head & 0xffff);
		qp->rx_cq.dbrec[0] = htobe32(rx_cq_head & 0xffffff);
		did_work = true;
	}


	return did_work;
}

int directpath_arp_server_init(void)
{
	int ret;

	ret = alloc_raw_ctx(1, false, &arp_ctx, true);
	if (ret)
		return ret;

	ret = arp_setup_steering();
	if (ret)
		return ret;

	ret = arp_setup_queues();
	if (ret)
		return ret;

	return 0;
}

#endif