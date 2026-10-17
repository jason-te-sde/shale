#include "util/arena.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "util/random.h"

namespace shale {
namespace {

// Returns the index of the first byte of [p, p + n) that differs from `want`, or n if none does.
size_t FirstMismatch(const char* p, size_t n, char want) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] != want) {
      return i;
    }
  }
  return n;
}

TEST(ArenaTest, ManyAllocationsKeepTheirBytes) {
  constexpr int kAllocations = 10000;
  constexpr uint64_t kSeed = 301;
  SCOPED_TRACE("seed " + std::to_string(kSeed));
  Random rnd(kSeed);
  Arena arena;
  std::vector<std::pair<size_t, char*>> allocated;
  allocated.reserve(kAllocations);
  size_t total = 0;
  for (int i = 0; i < kAllocations; i++) {
    size_t size;
    if (i % 1000 == 999) {
      size = 6000;  // wider than a standard block, so only a block of its own can serve it
    } else if (rnd.OneIn(10)) {
      size = rnd.Uniform(10) + 1;
    } else {
      size = rnd.Skewed(12) + 1;
    }
    char* p = arena.Allocate(size);
    for (size_t b = 0; b < size; b++) {
      p[b] = static_cast<char>(i % 256);
    }
    allocated.emplace_back(size, p);
    total += size;
    ASSERT_GE(arena.MemoryUsage(), total) << "after allocation " << i;
  }
  for (size_t i = 0; i < allocated.size(); i++) {
    const auto& [size, p] = allocated[i];
    const char want = static_cast<char>(i % 256);
    ASSERT_EQ(FirstMismatch(p, size, want), size)
        << "allocation " << i << " of " << size << " bytes was overwritten by a later one";
  }
  EXPECT_GE(arena.MemoryUsage(), total);
  EXPECT_LE(arena.MemoryUsage(), total + total / 10 + size_t{64} * 1024);
}

TEST(ArenaTest, AlignedAllocationsAreAligned) {
  constexpr uint64_t kSeed = 17;
  SCOPED_TRACE("seed " + std::to_string(kSeed));
  Random rnd(kSeed);
  Arena arena;
  for (int i = 0; i < 1000; i++) {
    // An unaligned request first, so that the aligned one starts from an odd address.
    arena.Allocate(rnd.Uniform(15) + 1);
    const size_t size = (i % 50 == 49) ? 2000 : rnd.Uniform(100) + 1;
    char* p = arena.AllocateAligned(size);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t), 0u)
        << "allocation " << i << " of " << size << " bytes at " << static_cast<void*>(p);
  }
}

TEST(ArenaTest, AlignedAllocationsDoNotOverlap) {
  Arena arena;
  std::vector<std::pair<size_t, char*>> allocated;
  allocated.reserve(500);
  for (int i = 0; i < 500; i++) {
    const size_t size = static_cast<size_t>(i % 37) + 1;
    char* p = arena.AllocateAligned(size);
    for (size_t b = 0; b < size; b++) {
      p[b] = static_cast<char>(i % 256);
    }
    allocated.emplace_back(size, p);
    *arena.Allocate(1) = 0;  // keeps the cursor misaligned between aligned requests
  }
  for (size_t i = 0; i < allocated.size(); i++) {
    const auto& [size, p] = allocated[i];
    ASSERT_EQ(FirstMismatch(p, size, static_cast<char>(i % 256)), size) << "allocation " << i;
  }
}

TEST(ArenaTest, OversizeRequestsThatDoNotFitGetABlockOfTheirOwn) {
  Arena arena;
  EXPECT_EQ(arena.MemoryUsage(), 0u);

  arena.Allocate(1);  // the first block: 4096 bytes, 4095 of them free
  const size_t one_block = arena.MemoryUsage();
  EXPECT_GE(one_block, 4096u);
  EXPECT_LT(one_block, 4096u + 64u) << "a 4096-byte block plus a little bookkeeping";

  arena.Allocate(3100);  // fits in the block, leaving 995 bytes
  EXPECT_EQ(arena.MemoryUsage(), one_block);

  arena.Allocate(1025);  // does not fit and is over a quarter block: a block of exactly its size
  const size_t with_own_block = arena.MemoryUsage();
  EXPECT_GE(with_own_block, one_block + 1025u);
  EXPECT_LT(with_own_block, one_block + 1025u + 64u);

  arena.Allocate(995);  // the first block, not the oversize one, is still the current block
  EXPECT_EQ(arena.MemoryUsage(), with_own_block);

  arena.Allocate(1024);  // does not fit, but is not over a quarter block: a fresh standard block
  EXPECT_GE(arena.MemoryUsage(), with_own_block + 4096u);
  EXPECT_LT(arena.MemoryUsage(), with_own_block + 4096u + 64u);
}

TEST(ArenaTest, MemoryUsageCanBeReadWhileAnotherThreadAllocates) {
  Arena arena;
  std::atomic<bool> done{false};
  std::atomic<bool> went_backwards{false};
  std::thread reader([&] {
    size_t last = 0;
    while (!done.load()) {
      const size_t now = arena.MemoryUsage();
      if (now < last) {
        went_backwards.store(true);
      }
      last = now;
    }
  });
  for (int i = 0; i < 20000; i++) {
    arena.Allocate(static_cast<size_t>(i % 200) + 1);
  }
  done.store(true);
  reader.join();
  EXPECT_FALSE(went_backwards.load());
  EXPECT_GT(arena.MemoryUsage(), 0u);
}

TEST(ArenaDeathTest, ZeroByteRequestsFail) {
  Arena arena;
  EXPECT_DEATH(arena.Allocate(0), "bytes > 0");
  EXPECT_DEATH(arena.AllocateAligned(0), "bytes > 0");
}

}  // namespace
}  // namespace shale
