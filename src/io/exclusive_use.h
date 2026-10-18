#ifndef SHALE_IO_EXCLUSIVE_USE_H_
#define SHALE_IO_EXCLUSIVE_USE_H_

#include <atomic>
#include <string>
#include <string_view>

#include "util/check.h"

namespace shale {

/// Detects two threads using the same object at the same time.
///
/// A file handle may pass from one thread to another, but its position and its write buffer are
/// not protected: a second call that overlaps the first corrupts them without any error. The Env
/// documents that rule, and this makes the handle enforce it, because a rule that lives only in a
/// comment is broken by the next caller who has not read it. Each public method of such a handle
/// opens a Scope for its whole body; a Scope opened while another is open on the same object
/// aborts, naming what was used. It catches the overlap itself, not the hand-off, which is
/// allowed, and costs one atomic exchange per call.
class ExclusiveUse {
 public:
  /// Marks the owning object as in use for the lifetime of the Scope.
  class Scope {
   public:
    /// Aborts if another Scope on `*use` is open. `what` names the object for the message and must
    /// be cheap to produce; it is only read if the check fails.
    Scope(ExclusiveUse* use, std::string_view what) : use_(use) {
      SHALE_CHECK(
          !use->busy_.exchange(true, std::memory_order_acquire),
          "concurrent use of " + std::string(what) + ": a handle is used by one thread at a time");
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    /// Marks the object as free again.
    ~Scope() { use_->busy_.store(false, std::memory_order_release); }

   private:
    ExclusiveUse* const use_;
  };

 private:
  std::atomic<bool> busy_{false};
};

}  // namespace shale

#endif  // SHALE_IO_EXCLUSIVE_USE_H_
