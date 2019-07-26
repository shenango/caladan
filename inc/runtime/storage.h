/*
 * storage.h - Storage
 */

#pragma once

#include <base/stddef.h>

extern int storage_write(const void *payload, uint64_t lba, uint32_t lba_count);
extern int storage_read(void *dest, uint64_t lba, uint32_t lba_count);
extern uint32_t storage_block_size(void);
extern uint64_t storage_num_blocks(void);