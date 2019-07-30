
#pragma once

#include <base/tcache.h>
#include <base/thread.h>
#include <iokernel/queue.h>

#include "../defs.h"


#define RQ_NUM_DESC			128
#define SQ_NUM_DESC			128

#define SQ_CLEAN_THRESH			RUNTIME_SOFTIRQ_LOCAL_BUDGET
#define SQ_CLEAN_MAX			SQ_CLEAN_THRESH

#define RX_BUF_RESERVED \
 (align_up(sizeof(struct mbuf), 2 * CACHE_LINE_SIZE))

extern struct mempool directpath_buf_mp;
extern struct tcache *directpath_buf_tcache;
extern DEFINE_PERTHREAD(struct tcache_perthread, directpath_buf_pt);
extern void directpath_rx_completion(struct mbuf *m);
extern int mlx5_init(struct hardware_q **rxq_out,
	    struct direct_txq **txq_out, unsigned int nr_rxq,
	    unsigned int nr_txq);
