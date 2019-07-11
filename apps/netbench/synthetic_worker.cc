// synthetic_worker.cc - support for generation of synthetic work

extern "C" {
#include <string.h>
#include <sys/mman.h>
#include <base/mem.h>
#include <base/stddef.h>
#undef min
#undef max
}

#include <cmath>
#include <algorithm>
#include <numeric>
#include <tuple>
#include <limits>
#include <random>

#include "synthetic_worker.h"

namespace {

void *memcpy_ermsb(void *dst, const void *src, size_t n) {
  asm volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) :: "memory");
  return dst;
}

std::vector<std::string> split(const std::string &text, char sep) {
  std::vector<std::string> tokens;
  std::string::size_type start = 0, end = 0;
  while ((end = text.find(sep, start)) != std::string::npos) {
    tokens.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  tokens.push_back(text.substr(start));
  return tokens;
}

} // anonymous namespace

void SqrtWorker::Work(uint64_t n) {
  constexpr double kNumber = 2350845.545;
  for (uint64_t i = 0; i < n; ++i) {
    volatile double v = sqrt(i * kNumber);
    std::ignore = v; // silences compiler warning
  }
}

StridedMemtouchWorker *
StridedMemtouchWorker::Create(std::size_t size, std::size_t stride) {
  char *buf = new char[size]();
  return new StridedMemtouchWorker(buf, size, stride);
}

void StridedMemtouchWorker::Work(uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) {
    volatile char c = buf_[(stride_ * i) % size_];
    std::ignore = c; // silences compiler warning
  }
}

MemStreamWorker *
MemStreamWorker::Create(std::size_t size) {
  void *addr;
  int prot, flags;

  prot = PROT_READ | PROT_WRITE;
  flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_HUGETLB |
     (PGSHIFT_2MB << MAP_HUGE_SHIFT);

  addr = mmap(NULL, align_up(size, PGSIZE_2MB), prot, flags, -1, 0);
  if (addr == MAP_FAILED)
    return nullptr;

  memset(addr, 0xAB, size);
  return new MemStreamWorker(static_cast<char*>(addr), size);
}

MemStreamWorker::~MemStreamWorker() {
  munmap((void*)buf_, align_up(size_, PGSIZE_2MB));
}

void MemStreamWorker::Work(uint64_t n) {
  if (n > size_) n = size_;
  for (uint64_t i = 0; i < n; ++i) {
    volatile char c = buf_[i];
    std::ignore = c; // silences compiler warning
  }
}

RandomMemtouchWorker *
RandomMemtouchWorker::Create(std::size_t size, unsigned int seed) {
  char *buf = new char[size]();
  std::vector<unsigned int> v(size);
  std::iota(std::begin(v), std::end(v), 0);
  std::mt19937 g(seed);
  std::shuffle(v.begin(), v.end(), g);
  return new RandomMemtouchWorker(buf, std::move(v));
}

void RandomMemtouchWorker::Work(uint64_t n) {
  for (uint64_t i = 0; i < n; ++i)
    buf_[schedule_[i % schedule_.size()]]++;
}

CacheAntagonistWorker *
CacheAntagonistWorker::Create(std::size_t size) {
  char *buf = new char[size]();
  return new CacheAntagonistWorker(buf, size);
}

void CacheAntagonistWorker::Work(uint64_t n) {
  for (uint64_t i = 0; i < n; ++i)
    memcpy_ermsb(&buf_[0], &buf_[size_ / 2], size_ / 2);
}

SyntheticWorker *SyntheticWorkerFactory(std::string s) {
  std::vector<std::string> tokens = split(s, ':');

  // the first token is the type of worker, must be specified
  if (tokens.size() < 1) return nullptr;

  if (tokens[0] == "sqrt") {
    if (tokens.size() != 1) return nullptr;
    return new SqrtWorker();
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
  }

  // invalid type of worker
  return nullptr;
}
