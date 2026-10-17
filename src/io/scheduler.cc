#include "shale/scheduler.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#include "util/check.h"
#include "util/mutex.h"
#include "util/thread_annotations.h"

namespace shale {

Scheduler::~Scheduler() = default;

namespace {

class ThreadScheduler final : public Scheduler {
 public:
  ThreadScheduler() : wake_(&mu_), worker_([this] { Work(); }) {}

  ~ThreadScheduler() override {
    // A worker that destroys its own scheduler would wait to join itself, forever.
    SHALE_CHECK(std::this_thread::get_id() != worker_.get_id(),
                "a thread scheduler destroyed from one of its own tasks");
    {
      MutexLock lock(&mu_);
      stopping_ = true;
    }
    wake_.SignalAll();
    worker_.join();
  }

  ThreadScheduler(const ThreadScheduler&) = delete;
  ThreadScheduler& operator=(const ThreadScheduler&) = delete;

  void Schedule(std::function<void()> task) override {
    SHALE_CHECK(task != nullptr, "Scheduler::Schedule needs a task");
    {
      MutexLock lock(&mu_);
      queue_.push_back(std::move(task));
    }
    wake_.Signal();
  }

  bool RunsOnCallerThread() const override { return false; }

  size_t RunPending() override { return 0; }

 private:
  // The worker thread. It keeps taking tasks until the destructor has asked it to stop and the
  // queue is empty, so that a task queued while the scheduler winds down still runs.
  void Work() {
    for (;;) {
      std::function<void()> task;
      {
        MutexLock lock(&mu_);
        while (queue_.empty() && !stopping_) {
          wake_.Wait();
        }
        if (queue_.empty()) {
          return;
        }
        task = std::move(queue_.front());
        queue_.pop_front();
      }
      // The lock is released: a task may call Schedule(), and a long task must not hold it.
      task();
    }
  }

  Mutex mu_;
  CondVar wake_;
  std::deque<std::function<void()>> queue_ SHALE_GUARDED_BY(mu_);
  bool stopping_ SHALE_GUARDED_BY(mu_) = false;
  // Declared last: the thread starts in the constructor and uses everything above.
  std::thread worker_;
};

}  // namespace

std::unique_ptr<Scheduler> NewThreadScheduler() { return std::make_unique<ThreadScheduler>(); }

class ManualScheduler::Use {
 public:
  explicit Use(const ManualScheduler* scheduler) : scheduler_(scheduler) {
    const std::thread::id self = std::this_thread::get_id();
    // Relaxed is enough to recognise ourselves: only this thread ever stores its own id, so no
    // other thread can make this load return it.
    if (scheduler_->user_.load(std::memory_order_relaxed) == self) {
      scheduler_->depth_++;
      return;
    }
    std::thread::id none;
    const bool acquired =
        scheduler_->user_.compare_exchange_strong(none, self, std::memory_order_acquire);
    SHALE_CHECK(acquired, "concurrent use of ManualScheduler: it is not thread-safe");
    scheduler_->depth_ = 1;
  }

  Use(const Use&) = delete;
  Use& operator=(const Use&) = delete;

  ~Use() {
    if (--scheduler_->depth_ == 0) {
      scheduler_->user_.store(std::thread::id(), std::memory_order_release);
    }
  }

 private:
  const ManualScheduler* const scheduler_;
};

void ManualScheduler::Schedule(std::function<void()> task) {
  SHALE_CHECK(task != nullptr, "Scheduler::Schedule needs a task");
  const Use use(this);
  queue_.push_back(std::move(task));
}

bool ManualScheduler::RunsOnCallerThread() const { return true; }

size_t ManualScheduler::RunPending() {
  size_t ran = 0;
  while (RunOne()) {
    ran++;
  }
  return ran;
}

bool ManualScheduler::RunOne() {
  const Use use(this);
  SHALE_CHECK(!running_, "RunOne or RunPending called from inside a task: tasks run one at a time");
  if (queue_.empty()) {
    return false;
  }
  // Take the task off the queue before running it, so that it can queue more work.
  std::function<void()> task = std::move(queue_.front());
  queue_.pop_front();
  running_ = true;
  task();
  running_ = false;
  return true;
}

size_t ManualScheduler::pending() const {
  const Use use(this);
  return queue_.size();
}

}  // namespace shale
