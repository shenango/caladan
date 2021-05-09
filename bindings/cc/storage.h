// storage.h - support for flash storage

#pragma once

extern "C" {
#include <runtime/storage.h>
}

// TODO: this should be per-device.
class Storage {
 public:
  // Write contiguous storage blocks.
  static int Write(const void *src, uint64_t lba, uint32_t lba_count) {
    return storage_write(src, lba, lba_count);
  }

  // Read contiguous storage blocks.
  static int Read(void *dst, uint64_t lba, uint32_t lba_count) {
    return storage_read(dst, lba, lba_count);
  }

  // Returns the size of each block.
  static uint32_t get_block_size() { return storage_block_size(); }

  // Returns the capacity of the device in blocks.
  static uint64_t get_num_blocks() { return storage_num_blocks(); }
};
