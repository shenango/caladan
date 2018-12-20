/*
 * storage.h - Storage
 */

#pragma once

#include <base/stddef.h>
#include <runtime/storage.h>

#if __has_include("spdk/nvme.h")

extern int storage_write(const void *payload, int lba, int lba_count);
extern int storage_read(void *dest, int lba, int lba_count);
extern int storage_block_size();
extern int storage_num_blocks();

#else

int storage_write(const void *payload,
		uint64_t lba, uint32_t lba_count)
{
	return -1;
}
int storage_read(void *dest,
		uint64_t lba, uint32_t lba_count)
{
	return -1;
}
int storage_block_size()
{
	return -1;
}
int storage_num_blocks()
{
	return -1;
}

#endif
