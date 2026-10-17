#include "util/arena.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace shale {

namespace {

constexpr size_t kAlignment = alignof(std::max_align_t);
static_assert((kAlignment & (kAlignment - 1)) == 0, "alignment must be a power of two");

}  // namespace

char* Arena::AllocateAligned(size_t bytes) {
  SHALE_CHECK(bytes > 0, "Arena::AllocateAligned needs bytes > 0");
  const size_t misalignment = reinterpret_cast<uintptr_t>(alloc_ptr_) & (kAlignment - 1);
  const size_t padding = (misalignment == 0) ? 0 : kAlignment - misalignment;
  if (padding <= alloc_bytes_remaining_ && bytes <= alloc_bytes_remaining_ - padding) {
    char* result = alloc_ptr_ + padding;
    alloc_ptr_ += bytes + padding;
    alloc_bytes_remaining_ -= bytes + padding;
    return result;
  }
  // Every block starts at an address that operator new aligns for any ordinary object, so both
  // the fresh block and the oversize block the fallback returns are already aligned.
  return AllocateFallback(bytes);
}

char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // A request this large would strand up to a quarter of a block in the tail, so it gets a block
    // of its own, and the current block stays available for the small requests that follow.
    return AllocateNewBlock(bytes);
  }
  // The tail of the current block is abandoned. It is smaller than this request, so under a
  // quarter of a block.
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;
  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  // Not make_unique<char[]>, which would zero a block that the caller is about to overwrite.
  blocks_.push_back(std::make_unique_for_overwrite<char[]>(block_bytes));
  char* result = blocks_.back().get();
  // Only this thread writes memory_usage_, so a load and a store is enough; the atomic is there
  // for the other threads that read it.
  memory_usage_.store(
      memory_usage_.load(std::memory_order_relaxed) + block_bytes + sizeof(std::unique_ptr<char[]>),
      std::memory_order_relaxed);
  return result;
}

}  // namespace shale
