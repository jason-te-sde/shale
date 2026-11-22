#include "testkit/alloc_counter.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace shale {

namespace {

// Trivially constructed and destroyed, so that they are usable from the first allocation of a
// thread to the last, whatever the order of the other thread-local objects.
thread_local bool t_counting = false;
thread_local uint64_t t_count = 0;

void Count() {
  if (t_counting) {
    ++t_count;
  }
}

void* Allocate(std::size_t size) {
  Count();
  return std::malloc(size == 0 ? 1 : size);
}

void* AllocateAligned(std::size_t size, std::size_t alignment) {
  Count();
  // aligned_alloc wants a size that is a multiple of the alignment.
  const std::size_t rounded = (size + alignment - 1) / alignment * alignment;
  return std::aligned_alloc(alignment, rounded == 0 ? alignment : rounded);
}

void* MustHave(void* p) {
  if (p == nullptr) {
    std::abort();
  }
  return p;
}

}  // namespace

AllocationCounter::AllocationCounter() {
  if (t_counting) {
    std::abort();
  }
  t_count = 0;
  t_counting = true;
}

AllocationCounter::~AllocationCounter() { t_counting = false; }

uint64_t AllocationCounter::count() const { return t_count; }

}  // namespace shale

// The replaceable allocation functions. Everything goes through malloc and free, and the sized and
// unsized forms of delete are the same.

void* operator new(std::size_t size) { return shale::MustHave(shale::Allocate(size)); }
void* operator new[](std::size_t size) { return shale::MustHave(shale::Allocate(size)); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return shale::MustHave(shale::AllocateAligned(size, static_cast<std::size_t>(alignment)));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return shale::MustHave(shale::AllocateAligned(size, static_cast<std::size_t>(alignment)));
}
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return shale::Allocate(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return shale::Allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t& /*tag*/) noexcept {
  return shale::AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& /*tag*/) noexcept {
  return shale::AllocateAligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t /*size*/) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t /*size*/) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t /*alignment*/) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t /*alignment*/) noexcept { std::free(p); }
void operator delete(void* p, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
  std::free(p);
}
void operator delete(void* p, const std::nothrow_t& /*tag*/) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t& /*tag*/) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t /*alignment*/,
                     const std::nothrow_t& /*tag*/) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::align_val_t /*alignment*/,
                       const std::nothrow_t& /*tag*/) noexcept {
  std::free(p);
}
