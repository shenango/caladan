// synthetic_worker.cc - support for generation of synthetic work

extern "C" {
#include <base/log.h>
#include <base/mem.h>
#include <base/stddef.h>
#include <string.h>
#include <sys/mman.h>
}

#include "synthetic_worker.h"
#include "util.h"
#include "sync.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <random>
#include <tuple>

extern barrier_t barrier;

namespace {

void *memcpy_ermsb(void *dst, const void *src, size_t n) {
  asm volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n)::"memory");
  return dst;
}

inline void clflush(volatile void *p) { asm volatile("clflush (%0)" ::"r"(p)); }

// Store data (indicated by the param c) to the cache line using the
// non-temporal store.
inline void nt_cacheline_store(char *p, int c) {
  __m128i i = _mm_set_epi8(c, c, c, c, c, c, c, c, c, c, c, c, c, c, c, c);
  _mm_stream_si128((__m128i *)&p[0], i);
  _mm_stream_si128((__m128i *)&p[16], i);
  _mm_stream_si128((__m128i *)&p[32], i);
  _mm_stream_si128((__m128i *)&p[48], i);
}

}  // anonymous namespace

void SqrtWorker::Work(uint64_t n) {
  constexpr double kNumber = 2350845.545;
  for (uint64_t i = 0; i < n; ++i) {
    volatile double v = sqrt(i * kNumber);
    std::ignore = v;  // silences compiler warning
  }
}

#define SQRT(src_var, dest_var, src_xmm, dest_xmm) \
  asm volatile(                                    \
    "movq %1, %%" src_xmm "\n"                     \
    "sqrtsd %%" src_xmm ", %%" dest_xmm "\n"       \
    "movq %%" dest_xmm ", %0 \n"                   \
    : "=r"(dest_var)                               \
    : "g"(src_var)                                 \
    : src_xmm, dest_xmm, "memory")

void AsmSqrtWorker::Work(uint64_t n) {
  constexpr double kNumber = 2350845.545;
  double src_0, src_1, src_2, src_3;
  double dest_0, dest_1, dest_2, dest_3;
  for (uint64_t i = 0; i < n; i += 4) {
    src_0 = i * kNumber;
    src_1 = (i + 1) * kNumber;
    src_2 = (i + 2) * kNumber;
    src_3 = (i + 3) * kNumber;
    SQRT(src_0, dest_0, "xmm0", "xmm1");
    SQRT(src_1, dest_1, "xmm2", "xmm3");
    SQRT(src_2, dest_2, "xmm4", "xmm5");
    SQRT(src_3, dest_3, "xmm6", "xmm7");
  }
}

StridedMemtouchWorker *StridedMemtouchWorker::Create(std::size_t size,
                                                     std::size_t stride) {
  char *buf = new char[size]();
  return new StridedMemtouchWorker(buf, size, stride);
}

void StridedMemtouchWorker::Work(uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) {
    volatile char c = buf_[(stride_ * i) % size_];
    std::ignore = c;  // silences compiler warning
  }
}

MemStreamWorker *MemStreamWorker::Create(std::size_t size) {
  void *addr;
  int prot, flags;

  prot = PROT_READ | PROT_WRITE;
  flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_HUGETLB |
          (PGSHIFT_2MB << MAP_HUGE_SHIFT);

  addr = mmap(NULL, align_up(size, PGSIZE_2MB), prot, flags, -1, 0);
  if (addr == MAP_FAILED) return nullptr;

  memset(addr, 0xAB, size);
  return new MemStreamWorker(static_cast<char *>(addr), size);
}

MemStreamWorker::~MemStreamWorker() {
  munmap((void *)buf_, align_up(size_, PGSIZE_2MB));
}

void MemStreamWorker::Work(uint64_t n) {
  if (n > size_) n = size_;
  for (uint64_t i = 0; i < n; ++i) {
    volatile char c = buf_[i];
    std::ignore = c;  // silences compiler warning
  }
}

RandomMemtouchWorker *RandomMemtouchWorker::Create(std::size_t size,
                                                   unsigned int seed) {
  char *buf = new char[size]();
  std::vector<unsigned int> v(size);
  std::iota(std::begin(v), std::end(v), 0);
  std::mt19937 g(seed);
  std::shuffle(v.begin(), v.end(), g);
  return new RandomMemtouchWorker(buf, std::move(v));
}

void RandomMemtouchWorker::Work(uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) buf_[schedule_[i % schedule_.size()]]++;
}

CacheAntagonistWorker *CacheAntagonistWorker::Create(std::size_t size) {
  char *buf = new char[size]();
  return new CacheAntagonistWorker(buf, size);
}

void CacheAntagonistWorker::Work(uint64_t n) {
  for (uint64_t i = 0; i < n; ++i)
    memcpy_ermsb(&buf_[0], &buf_[size_ / 2], size_ / 2);
}

