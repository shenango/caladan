
#include <inttypes.h>
#include <rte_dmadev.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>

#include <base/log.h>
#include <iokernel/queue.h>

#include "defs.h"

#define MAX_DMA_DEVICES 4
#define DMA_RING_SIZE 64
#define DMA_RING_MASK (DMA_RING_SIZE - 1)

struct dma_completion_data {
	struct rte_mbuf *src_buf;
	struct rte_mbuf *dst_buf;
	copy_cb_t cb;
};

struct dma_chan {
	int16_t dev_id;
	uint32_t dma_head;
	uint32_t dma_tail;
	uint32_t dma_last_submit_head;
	struct dma_completion_data compl[DMA_RING_SIZE];
};

static struct dma_chan rx_dma_chan[MAX_DMA_DEVICES];
static unsigned int next_enq_pos;
static int dma_devices_active;

unsigned int dma_chan_slots_avail(void)
{
	struct dma_chan *chan = &rx_dma_chan[next_enq_pos];
	return DMA_RING_SIZE - (chan->dma_head - chan->dma_tail);
}

static unsigned int dma_dequeue_chan(struct dma_chan *chan,
	                             unsigned int budget)
{
	bool has_error;
	struct dma_completion_data *compl;
	uint16_t nr_completed;
	unsigned int i;
	struct rte_mbuf *src_bufs[budget];

	if (chan->dma_head == chan->dma_tail)
		return 0;

	nr_completed = rte_dma_completed(chan->dev_id, 0, budget, NULL,
		                         &has_error);
	BUG_ON(has_error);

	if (!nr_completed)
		return 0;

	for (i = 0; i < nr_completed; i++) {
		compl = &chan->compl[chan->dma_tail++ & DMA_RING_MASK];
		src_bufs[i] = compl->src_buf;
		compl->cb(compl->dst_buf);
	}

	if (nr_completed) {
		log_debug("dma: dequeued %u transfers", nr_completed);
		rte_pktmbuf_free_bulk(src_bufs, nr_completed);
	}

	return nr_completed;
}

bool dma_dequeue(void)
{
	unsigned int i, completed = 0;

	if (!dma_devices_active)
		return false;

	for (i = 0; i < dma_devices_active; i++)
		completed += dma_dequeue_chan(&rx_dma_chan[i], IOKERNEL_DMA_BURST_SIZE);
	STAT_INC(DMA_DEQUEUE, completed);
	return completed > 0;
}

static bool dma_flush(void)
{
	int ret;
	struct dma_chan *chan = &rx_dma_chan[next_enq_pos];

	if (chan->dma_head == chan->dma_last_submit_head)
		return false;

	ret = rte_dma_submit(chan->dev_id, 0);
	BUG_ON(ret < 0);

	STAT_INC(DMA_SUBMIT, 1);
	chan->dma_last_submit_head = chan->dma_head;

	if (++next_enq_pos == dma_devices_active)
		next_enq_pos = 0;

	return true;
}

static bool enqueue_rx_dma(struct rte_mbuf *src_buf, struct rte_mbuf *dst_buf,
	                   copy_cb_t cb)
{
	int ret;
	struct dma_chan *chan = &rx_dma_chan[next_enq_pos];
	struct dma_completion_data *compl;

	assert(dma_devices_active);

	if (unlikely(chan->dma_head - chan->dma_tail >= DMA_RING_SIZE)) {
		log_warn_ratelimited("dma channel full!");
		return false;
	}

	ret = rte_dma_copy(chan->dev_id, 0, rte_pktmbuf_iova(src_buf),
		           rte_pktmbuf_iova(dst_buf),
		           rte_pktmbuf_pkt_len(src_buf), 0);
	if (unlikely(ret < 0)) {
		log_warn_ratelimited("unexpected DMA copy error %d", ret);
		return false;
	}

	STAT_INC(DMA_ENQUEUE, 1);
	log_debug("dma: enqueue one");

	compl = &chan->compl[chan->dma_head++ & DMA_RING_MASK];
	compl->src_buf = src_buf;
	compl->dst_buf = dst_buf;
	compl->cb = cb;
	return true;
}

