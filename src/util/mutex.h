#ifndef SHALE_UTIL_MUTEX_H_
#define SHALE_UTIL_MUTEX_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "util/check.h"
#include "util/thread_annotations.h"

namespace shale {

class CondVar;

/// A mutual-exclusion lock that Clang's thread-safety analysis understands, and that knows which
/// thread holds it.
///
/// It wraps std::mutex, so it is not recursive and not copyable. The owner is recorded so that
/// the rules the analysis cannot check on every path are checked at run time instead: locking a
/// mutex the caller already holds, unlocking one it does not hold, and waiting on a condition
/// variable without it. Each aborts through SHALE_CHECK rather than deadlocking or corrupting the
/// state the mutex protects.
class SHALE_CAPABILITY("mutex") Mutex {
 public:
  Mutex() = default;
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  /// Blocks until the calling thread holds the mutex. Aborts if it already does.
  void Lock() SHALE_ACQUIRE() SHALE_NO_THREAD_SAFETY_ANALYSIS {
    SHALE_CHECK(owner_.load(std::memory_order_relaxed) != std::this_thread::get_id(),
                "mutex already held by this thread");
    mu_.lock();
    owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
  }

  /// Releases the mutex. Aborts if the calling thread does not hold it.
  void Unlock() SHALE_RELEASE() SHALE_NO_THREAD_SAFETY_ANALYSIS {
    AssertHeld();
    owner_.store(std::thread::id(), std::memory_order_relaxed);
    mu_.unlock();
  }

  /// Aborts unless the calling thread holds the mutex. It also tells the analysis that the mutex
  /// is held from here on, for the places where the lock was taken out of its sight.
  void AssertHeld() const SHALE_ASSERT_CAPABILITY(this) {
    // Relaxed is enough: only the holder ever stores its own id, so a thread that does not hold
    // the mutex can never read its own id here, whatever else it reads.
    SHALE_CHECK(owner_.load(std::memory_order_relaxed) == std::this_thread::get_id(),
                "mutex not held by this thread");
  }

 private:
  friend class CondVar;

  std::mutex mu_;
  std::atomic<std::thread::id> owner_{};
};

/// Holds a Mutex for the lifetime of the object: locks it on construction, unlocks it on
/// destruction. The analysis tracks the scope, so guarded members may be used while it lives.
class SHALE_SCOPED_CAPABILITY MutexLock {
 public:
  /// Locks `*mu`, which must outlive this object.
  explicit MutexLock(Mutex* mu) SHALE_ACQUIRE(mu) : mu_(mu) { mu->Lock(); }

  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;

  /// Unlocks the mutex.
  ~MutexLock() SHALE_RELEASE() { mu_->Unlock(); }

 private:
  Mutex* const mu_;
};

/// A condition variable bound to one Mutex for its whole life.
///
/// Wait() may return without a Signal(), so callers always wait in a loop that re-tests the
/// condition they are waiting for, with the mutex held.
class CondVar {
 public:
  /// Binds the condition variable to `*mu`, which must outlive it.
  explicit CondVar(Mutex* mu) : mu_(mu) {}

  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;

  /// Atomically releases the mutex and blocks until signalled, then reacquires the mutex before
  /// returning. The calling thread must hold the mutex; aborts if it does not.
  void Wait() {
    mu_->AssertHeld();
    // The mutex is not held by anyone while this thread sleeps, so its recorded owner must say so.
    mu_->owner_.store(std::thread::id(), std::memory_order_relaxed);
    {
      // std::condition_variable only waits on a std::unique_lock. Adopt the already-locked mutex
      // into one, and release() it afterwards so that its destructor leaves the mutex locked.
      std::unique_lock<std::mutex> lock(mu_->mu_, std::adopt_lock);
      cv_.wait(lock);
      lock.release();
    }
    mu_->owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
  }

  /// Wakes one thread blocked in Wait(), if there is any.
  void Signal() { cv_.notify_one(); }

  /// Wakes every thread blocked in Wait().
  void SignalAll() { cv_.notify_all(); }

 private:
  Mutex* const mu_;
  std::condition_variable cv_;
};

}  // namespace shale

#endif  // SHALE_UTIL_MUTEX_H_