MemBWAntagonistWorker *MemBWAntagonistWorker::Create(
  std::size_t size, int nop_period, int nop_num) {
  // non-temporal store won't bypass cache when accessing the remote memory.
  char *buf = reinterpret_cast<char *>(numa_alloc_local(size));
  // numa_alloc_* will allocate memory in pages, therefore it must be cacheline
  // aligned.
  if (reinterpret_cast<uint64_t>(buf) % CACHELINE_SIZE != 0) {
    // Should never be executed.
    log_crit("The allocated memory should be cacheline size aligned.");
    return nullptr;
  }
  // Flush the cache explicitly. Non-temporal store will still write into cache
  // if the corresponding data is already at cache.
  for (std::size_t i = 0; i < size; i += CACHELINE_SIZE) {
    clflush(reinterpret_cast<volatile void *>(buf + i));
  }
  return new MemBWAntagonistWorker(buf, size, nop_period, nop_num);
}

void MemBWAntagonistWorker::Work(uint64_t n) {
  int cnt = 0;
  for (uint64_t k = 0; k < n; k++) {
    for (std::size_t i = 0; i < size_; i += CACHELINE_SIZE) {
      nt_cacheline_store(buf_ + i, 0);
      if (cnt++ == nop_period_) {
        cnt = 0;
	for (int j = 0; j < nop_num_; j++) {
  	  asm("");
	}
      }
    }
  }
}

DynamicCacheAntagonistWorker *DynamicCacheAntagonistWorker::Create(
  std::size_t size, int period, int nop_num) {
  char *buf = new char[size]();
  return new DynamicCacheAntagonistWorker(buf, size, period, nop_num);
}

void DynamicCacheAntagonistWorker::Work(uint64_t n) {
  double *ptr = reinterpret_cast<double *>(buf_);
  size_t offset = size_ / 2 / sizeof(double);
  for (uint64_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < offset; j++) {
      ptr[j + offset] = ptr[j];
      if (cnt_++ == period_) {
	barrier_wait(&barrier);
        cnt_ = 0;
	for (int k = 0; k < nop_num_; k++) {
	  asm("");
	}
      }
    }
  }
}

SyntheticWorker *SyntheticWorkerFactory(std::string s) {
  std::vector<std::string> tokens = split(s, ':');

  // the first token is the type of worker, must be specified
  if (tokens.size() < 1) return nullptr;

  if (tokens[0] == "sqrt") {
    if (tokens.size() != 1) return nullptr;
    return new SqrtWorker();
  }   if (tokens[0] == "asmsqrt") {
    if (tokens.size() != 1) return nullptr;
    return new AsmSqrtWorker();
  } else if (tokens[0] == "stridedmem") {
    if (tokens.size() != 3) return nullptr;
    unsigned long size = std::stoul(tokens[1], nullptr, 0);
    unsigned long stride = std::stoul(tokens[2], nullptr, 0);
    return StridedMemtouchWorker::Create(size, stride);
  } else if (tokens[0] == "randmem") {
    if (tokens.size() != 3) return nullptr;
    unsigned long size = std::stoul(tokens[1], nullptr, 0);
    unsigned long seed = std::stoul(tokens[2], nullptr, 0);
    if (seed > std::numeric_limits<unsigned int>::max()) return nullptr;
    return RandomMemtouchWorker::Create(size, seed);
  } else if (tokens[0] == "memstream") {
    if (tokens.size() != 2) return nullptr;
    unsigned long size = std::stoul(tokens[1], nullptr, 0);
    return MemStreamWorker::Create(size);
  } else if (tokens[0] == "cacheantagonist") {
    if (tokens.size() != 2) return nullptr;
    unsigned long size = std::stoul(tokens[1], nullptr, 0);
    return CacheAntagonistWorker::Create(size);
  } else if (tokens[0] == "membwantagonist") {
    if (tokens.size() != 4) return nullptr;
    unsigned long size = std::stoul(tokens[1], nullptr, 0);
    unsigned long nop_period = std::stoul(tokens[2], nullptr, 0);
    unsigned long nop_num = std::stoul(tokens[3], nullptr, 0);
    return MemBWAntagonistWorker::Create(size, nop_period, nop_num);
  } else if (tokens[0] == "dynamiccacheantagonist") {
    if (tokens.size() != 4) return nullptr;
    unsigned long size = std::stoul(tokens[1], nullptr, 0);
    unsigned long period = std::stoul(tokens[2], nullptr, 0);
    unsigned long long nop_num = std::stoul(tokens[3], nullptr, 0);
    return DynamicCacheAntagonistWorker::Create(size, period, nop_num);
  }

  // invalid type of worker
  return nullptr;
}
