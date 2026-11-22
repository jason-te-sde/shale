#ifndef SHALE_TESTKIT_ALLOC_COUNTER_H_
#define SHALE_TESTKIT_ALLOC_COUNTER_H_

// Counts the calls to operator new that the current thread makes, for tests of how many
// allocations an operation costs. alloc_counter.cc replaces every replaceable global operator new
// and operator delete, so it is compiled into the one test binary that counts, and is not part of
// shale_testkit: no other binary has a replaced operator new.

#include <cstdint>

namespace shale {

/// Counts the operator new calls (plain, array, aligned and nothrow) that the thread which made it
/// performs while it lives. Allocations of other threads are not counted. Counters do not nest: a
/// second one made on a thread that has one alive aborts.
class AllocationCounter {
 public:
  /// Starts counting on the calling thread, from zero.
  AllocationCounter();

  /// Stops counting.
  ~AllocationCounter();

  AllocationCounter(const AllocationCounter&) = delete;
  AllocationCounter& operator=(const AllocationCounter&) = delete;

  /// Returns the calls counted so far.
  uint64_t count() const;
};

}  // namespace shale

#endif  // SHALE_TESTKIT_ALLOC_COUNTER_H_
