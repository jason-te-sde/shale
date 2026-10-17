#ifndef SHALE_UTIL_ARENA_H_
#define SHALE_UTIL_ARENA_H_

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "util/check.h"

namespace shale {

/// A bump allocator: hands out memory from large blocks and frees all of it at once, when the
/// arena is destroyed.
///
/// It exists for the memtable, whose entries all die together. Allocation is a pointer bump, with
/// no per-allocation header and no per-allocation free. Memory is carved from 4096-byte blocks.
/// A request that does not fit in what is left of the current block starts a new block, unless it
/// is larger than a quarter of a block: that one gets a block of exactly its size and the current
/// block stays in use, so that a big request never strands a large tail of the current block.
///
/// Allocate() and AllocateAligned() are not thread-safe: one thread at a time, ordered by whatever
/// lock the owner holds. MemoryUsage() may be called from any thread at any time.
class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  /// Returns `bytes` bytes of uninitialized memory with no alignment guarantee. `bytes` must be
  /// greater than zero. The memory stays valid until the arena is destroyed.
  char* Allocate(size_t bytes) {
    SHALE_CHECK(bytes > 0, "Arena::Allocate needs bytes > 0");
    if (bytes <= alloc_bytes_remaining_) {
      char* result = alloc_ptr_;
      alloc_ptr_ += bytes;
      alloc_bytes_remaining_ -= bytes;
      return result;
    }
    return AllocateFallback(bytes);
  }

  /// Like Allocate(), but the result is aligned to `alignof(std::max_align_t)`, so that any
  /// ordinary object can be constructed in it. `bytes` must be greater than zero.
  char* AllocateAligned(size_t bytes);

  /// Returns the bytes the arena holds from the system: whole blocks plus the bookkeeping for
  /// them, not just the bytes handed out. Safe to call concurrently with an allocation.
  size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }

 private:
  /// The size of a standard block.
  static constexpr size_t kBlockSize = 4096;

  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

}  // namespace shale

#endif  // SHALE_UTIL_ARENA_H_
