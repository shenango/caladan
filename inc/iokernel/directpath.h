/*
 * directpath.h - definitions for directpath structures
 */

#pragma once

#include <sys/types.h>

#include <iokernel/shm.h>

#define DIRECTPATH_STRIDE_RQ_NUM_DESC   128
#define DIRECTPATH_STRIDE_MODE_BUF_SZ   16384
#define DIRECTPATH_STRIDE_SIZE          256

#define DIRECTPATH_NUM_STRIDES \
 (DIRECTPATH_STRIDE_MODE_BUF_SZ / DIRECTPATH_STRIDE_SIZE)

#define DIRECTPATH_STRIDE_SHIFT (__builtin_ctz(DIRECTPATH_NUM_STRIDES))

#define DIRECTPATH_TOTAL_RX_EL \
 (DIRECTPATH_NUM_STRIDES * DIRECTPATH_STRIDE_RQ_NUM_DESC)
#define DIRECTPATH_STRIDE_REFILL_THRESH_HI \
 (DIRECTPATH_TOTAL_RX_EL * 1 / 4)

#define DIRECTPATH_STRIDE_RX_BUF_POOL_SZ \
    (2 * DIRECTPATH_STRIDE_RQ_NUM_DESC * DIRECTPATH_STRIDE_MODE_BUF_SZ)

BUILD_ASSERT(DIRECTPATH_STRIDE_MODE_BUF_SZ % DIRECTPATH_STRIDE_SIZE == 0);
BUILD_ASSERT(PGSIZE_2MB % DIRECTPATH_STRIDE_MODE_BUF_SZ == 0);
BUILD_ASSERT(DIRECTPATH_STRIDE_SIZE >= 64);

struct directpath_ring_q_spec {
    shmptr_t buf;
    shmptr_t dbrec;
    uint64_t nr_entries;
    uint32_t stride;
};

struct directpath_queue_spec {
    uint32_t sqn;
    uint32_t uarn;
    uint32_t uar_offset;
    struct directpath_ring_q_spec rx_wq;
    struct directpath_ring_q_spec rx_cq;
    struct directpath_ring_q_spec tx_wq;
    struct directpath_ring_q_spec tx_cq;
};

struct directpath_spec {
    uint32_t mr;
    size_t va_base;
    size_t memfd_region_size;

    /* bar map */
    off_t offs;
    size_t bar_map_size;

    struct directpath_ring_q_spec rmp;

    shmptr_t buf_region;
    size_t rx_buf_region_size;
    size_t tx_buf_region_size;

    struct directpath_queue_spec qs[];
};
