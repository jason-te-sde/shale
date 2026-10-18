#include "shale/scheduler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace shale {
namespace {

TEST(ManualSchedulerTest, ScheduleDoesNotRun) {
  ManualScheduler scheduler;
  int counter = 0;
  scheduler.Schedule([&counter] { counter++; });
  EXPECT_EQ(counter, 0);
  EXPECT_EQ(scheduler.pending(), 1u);
}

TEST(ManualSchedulerTest, RunsOnTheCallerThread) {
  ManualScheduler scheduler;
  EXPECT_TRUE(scheduler.RunsOnCallerThread());
  std::thread::id ran_on;
  scheduler.Schedule([&ran_on] { ran_on = std::this_thread::get_id(); });
  EXPECT_EQ(scheduler.RunPending(), 1u);
  EXPECT_EQ(ran_on, std::this_thread::get_id());
}

TEST(ManualSchedulerTest, RunOneIsFifo) {
  ManualScheduler scheduler;
  std::vector<int> order;
  for (int i = 1; i <= 3; i++) {
    scheduler.Schedule([&order, i] { order.push_back(i); });
  }
  EXPECT_TRUE(scheduler.RunOne());
  EXPECT_EQ(order, (std::vector<int>{1}));
  EXPECT_EQ(scheduler.pending(), 2u);
  EXPECT_TRUE(scheduler.RunOne());
  EXPECT_TRUE(scheduler.RunOne());
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_FALSE(scheduler.RunOne());
  EXPECT_EQ(scheduler.pending(), 0u);
}

TEST(ManualSchedulerTest, RunPendingOnAnEmptyQueueRunsNothing) {
  ManualScheduler scheduler;
  EXPECT_EQ(scheduler.RunPending(), 0u);
}

TEST(ManualSchedulerTest, RunPendingRunsTasksQueuedByTasks) {
  ManualScheduler scheduler;
  std::vector<int> order;
  scheduler.Schedule([&] {
    order.push_back(1);
    scheduler.Schedule([&order] { order.push_back(2); });
  });
  scheduler.Schedule([&order] { order.push_back(3); });
  EXPECT_EQ(scheduler.RunPending(), 3u);
  // The task queued by the first one goes behind the one that was already waiting.
  EXPECT_EQ(order, (std::vector<int>{1, 3, 2}));
  EXPECT_EQ(scheduler.pending(), 0u);
}

TEST(ManualSchedulerTest, ATaskCanBeQueuedWhileOneIsRunningUnderRunOne) {
  ManualScheduler scheduler;
  int counter = 0;
  scheduler.Schedule([&] { scheduler.Schedule([&counter] { counter++; }); });
  EXPECT_TRUE(scheduler.RunOne());
  EXPECT_EQ(scheduler.pending(), 1u) << "RunOne runs one task, not the ones it queues";
  EXPECT_EQ(counter, 0);
}

TEST(ManualSchedulerTest, DestroyingItDropsTheTasksStillQueued) {
  // A deterministic driver that stops pumping leaves the work undone, as a process that dies would.
  int counter = 0;
  {
    ManualScheduler scheduler;
    scheduler.Schedule([&counter] { counter++; });
  }
  EXPECT_EQ(counter, 0);
}

TEST(ManualSchedulerTest, PassingItFromOneThreadToAnotherIsFine) {
  // One thread after another is allowed. What is not allowed is two at once.
  ManualScheduler scheduler;
  int counter = 0;
  scheduler.Schedule([&counter] { counter++; });
  std::thread other([&scheduler] { EXPECT_EQ(scheduler.RunPending(), 1u); });
  other.join();
  scheduler.Schedule([&counter] { counter++; });
  EXPECT_EQ(scheduler.RunPending(), 1u);
  EXPECT_EQ(counter, 2);
}

TEST(ManualSchedulerDeathTest, ARunFromInsideATaskAborts) {
  // Tasks run one at a time. A task that pumps the scheduler would run a second task inside the
  // first, and the guarantee would hold only until someone relied on it.
  EXPECT_DEATH(
      {
        ManualScheduler scheduler;
        scheduler.Schedule([&scheduler] { static_cast<void>(scheduler.RunPending()); });
        static_cast<void>(scheduler.RunPending());
      },
      "inside a task");
  EXPECT_DEATH(
      {
        ManualScheduler scheduler;
        scheduler.Schedule([&scheduler] { static_cast<void>(scheduler.RunOne()); });
        static_cast<void>(scheduler.RunOne());
      },
      "inside a task");
}

TEST(ManualSchedulerDeathTest, ACallFromAnotherThreadDuringARunAborts) {
  EXPECT_DEATH(
      {
        ManualScheduler scheduler;
        scheduler.Schedule([&scheduler] {
          std::thread other([&scheduler] { scheduler.Schedule([] {}); });
          other.join();
        });
        static_cast<void>(scheduler.RunPending());
      },
      "concurrent use of ManualScheduler");
}

TEST(ManualSchedulerDeathTest, DestroyingItFromOneOfItsOwnTasksAborts) {
  // The scheduler would be freed under the task that is running, and the call that runs it would
  // go on to use it.
  EXPECT_DEATH(
      {
        auto scheduler = std::make_unique<ManualScheduler>();
        ManualScheduler* const raw = scheduler.get();
        scheduler->Schedule([&scheduler] { scheduler.reset(); });
        static_cast<void>(raw->RunPending());
      },
      "own tasks");
}

TEST(ManualSchedulerDeathTest, AnEmptyTaskAborts) {
  EXPECT_DEATH(
      {
        ManualScheduler scheduler;
        scheduler.Schedule(std::function<void()>());
      },
      "needs a task");
}

// A thread scheduler under test, plus a way to hold its worker inside the first task until the
// test says so.
struct BlockedWorker {
  std::promise<void> started;
  std::future<void> started_signal = started.get_future();
  std::promise<void> release;
  std::shared_future<void> release_signal = release.get_future().share();

