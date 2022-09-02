
#pragma once

#include <base/pci.h>
#include <base/tcache.h>
#include <base/thread.h>
#include <iokernel/queue.h>

#include "../defs.h"


#define RQ_NUM_DESC			1024
#define SQ_NUM_DESC			128

#define SQ_CLEAN_THRESH			RUNTIME_RX_BATCH_SIZE
#define SQ_CLEAN_MAX			SQ_CLEAN_THRESH

/* space for the mbuf struct */
#define RX_BUF_HEAD \
 (align_up(sizeof(struct mbuf), 2 * CACHE_LINE_SIZE))
/* some NICs expect enough padding for CRC etc., even if they strip it */
#define RX_BUF_TAIL			64

static inline unsigned int directpath_get_buf_size(void)
{
	return align_up(net_get_mtu() + RX_BUF_HEAD + RX_BUF_TAIL,
			2 * CACHE_LINE_SIZE);
}

extern struct pci_addr nic_pci_addr;
extern bool cfg_pci_addr_specified;

extern struct mempool directpath_buf_mp;
extern struct tcache *directpath_buf_tcache;
extern DEFINE_PERTHREAD(struct tcache_perthread, directpath_buf_pt);
extern void directpath_rx_completion(struct mbuf *m);
extern int mlx5_init_queue_steering(struct hardware_q **rxq_out,
	    struct direct_txq **txq_out, unsigned int nr_rxq,
	    unsigned int nr_txq);
extern int mlx5_init_flow_steering(struct hardware_q **rxq_out,
	    struct direct_txq **txq_out, unsigned int nr_rxq,
	    unsigned int nr_txq);
struct ibv_device;
extern int ibv_device_to_pci_addr(const struct ibv_device *device, struct pci_addr *pci_addr);
