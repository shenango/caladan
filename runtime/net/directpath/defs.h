
#pragma once

#include <base/pci.h>
#include <base/tcache.h>
#include <base/thread.h>
#include <iokernel/directpath.h>
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

static inline size_t directpath_get_buf_size(void)
{
	if (cfg_directpath_strided)
		return DIRECTPATH_STRIDE_MODE_BUF_SZ;

	return align_up(net_get_mtu() + RX_BUF_HEAD + RX_BUF_TAIL,
			2 * CACHE_LINE_SIZE);
}

extern struct pci_addr nic_pci_addr;
extern struct mempool directpath_buf_mp;
extern struct tcache *directpath_buf_tcache;
extern DEFINE_PERTHREAD(struct tcache_perthread, directpath_buf_pt);
extern void directpath_rx_completion(struct mbuf *m);

extern int mlx5_init(void);
extern int mlx5_init_thread(void);