static void copy_batch_dma(struct rte_mbuf **src, struct rte_mbuf **dst, int n,
	                   copy_cb_t cb)
{
	char *buf;
	int i;
	size_t bytes;

	assert(dma_devices_active);

	for (i = 0; i < n; i++) {
		bytes = rte_pktmbuf_pkt_len(src[i]);
		buf = rte_pktmbuf_append(dst[i], bytes);
		dst[i]->ol_flags = src[i]->ol_flags;
		dst[i]->ol_flags |= RTE_MBUF_F_RX_IP_CKSUM_GOOD;
		dst[i]->hash = src[i]->hash;
		bool ret = enqueue_rx_dma(src[i], dst[i], cb);
		if (unlikely(!ret)) {
			memcpy(buf, rte_pktmbuf_mtod(src[i], char *), bytes);
			rte_pktmbuf_free(src[i]);
			cb(dst[i]);
		}
	}

	dma_flush();
}

void copy_batch(struct rte_mbuf **src, struct rte_mbuf **dst, int n,
	        copy_cb_t cb)
{
	char *buf;
	int i;
	size_t bytes;

	if (dma_devices_active) {
		copy_batch_dma(src, dst, n, cb);
		return;
	}

	for (i = 0; i < n; i++) {
		bytes = rte_pktmbuf_pkt_len(src[i]);
		buf = rte_pktmbuf_append(dst[i], bytes);
		dst[i]->ol_flags = src[i]->ol_flags;
		dst[i]->ol_flags |= RTE_MBUF_F_RX_IP_CKSUM_GOOD;
		dst[i]->hash = src[i]->hash;
		memcpy(buf, rte_pktmbuf_mtod(src[i], char *), bytes);
		cb(dst[i]);
	}

	rte_pktmbuf_free_bulk(src, n);
}

static int dma_chan_init(struct dma_chan *chan, int16_t next_dev_id)
{
	uint16_t count, vchan = 0;
	int ret;

	struct rte_dma_info info;
	struct rte_dma_conf dev_config = { .nb_vchans = 1 };
	struct rte_dma_vchan_conf qconf = {
		.direction = RTE_DMA_DIR_MEM_TO_MEM,
		.nb_desc = DMA_RING_SIZE * 2,
	};

	count = rte_dma_count_avail();
	if (count < 1)
		return -1;

	chan->dev_id = rte_dma_next_dev(next_dev_id);
	if ((uint16_t)chan->dev_id == UINT16_MAX || chan->dev_id < next_dev_id)
		return -1;

	ret = rte_dma_configure(chan->dev_id, &dev_config);
	if (ret) {
		log_err("dma: error configuring device");
		return -1;
	}

	ret = rte_dma_vchan_setup(chan->dev_id, vchan, &qconf);
	if (ret) {
		log_err("dma: error configuring vchan");
		return -1;
	}

	rte_dma_info_get(chan->dev_id, &info);
	if (info.nb_vchans != 1) {
		log_err("dma:  no configured queues reported on device");
		return -1;
	}

	ret = rte_dma_start(chan->dev_id);
	if (ret) {
		log_err("dma: failed to start device");
		return -1;
	}

	return 0;
}

int dma_init(void) {
	int i, ret;

	for (i = 0; i < MAX_DMA_DEVICES; i++) {
		ret = dma_chan_init(&rx_dma_chan[i], i);
		if (ret)
			break;
	}

	dma_devices_active = i;
	if (dma_devices_active)
		log_info("dma: copy engine enabled (%d devices)",
			 dma_devices_active);
	else
		log_info("dma: copy engine disabled");

	return 0;
}
