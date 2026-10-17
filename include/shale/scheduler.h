#ifndef SHALE_SCHEDULER_H_
#define SHALE_SCHEDULER_H_

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <thread>

namespace shale {

/// Where the engine's background work runs: flushing a memtable, compacting files, deleting
/// obsolete ones.
///
/// The engine hands every piece of background work to a Scheduler and never starts a thread of its
/// own, so that a test can replace the thread with a driver that decides when each task runs. That
/// is what makes a crash simulation reproducible: the interleaving of foreground and background
/// work is then a choice of the test, not of the operating system.
///
/// Tasks submitted to one scheduler run one at a time, in the order they were submitted, so a task
/// never has to guard against another task of the same scheduler. For the same reason a task must
/// not wait for a task submitted after it: on a scheduler that runs tasks one at a time that wait
/// never ends.
class Scheduler {
 public:
  /// Does nothing; a Scheduler is destroyed through this base class.
  virtual ~Scheduler();

  /// Queues `task` to run later and returns without running it. A task may call Schedule() on the
  /// scheduler it runs on.
  /// @param task what to run; must not be empty.
  virtual void Schedule(std::function<void()> task) = 0;

  /// Returns true if tasks run only when RunPending() is called on the caller's thread, and false
  /// if the scheduler runs them on a thread of its own.
  virtual bool RunsOnCallerThread() const = 0;

  /// Runs every queued task on the calling thread, including the tasks that those tasks queue, and
  /// returns how many ran. Returns 0 at once, without running anything, when the scheduler runs
  /// tasks on a thread of its own.
  virtual size_t RunPending() = 0;
};

/// Returns a Scheduler that runs tasks, one at a time and in order, on one worker thread that it
/// starts itself.
///
/// It is safe to use from any number of threads. Its destructor runs every task that is still
/// queued, including the tasks those tasks queue while it does, and then joins the worker: queued
/// work is finished, not abandoned. Nothing may call Schedule() on it from outside once its
/// destructor has begun, and it must not be destroyed from one of its own tasks, which aborts the
/// process because the worker cannot join itself.
std::unique_ptr<Scheduler> NewThreadScheduler();

/// A Scheduler that runs a task only when its owner says so, on the owner's thread.
///
/// It is for deterministic drivers: a test or a simulator calls Schedule() through the engine,
/// then decides when to run the queued work with RunOne() or RunPending(), and so decides how
/// foreground and background work interleave.
///
/// It is NOT thread-safe, and it is meant for a single-threaded driver. Two threads may use it one
/// after the other, but not at the same time: a call that overlaps another one on a different
/// thread aborts the process, naming the class. So does running tasks from inside a task, because
/// that would run a second task inside the first. A task may call Schedule() and pending() on the
/// scheduler it runs on.
///
/// Destroying it drops the tasks still queued without running them.
class ManualScheduler final : public Scheduler {
 public:
  /// Queues `task` behind the tasks already queued. It does not run it.
  /// @param task what to run; must not be empty.
  void Schedule(std::function<void()> task) override;

  /// Returns true: tasks run when the owner calls RunPending() or RunOne().
  bool RunsOnCallerThread() const override;

  /// Runs the queued tasks in order until none is left, including the ones those tasks queue, and
  /// returns how many ran. A task that always queues another one makes it run forever.
  size_t RunPending() override;

  /// Runs the oldest queued task and returns true, or returns false if none is queued. Tasks that
  /// the task queues stay queued.
  bool RunOne();

  /// Returns how many tasks are queued and have not started.
  size_t pending() const;

 private:
  // Marks the scheduler as in use by the calling thread for the lifetime of the object, and aborts
  // if another thread is already inside it. Defined in scheduler.cc.
  class Use;

  mutable std::atomic<std::thread::id> user_{};
  // How many Use objects the thread in `user_` has open. Only that thread touches it.
  mutable int depth_ = 0;
  // True while a task runs. Touched only by the thread in `user_`.
  bool running_ = false;
  std::deque<std::function<void()>> queue_;
};

}  // namespace shale

#endif  // SHALE_SCHEDULER_H_