  // Waits for the worker to be inside the first task, for at most ten seconds. If it never gets
  // there, the worker is released so that nothing in the test can wait for it for ever, and the
  // result is false: a broken scheduler fails the test instead of hanging it.
  bool WaitForStart() {
    if (started_signal.wait_for(std::chrono::seconds(10)) == std::future_status::ready) {
      return true;
    }
    release.set_value();
    return false;
  }

  // The first task: tells the test that the worker is inside it, waits to be released, then runs
  // `after`.
  std::function<void()> BlockingTask(std::function<void()> after) {
    return [this, after = std::move(after)] {
      started.set_value();
      release_signal.wait();
      after();
    };
  }
};

TEST(ThreadSchedulerTest, RunsInOrderOnOneOtherThread) {
  std::vector<std::pair<int, std::thread::id>> ran;
  {
    const std::unique_ptr<Scheduler> scheduler = NewThreadScheduler();
    for (int i = 0; i < 100; i++) {
      scheduler->Schedule([&ran, i] { ran.emplace_back(i, std::this_thread::get_id()); });
    }
  }  // The destructor runs everything that is queued and joins, so `ran` is complete and safe to
     // read below.
  ASSERT_EQ(ran.size(), 100u);
  for (size_t i = 0; i < ran.size(); i++) {
    EXPECT_EQ(ran[i].first, static_cast<int>(i));
    EXPECT_EQ(ran[i].second, ran[0].second) << "task " << i << " ran on a different thread";
  }
  EXPECT_NE(ran[0].second, std::this_thread::get_id());
}

TEST(ThreadSchedulerTest, DestructorDrainsQueue) {
  BlockedWorker worker;
  int counter = 0;
  auto scheduler = NewThreadScheduler();
  scheduler->Schedule(worker.BlockingTask([&counter] { counter++; }));
  ASSERT_TRUE(worker.WaitForStart()) << "the worker never started the first task";
  for (int i = 0; i < 100; i++) {
    scheduler->Schedule([&counter] { counter++; });
  }

  // Release the worker only once the destructor is under way, so that the 100 tasks are still
  // queued when it starts. A scheduler that discards its queue on destruction then fails here; one
  // that is released too early would run them before it was asked to stop, which proves nothing.
  // The pause cannot make a correct scheduler fail, only let a wrong one through if it is very
  // slow to start destroying.
  std::atomic<bool> destroying{false};
  std::thread releaser([&] {
    while (!destroying.load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    worker.release.set_value();
  });
  destroying.store(true);
  scheduler.reset();
  releaser.join();
  EXPECT_EQ(counter, 101) << "every queued task must have run before the destructor returned";
}

TEST(ThreadSchedulerTest, DestructorRunsTasksQueuedWhileItDrains) {
  BlockedWorker worker;
  int counter = 0;
  auto scheduler = NewThreadScheduler();
  Scheduler* const raw = scheduler.get();
  scheduler->Schedule(worker.BlockingTask([] {}));
  ASSERT_TRUE(worker.WaitForStart()) << "the worker never started the first task";
  // Queued behind the blocked task, so it runs after the destructor has begun, and queues a
  // follow-up from the worker thread while the scheduler is stopping.
  scheduler->Schedule([&counter, raw] {
    counter++;
    raw->Schedule([&counter] { counter++; });
  });

  std::atomic<bool> destroying{false};
  std::thread releaser([&] {
    while (!destroying.load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    worker.release.set_value();
  });
  destroying.store(true);
  scheduler.reset();
  releaser.join();
  EXPECT_EQ(counter, 2);
}

TEST(ThreadSchedulerTest, TasksRunWithoutAnyoneWaiting) {
  // The worker runs a task as soon as it is queued; nothing but Schedule() is needed.
  auto scheduler = NewThreadScheduler();
  std::promise<void> ran;
  std::future<void> ran_signal = ran.get_future();
  scheduler->Schedule([&ran] { ran.set_value(); });
  EXPECT_EQ(ran_signal.wait_for(std::chrono::seconds(30)), std::future_status::ready);
}

TEST(ThreadSchedulerTest, ATaskMayQueueMoreWork) {
  auto scheduler = NewThreadScheduler();
  std::promise<void> done;
  std::future<void> done_signal = done.get_future();
  scheduler->Schedule([&] { scheduler->Schedule([&done] { done.set_value(); }); });
  EXPECT_EQ(done_signal.wait_for(std::chrono::seconds(30)), std::future_status::ready);
}

TEST(ThreadSchedulerTest, ManyThreadsMayScheduleAtOnce) {
  // Each producer's own tasks run in the order it queued them, and none is lost.
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 250;
  std::vector<std::vector<int>> seen(kProducers);
  {
    const std::unique_ptr<Scheduler> scheduler = NewThreadScheduler();
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; p++) {
      producers.emplace_back([&, p] {
        for (int i = 0; i < kPerProducer; i++) {
          scheduler->Schedule([&seen, p, i] { seen[static_cast<size_t>(p)].push_back(i); });
        }
      });
    }
    for (std::thread& producer : producers) {
      producer.join();
    }
  }
  for (const std::vector<int>& list : seen) {
    ASSERT_EQ(list.size(), static_cast<size_t>(kPerProducer));
    for (size_t i = 0; i < list.size(); i++) {
      ASSERT_EQ(list[i], static_cast<int>(i));
    }
  }
}

TEST(ThreadSchedulerTest, RunPendingIsNoOp) {
  const std::unique_ptr<Scheduler> scheduler = NewThreadScheduler();
  EXPECT_FALSE(scheduler->RunsOnCallerThread());
  EXPECT_EQ(scheduler->RunPending(), 0u);
}

TEST(ThreadSchedulerDeathTest, AnEmptyTaskAborts) {
  EXPECT_DEATH(
      {
        const std::unique_ptr<Scheduler> scheduler = NewThreadScheduler();
        scheduler->Schedule(std::function<void()>());
      },
      "needs a task");
}

TEST(ThreadSchedulerDeathTest, DestroyingItFromOneOfItsOwnTasksAborts) {
  // The destructor joins the worker, and a worker cannot join itself: it would hang forever
  // instead of failing.
  EXPECT_DEATH(
      {
        std::unique_ptr<Scheduler> scheduler = NewThreadScheduler();
        std::promise<void> never;
        std::future<void> never_signal = never.get_future();
        scheduler->Schedule([&scheduler] { scheduler.reset(); });
        // Not reached if the scheduler aborts, as it must. If it does not, the test fails, in a
        // moment, rather than waits for ever.
        never_signal.wait_for(std::chrono::seconds(10));
      },
      "own tasks");
}

}  // namespace
}  // namespace shale
