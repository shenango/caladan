// synthetic_worker.h - support for generation of synthetic work

#pragma once

#include <emmintrin.h>
#include <numa.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#define CACHELINE_SIZE (64)

class SyntheticWorker {
 public:
  virtual ~SyntheticWorker() {}
  // Perform n iterations of fake work.
  virtual void Work(uint64_t n) = 0;
};

class SqrtWorker : public SyntheticWorker {
 public:
  SqrtWorker() {}
  ~SqrtWorker() {}

  // Performs n iterations of sqrt().
  void Work(uint64_t n);
};

class AsmSqrtWorker : public SyntheticWorker {
 public:
  AsmSqrtWorker() {}
  ~AsmSqrtWorker() {}

  // Performs n iterations of sqrt().
  void Work(uint64_t n);
};

class StridedMemtouchWorker : public SyntheticWorker {
 public:
  ~StridedMemtouchWorker() { delete buf_; }

  // Creates a strided memory touching worker.
  static StridedMemtouchWorker *Create(std::size_t size, size_t stride);

  // Performs n strided memory touches.
  void Work(uint64_t n);

 private:
  StridedMemtouchWorker(char *buf, std::size_t size, size_t stride)
      : buf_(buf), size_(size), stride_(stride) {}

  volatile char *buf_;
  std::size_t size_;
  std::size_t stride_;
};

class MemStreamWorker : public SyntheticWorker {
 public:
  ~MemStreamWorker();

  // Creates a memory streaming worker.
  static MemStreamWorker *Create(std::size_t size);

  // Performs n memory reads.
  void Work(uint64_t n);

 private:
  MemStreamWorker(char *buf, std::size_t size) : buf_(buf), size_(size) {}

  volatile char *buf_;
  std::size_t size_;
};

class RandomMemtouchWorker : public SyntheticWorker {
 public:
  ~RandomMemtouchWorker() { delete buf_; }

  // Creates a random memory touching worker.
  static RandomMemtouchWorker *Create(std::size_t size, unsigned int seed);

  // Performs n random memory touches.
  void Work(uint64_t n);

 private:
  RandomMemtouchWorker(char *buf, std::vector<unsigned int> schedule)
      : buf_(buf), schedule_(std::move(schedule)) {}

  volatile char *buf_;
  std::vector<unsigned int> schedule_;
};

class CacheAntagonistWorker : public SyntheticWorker {
 public:
  ~CacheAntagonistWorker() { delete buf_; }

  // Creates a cache antagonist worker.
  static CacheAntagonistWorker *Create(std::size_t size);

  // Perform n cache accesses.
  void Work(uint64_t n);

 private:
  CacheAntagonistWorker(char *buf, std::size_t size) : buf_(buf), size_(size) {}

  char *buf_;
  std::size_t size_;
};

class MemBWAntagonistWorker : public SyntheticWorker {
 public:
  ~MemBWAntagonistWorker() { numa_free(buf_, size_); }

  // Creates a memory bandwidth antagonist worker. It allocates an array whose
  // size is indicated by the parameter.
  static MemBWAntagonistWorker *Create(std::size_t size, int nop_period,
				       int nop_num);

  // Perform n times array stores.
  void Work(uint64_t n);

 private:
  MemBWAntagonistWorker(char *buf, std::size_t size, int nop_period, int nop_num) :
    buf_(buf), size_(size), nop_period_(nop_period), nop_num_(nop_num) {}

  char *buf_;
  std::size_t size_;
  int nop_period_;
  int nop_num_;
};

class DynamicCacheAntagonistWorker : public SyntheticWorker {
 public:
  ~DynamicCacheAntagonistWorker() { delete buf_; }

  // Creates a cache antagonist worker.
  static DynamicCacheAntagonistWorker *Create(std::size_t size,
					      int period, int nop_num);

  // Perform n cache accesses.
  void Work(uint64_t n);

 private:
 DynamicCacheAntagonistWorker(char *buf, std::size_t size, int period, int nop_num) :
   buf_(buf), size_(size), period_(period), nop_num_(nop_num) {}

  char *buf_;
  std::size_t size_;
  int period_;
  int nop_num_;
  int cnt_;
};

// Parses a string to generate one of the above fake workers.
SyntheticWorker *SyntheticWorkerFactory(std::string s);
