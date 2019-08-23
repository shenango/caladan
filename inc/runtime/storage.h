/*
 * storage.h - Storage
 */

#pragma once

#include <base/stddef.h>

extern int storage_write(const void *payload, uint64_t lba, uint32_t lba_count);
extern int storage_read(void *dest, uint64_t lba, uint32_t lba_count);



/*
 * storage_block_size - get the size of a block from the nvme device
 */
static inline uint32_t storage_block_size(void)
{
	extern uint32_t block_size;
	return block_size;
}

/*
 * storage_num_blocks - gets the number of blocks from the nvme device
 */
static inline uint64_t storage_num_blocks(void)
{
	extern uint64_t num_blocks;
	return num_blocks;
}
