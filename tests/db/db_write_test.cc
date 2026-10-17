// The write path with more than one writer: group commit, the rules that slow or stop writes while
// level 0 is full or the flush is behind, and the error that stops them for good.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "db/db_impl.h"
#include "db/db_test_util.h"
#include "db/hook_env.h"
#include "io/write_batch_internal.h"
#include "shale/db.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"

namespace shale {
namespace {

using Op = FaultInjectionEnv::Op;

// Polls `done` every 100 microseconds and returns whether it came true within ten seconds. A test
// waits for progress with this and never for a fixed time: a slow machine only makes it slower.
bool PollUntil(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return true;
}

// What writer threads and the test share. The threads hold it, so it outlives a test that returns
// early from a failed ASSERT while they run: a thread that wrote to the stack of a test that was
// over would be a crash that hides the failure.
struct Progress {
  std::atomic<int> failures{0};  // writes that returned an error
  std::atomic<int> done{0};      // threads that have finished
  Status status;                 // what a thread keeps for the test; read after the join
};

// A Scheduler that holds every task until the test runs it, from any thread of the test's
// choosing, one at a time. Unlike a ManualScheduler it is safe to use from several threads, and it
// does not run tasks on the thread of whoever waits, so a writer that stalls really waits for the
// test: that is how a test decides when a flush or a compaction finishes. Open() lets everything
// that is held, and everything scheduled after, go to a thread of its own, for closing.
class GatedScheduler final : public Scheduler {
 public:
  GatedScheduler() : inner_(NewThreadScheduler()) {}

  void Schedule(std::function<void()> task) override {
    {
      const std::lock_guard<std::mutex> lock(mu_);
      if (!open_) {
        held_.push_back(std::move(task));
        return;
      }
    }
    inner_->Schedule(std::move(task));
  }

  bool RunsOnCallerThread() const override { return false; }
  size_t RunPending() override { return 0; }

  // Runs the oldest held task on the calling thread; returns false if none is held.
  bool RunOne() {
    std::function<void()> task;
    {
      const std::lock_guard<std::mutex> lock(mu_);
      if (held_.empty()) {
        return false;
      }
      task = std::move(held_.front());
      held_.pop_front();
    }
    task();
    return true;
  }

  // Sends what is held, and everything from now on, to the scheduler's own thread.
  void Open() {
    std::deque<std::function<void()>> held;
    {
      const std::lock_guard<std::mutex> lock(mu_);
      open_ = true;
      held.swap(held_);
    }
    for (std::function<void()>& task : held) {
      inner_->Schedule(std::move(task));
    }
  }

 private:
  std::mutex mu_;
  bool open_ = false;                       // guarded by mu_
  std::deque<std::function<void()>> held_;  // guarded by mu_
  const std::unique_ptr<Scheduler> inner_;  // destroyed first: it runs what it was handed
};

class DBWriteTest : public DBTestBase {
 protected:
  // Whatever a test did or did not get to, the leader that it holds goes on, the tasks that the
  // gate holds are let go, the threads are joined and the database is closed before the gate is
  // destroyed: a thread that outlives the test, or a writer that waits for a task that is held for
  // ever, would hang or end the process. The gate opens before the join, for the second.
  ~DBWriteTest() override {
    ReleaseLeader();
    if (gate_ != nullptr) {
      gate_->Open();
    }
    JoinWriters();
    Close();
  }

  // The options of a test that writes from several threads: the database runs its own scheduler.
  Options ThreadedOptions() {
    Options options = CurrentOptions();
    options.scheduler = nullptr;
    return options;
  }

  // Makes the next group leader, and every one after it until released, wait in the hook until
  // ReleaseLeader(). The writers that arrive meanwhile queue up behind it.
  void HoldLeaders() {
    dbfull()->TEST_SetBeforeWalWriteHook([future = release_future_] { future.wait(); });
  }

  void ReleaseLeader() {
    if (!released_) {
      released_ = true;
      release_.set_value();
    }
  }

  // Starts a thread that puts `key` = `value`, and returns the index of its status.
  size_t StartPut(std::string key, std::string value, bool sync = true) {
    batches_.emplace_back();
    batches_.back().Put(key, value);
    return StartWrite(&batches_.back(), sync);
  }

  // Starts a thread that writes `batch`, which the test keeps alive, and returns the index of its
  // status.
  size_t StartWrite(WriteBatch* batch, bool sync) {
    statuses_.emplace_back(Status::NotSupported("the write has not returned"));
    Status* const status = &statuses_.back();
    threads_.emplace_back([this, batch, sync, status] {
      WriteOptions options;
      options.sync = sync;
      *status = db_->Write(options, batch);
    });
    return statuses_.size() - 1;
  }

  void JoinWriters() {
    for (std::thread& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  // Whether the queue holds `n` writers within ten seconds.
  bool WaitForQueued(size_t n) {
    return PollUntil([&] { return dbfull()->TEST_QueuedWriters() == n; });
  }

  // Starts a put of a value of `size` bytes and waits until `n` writers are queued.
  void QueueBehind(const std::string& key, size_t size, size_t n) {
    StartPut(key, std::string(size, 'x'), /*sync=*/false);
    ASSERT_TRUE(WaitForQueued(n)) << "queued: " << dbfull()->TEST_QueuedWriters();
  }

  // The scheduler that holds tasks, made on first use.
  GatedScheduler* Gate() {
    if (gate_ == nullptr) {
      gate_ = std::make_unique<GatedScheduler>();
    }
    return gate_.get();
  }

  std::promise<void> release_;
  std::shared_future<void> release_future_ = release_.get_future().share();
  bool released_ = false;
  std::deque<WriteBatch> batches_;  // stable addresses: the threads hold them
  std::deque<Status> statuses_;
  std::vector<std::thread> threads_;
  std::unique_ptr<GatedScheduler> gate_;
};

TEST_F(DBWriteTest, GroupCommitMergesQueuedWriters) {
  Reopen(ThreadedOptions());
  HoldLeaders();
  StartPut("k0", "v0");
  // Once the first writer is in the queue, and visible, it has built its group, alone, and has
  // given up the mutex for the hook: the others cannot have been taken into it.
  ASSERT_TRUE(WaitForQueued(1));
  StartPut("k1", "v1");
  StartPut("k2", "v2");
  StartPut("k3", "v3");
  ASSERT_TRUE(WaitForQueued(4)) << "queued: " << dbfull()->TEST_QueuedWriters();
  ReleaseLeader();
  JoinWriters();

  for (const Status& status : statuses_) {
    EXPECT_OK(status);
  }
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(counters.write_groups, 2u) << "the three that queued share one log record";
  EXPECT_EQ(counters.wal_syncs, 2u) << "one sync a group";
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(Get("k" + std::to_string(i)), "v" + std::to_string(i));
  }
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 4u);
  EXPECT_EQ(dbfull()->TEST_QueuedWriters(), 0u);
  EXPECT_EQ(counters.user_bytes_written, 4u * (2 + 2)) << "every writer of a group is counted";
}

TEST_F(DBWriteTest, AGroupKeepsTheOrderOfTheQueue) {
  Reopen(ThreadedOptions());
  HoldLeaders();
  StartPut("first", "1");
  ASSERT_TRUE(WaitForQueued(1));
  // Three writes of one key, in the order they queue: the last to queue is the newest, and each
  // batch is numbered after the one before it.
  const size_t b = StartPut("k", "b");
  ASSERT_TRUE(WaitForQueued(2));
  const size_t c = StartPut("k", "c");
  ASSERT_TRUE(WaitForQueued(3));
  const size_t d = StartPut("k", "d");
  ASSERT_TRUE(WaitForQueued(4));
  ReleaseLeader();
  JoinWriters();

  EXPECT_OK(statuses_[b]);
  EXPECT_OK(statuses_[c]);
  EXPECT_OK(statuses_[d]);
  EXPECT_EQ(dbfull()->TEST_Counters().write_groups, 2u);
  EXPECT_EQ(Get("k"), "d");
  // Each writer's own batch carries its number, as it does when it is written alone.
  EXPECT_EQ(WriteBatchInternal::Sequence(&batches_[b]), 2u);
  EXPECT_EQ(WriteBatchInternal::Sequence(&batches_[c]), 3u);
  EXPECT_EQ(WriteBatchInternal::Sequence(&batches_[d]), 4u);
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 4u);
}

TEST_F(DBWriteTest, NonSyncLeaderDoesNotAbsorbSyncWriter) {
  Reopen(ThreadedOptions());
  HoldLeaders();
  StartPut("leader", "1", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(1));
  StartPut("n", "2", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(2));
  StartPut("s", "3", /*sync=*/true);
  ASSERT_TRUE(WaitForQueued(3));
  ReleaseLeader();
  JoinWriters();

  for (const Status& status : statuses_) {
    EXPECT_OK(status);
  }
  const DBCounters counters = dbfull()->TEST_Counters();
  // The leader of the second group does not sync, and a writer that asked for a sync cannot be in
  // its record, which would be acknowledged without one: it leads a group of its own.
  EXPECT_EQ(counters.write_groups, 3u);
  EXPECT_EQ(counters.wal_syncs, 1u);
  EXPECT_EQ(Get("s"), "3");
}

TEST_F(DBWriteTest, ASyncLeaderTakesNonSyncWritersAndSyncsOnce) {
  Reopen(ThreadedOptions());
  HoldLeaders();
  StartPut("first", "1", /*sync=*/true);
  ASSERT_TRUE(WaitForQueued(1));
  StartPut("a", "2", /*sync=*/true);  // leads the second group
  ASSERT_TRUE(WaitForQueued(2));
  StartPut("b", "3", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(3));
  StartPut("c", "4", /*sync=*/true);
  ASSERT_TRUE(WaitForQueued(4));
  ReleaseLeader();
  JoinWriters();

  for (const Status& status : statuses_) {
    EXPECT_OK(status);
  }
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(counters.write_groups, 2u);
  EXPECT_EQ(counters.wal_syncs, 2u) << "the first group, and the second, whatever it holds";
}

TEST_F(DBWriteTest, TheSizeOfAGroupIsBoundedByTheSizeOfItsLeader) {
  // A leader of 128 KiB or less takes followers up to its own size and 128 KiB more. The leader
  // here is 10 KiB, so the limit is about 138 KiB: it takes one follower of 100 KiB and not two.
  Reopen(ThreadedOptions());
  HoldLeaders();
  ASSERT_NO_FATAL_FAILURE(QueueBehind("first", 10, 1));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("a", 10 << 10, 2));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("b", 100 << 10, 3));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("c", 100 << 10, 4));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("d", 100 << 10, 5));
  ReleaseLeader();
  JoinWriters();

  for (const Status& status : statuses_) {
    EXPECT_OK(status);
  }
  // first | a b | c d: without the bound, one group would hold all four.
  EXPECT_EQ(dbfull()->TEST_Counters().write_groups, 3u);
}

TEST_F(DBWriteTest, AGroupWithALargeLeaderStopsAtOneMebibyte) {
  // A leader above 128 KiB takes followers up to 1 MiB in all: 200 KiB and two of 300 KiB are
  // 800 KiB, and a third follower would make 1100 KiB.
  Reopen(ThreadedOptions());
  HoldLeaders();
  ASSERT_NO_FATAL_FAILURE(QueueBehind("first", 10, 1));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("a", 200 << 10, 2));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("b", 300 << 10, 3));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("c", 300 << 10, 4));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("d", 300 << 10, 5));
  ReleaseLeader();
  JoinWriters();

  for (const Status& status : statuses_) {
    EXPECT_OK(status);
  }
  EXPECT_EQ(dbfull()->TEST_Counters().write_groups, 3u) << "first | a b c | d";
}

TEST_F(DBWriteTest, ALeaderAboveOneMebibyteTakesNoFollower) {
  Reopen(ThreadedOptions());
  HoldLeaders();
  ASSERT_NO_FATAL_FAILURE(QueueBehind("first", 10, 1));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("big", 1536 << 10, 2));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("a", 10, 3));
  ASSERT_NO_FATAL_FAILURE(QueueBehind("b", 10, 4));
  ReleaseLeader();
  JoinWriters();

  for (const Status& status : statuses_) {
    EXPECT_OK(status);
  }
  EXPECT_EQ(dbfull()->TEST_Counters().write_groups, 3u) << "first | big | a b";
  EXPECT_EQ(Get("big"), std::string(1536 << 10, 'x'));
}

TEST_F(DBWriteTest, AFailedGroupFailsEveryMemberWithTheSameStatus) {
  Reopen(ThreadedOptions());
  HoldLeaders();
  StartPut("first", "1");
  ASSERT_TRUE(WaitForQueued(1));
  StartPut("a", "2");
  ASSERT_TRUE(WaitForQueued(2));
  StartPut("b", "3");
  ASSERT_TRUE(WaitForQueued(3));
  StartPut("c", "4");
  ASSERT_TRUE(WaitForQueued(4));
  env_->FailOpAfter(Op::kSync, 1);  // the first group's sync goes through, the second's does not
  ReleaseLeader();
  JoinWriters();

  EXPECT_OK(statuses_[0]);
  for (size_t i = 1; i < statuses_.size(); i++) {
    EXPECT_TRUE(statuses_[i].IsIOError()) << i << ": " << statuses_[i].ToString();
    EXPECT_EQ(statuses_[i].ToString(), statuses_[1].ToString());
  }
  EXPECT_EQ(Get("first"), "1");
  for (const char* key : {"a", "b", "c"}) {
    EXPECT_EQ(Get(key), "NOT_FOUND") << "a write that failed is not visible";
  }
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u) << "one error, whatever the size of the group";
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 1u);
}

TEST_F(DBWriteTest, SlowdownSleepsOncePerWrite) {
  Options options = CurrentOptions();
  options.l0_compaction_trigger = 10;
  options.l0_slowdown_trigger = 2;
  options.l0_stop_trigger = 10;
  Reopen(options);
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Put("b", "2"));
  EXPECT_EQ(dbfull()->TEST_Counters().write_slowdowns, 0u) << "level 0 has one file";
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 2);
  EXPECT_EQ(dbfull()->TEST_Counters().write_slowdowns, 0u) << "a flush is not a write to slow";

  const uint64_t t0 = env_->NowMicros();
  ASSERT_OK(Put("c", "3"));
  EXPECT_EQ(dbfull()->TEST_Counters().write_slowdowns, 1u);
  EXPECT_GE(env_->NowMicros() - t0, 1000u) << "the write slept for a millisecond";
  const uint64_t after_one = env_->NowMicros();

  ASSERT_OK(db_->Flush());  // no sleep for it either, with level 0 at three files
  EXPECT_EQ(env_->NowMicros(), after_one);
  ASSERT_OK(Put("d", "4"));
  EXPECT_EQ(dbfull()->TEST_Counters().write_slowdowns, 2u);
  EXPECT_EQ(env_->NowMicros() - after_one, 1000u) << "once, and not more, for one write";
  EXPECT_EQ(Get("c"), "3");
}

TEST_F(DBWriteTest, StopWaitsForBackgroundWork) {
  // The scheduler is never run by the test: the writer that has to wait runs the background work
  // itself, which is what keeps it from waiting for ever.
  Reopen(SmallBuffer());
  for (int i = 0; i < 300; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false)) << "write " << i;
  }
  EXPECT_GE(dbfull()->TEST_Counters().write_stops, 1u);
  EXPECT_GE(dbfull()->TEST_Counters().flushes, 1u);
  EXPECT_EQ(Get(UserKey(0)), KiB(0));
  EXPECT_EQ(Get(UserKey(299)), KiB(299));
}

TEST_F(DBWriteTest, ALevelZeroStopWithACompactionTriggerAboveItStillMakesProgress) {
  // Level 0 is compacted at the lower of the two triggers: a write that stops at three files must
  // have a compaction to wait for, or it would wait for ever, and on this scheduler abort.
  Options options = SmallBuffer();
  options.l0_compaction_trigger = 100;
  options.l0_slowdown_trigger = 3;
  options.l0_stop_trigger = 3;
  Reopen(options);
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false)) << "write " << i;
  }
  EXPECT_GE(dbfull()->TEST_Counters().compactions + dbfull()->TEST_Counters().trivial_moves, 1u);
  EXPECT_LE(NumTableFilesAtLevel(0), 3);
  EXPECT_EQ(Get(UserKey(199)), KiB(199));
}

TEST_F(DBWriteTest, AWriterWaitsAtTheStopTriggerUntilACompactionRuns) {
  // Background work runs only when the test says. The writer fills memtables, each of which has to
  // be flushed before the next can be switched, and level 0 grows with each flush until a
  // compaction runs: the writer must stop at the trigger, and the test shows it stopped there.
  Options options = SmallBuffer();
  options.scheduler = Gate();
  options.l0_compaction_trigger = 2;
  options.l0_slowdown_trigger = 3;
  options.l0_stop_trigger = 3;
  Reopen(options);

  constexpr int kPuts = 200;
  const auto progress = std::make_shared<Progress>();
  threads_.emplace_back([this, progress] {
    for (int i = 0; i < kPuts; i++) {
      if (!Put(UserKey(i), KiB(i), /*sync=*/false).ok()) {
        progress->failures++;
      }
    }
    progress->done++;
  });

  uint64_t stops_seen = 0;
  int most_files = 0;
  int stopped_at_trigger = 0;
  int tasks_run = 0;
  while (true) {
    // The writer has stopped when it has counted a wait, which it does before it blocks, with the
    // mutex held: the count shows only after it is blocked. Or it is done.
    ASSERT_TRUE(PollUntil([&] {
      return progress->done.load() > 0 || dbfull()->TEST_Counters().write_stops > stops_seen;
    })) << "the writer neither finished nor stopped";
    if (progress->done.load() > 0) {
      break;
    }
    stops_seen = dbfull()->TEST_Counters().write_stops;
    const int files = NumTableFilesAtLevel(0);
    most_files = std::max(most_files, files);
    if (files == options.l0_stop_trigger) {
      stopped_at_trigger++;
    }
    ASSERT_TRUE(gate_->RunOne()) << "the writer waits, and no background task is held";
    tasks_run++;
  }
  JoinWriters();
  EXPECT_EQ(progress->failures.load(), 0);
  EXPECT_LE(most_files, options.l0_stop_trigger);
  EXPECT_GT(stopped_at_trigger, 0) << "the writer never stopped for level 0: it proves nothing";
  EXPECT_GT(tasks_run, 4);
  EXPECT_GE(dbfull()->TEST_Counters().write_stops, stops_seen);
  EXPECT_LE(NumTableFilesAtLevel(0), options.l0_stop_trigger);
  gate_->Open();
  EXPECT_EQ(Get(UserKey(kPuts - 1)), KiB(kPuts - 1));
}

TEST_F(DBWriteTest, SustainedWritesKeepLevelZeroAtOrBelowTheStopTrigger) {
  Options options = SmallBuffer();
  options.scheduler = nullptr;
  options.l0_compaction_trigger = 2;
  options.l0_slowdown_trigger = 3;
  options.l0_stop_trigger = 4;
  Reopen(options);

  constexpr int kWriters = 4;
  constexpr int kPutsEach = 400;
  const auto progress = std::make_shared<Progress>();
  const auto most_files = std::make_shared<std::atomic<int>>(0);
  // The monitor is one of the threads, so that it too is joined whatever the test does.
  threads_.emplace_back([this, progress, most_files] {
    while (progress->done.load() < kWriters) {
      const int files = NumTableFilesAtLevel(0);
      int seen = most_files->load();
      while (files > seen && !most_files->compare_exchange_weak(seen, files)) {
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });
  for (int w = 0; w < kWriters; w++) {
    threads_.emplace_back([this, progress, w] {
      for (int i = 0; i < kPutsEach; i++) {
        if (!Put("w" + std::to_string(w) + "-" + UserKey(i), KiB(i), /*sync=*/false).ok()) {
          progress->failures++;
        }
      }
      progress->done++;
    });
  }
  JoinWriters();

  EXPECT_EQ(progress->failures.load(), 0);
  EXPECT_LE(most_files->load(), options.l0_stop_trigger)
      << "level 0 grew past the stop trigger under sustained writes";
  ASSERT_OK(db_->Flush());
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_GE(counters.flushes, 50u) << "the writers were too gentle to prove anything";
  EXPECT_GE(counters.compactions + counters.trivial_moves, 1u);
  EXPECT_EQ(Get("w3-" + UserKey(kPutsEach - 1)), KiB(kPutsEach - 1));
  EXPECT_EQ(dbfull()->TEST_LastSequence(), SequenceNumber{kWriters * kPutsEach});
}

TEST_F(DBWriteTest, WalSyncFailureStopsWrites) {
  ASSERT_OK(Put("before", "1"));
  env_->FailOpAfter(Op::kSync, 0);
  const Status failed = Put("lost", "2");
  EXPECT_TRUE(failed.IsIOError()) << failed.ToString();
  const Status later = Put("after", "3", /*sync=*/false);
  EXPECT_EQ(later.ToString(), failed.ToString()) << "every later write returns the first error";
  EXPECT_EQ(Get("before"), "1") << "reads go on";
  EXPECT_EQ(Get("lost"), "NOT_FOUND");
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
}

TEST_F(DBWriteTest, WalSyncFailureRecoversAfterReopen) {
  ASSERT_OK(Put("before", "1"));
  env_->FailOpAfter(Op::kSync, 0);
  ASSERT_TRUE(Put("lost", "2").IsIOError());
  env_->ClearFaults();
  Reopen();
  ASSERT_OK(Put("after", "3"));
  EXPECT_EQ(Get("before"), "1");
  EXPECT_EQ(Get("after"), "3");
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 0u);
}

TEST_F(DBWriteTest, AStalledWriterAndTheOnesQueuedBehindItAreWokenByABackgroundError) {
  // The memtable is full and its predecessor is waiting for a flush that the test holds back. The
  // writer waits for it, two more queue up behind that one, and the flush fails: all three return
  // the error, and none waits for a flush that will never come.
  Options options = SmallBuffer();
  HookEnv* const hook_env = NewHookEnv();
  // Shared with the hook, which the database may call until it is closed, after this body is over.
  const auto fail_tables = std::make_shared<std::atomic<bool>>(false);
  hook_env->SetHook([fail_tables](std::string_view call, const std::string& path) {
    if (fail_tables->load() && call == "new_writable" && path.ends_with(".sst")) {
      return Status::IOError("injected: no table can be made");
    }
    return Status::OK();
  });
  options.env = hook_env;
  options.scheduler = Gate();
  Reopen(options);

  const auto progress = std::make_shared<Progress>();
  threads_.emplace_back([this, progress] {
    for (int i = 0; i < 1000 && progress->status.ok(); i++) {
      progress->status = Put(UserKey(i), KiB(i), /*sync=*/false);
    }
    progress->done++;
  });
  ASSERT_TRUE(PollUntil([&] { return dbfull()->TEST_Counters().write_stops > 0; }))
      << "the writer never stopped";
  StartPut("x", "1", /*sync=*/false);
  StartPut("y", "2", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(3)) << "queued: " << dbfull()->TEST_QueuedWriters();

  fail_tables->store(true);
  ASSERT_TRUE(gate_->RunOne());  // the flush, which fails
  JoinWriters();
  EXPECT_EQ(progress->done.load(), 1);
  const Status first_status = progress->status;
  EXPECT_TRUE(first_status.IsIOError()) << first_status.ToString();
  for (const Status& status : statuses_) {
    EXPECT_EQ(status.ToString(), first_status.ToString());
  }
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
  EXPECT_EQ(Get(UserKey(0)), KiB(0)) << "reads go on";
  EXPECT_EQ(dbfull()->TEST_QueuedWriters(), 0u);
  hook_env->SetHook(nullptr);
}

TEST_F(DBWriteTest, ConcurrentWritersWithThreadScheduler) {
  Options options = ThreadedOptions();
  options.write_buffer_size = 65536;
  Reopen(options);

  constexpr int kThreads = 8;
  constexpr int kPutsEach = 500;
  const auto key_of = [](int thread, int i) {
    return "t" + std::to_string(thread) + "-" + UserKey(i);
  };
  const auto value_of = [](int thread, int i) {
    return std::string(48, static_cast<char>('a' + thread)) + ValueOf(i);
  };
  const auto progress = std::make_shared<Progress>();
  for (int t = 0; t < kThreads; t++) {
    threads_.emplace_back([this, progress, key_of, value_of, t] {
      for (int i = 0; i < kPutsEach; i++) {
        if (!Put(key_of(t, i), value_of(t, i), /*sync=*/false).ok()) {
          progress->failures++;
        }
      }
    });
  }
  JoinWriters();
  EXPECT_EQ(progress->failures.load(), 0);
  ASSERT_OK(db_->Flush());
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(dbfull()->TEST_LastSequence(), SequenceNumber{kThreads * kPutsEach});
  EXPECT_GE(counters.flushes, 3u) << "the memtable never filled: the test proves less than it says";
  EXPECT_GE(counters.write_groups, 1u);
  EXPECT_LE(counters.write_groups, uint64_t{kThreads * kPutsEach});
  for (int t = 0; t < kThreads; t++) {
    for (int i = 0; i < kPutsEach; i++) {
      ASSERT_EQ(Get(key_of(t, i)), value_of(t, i)) << key_of(t, i);
    }
  }
  Close();  // the thread scheduler is joined, and nothing is left running
  Reopen(options);
  EXPECT_EQ(Get(key_of(7, 499)), value_of(7, 499));
}

TEST_F(DBWriteTest, FlushesFromAnotherThreadAreOrderedWithTheWriters) {
  Options options = ThreadedOptions();
  options.write_buffer_size = 65536;
  Reopen(options);

  constexpr int kWriters = 4;
  constexpr int kPutsEach = 300;
  const auto progress = std::make_shared<Progress>();
  for (int w = 0; w < kWriters; w++) {
    threads_.emplace_back([this, progress, w] {
      for (int i = 0; i < kPutsEach; i++) {
        if (!Put("w" + std::to_string(w) + "-" + UserKey(i), KiB(i), /*sync=*/false).ok()) {
          progress->failures++;
        }
      }
      progress->done++;
    });
  }
  int flushes = 0;
  Status flush_status;
  while (progress->done.load() < kWriters && flush_status.ok()) {
    flush_status = db_->Flush();
    flushes++;
  }
  JoinWriters();
  ASSERT_OK(flush_status);
  EXPECT_EQ(progress->failures.load(), 0);
  EXPECT_GE(flushes, 1);
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(dbfull()->TEST_LastSequence(), SequenceNumber{kWriters * kPutsEach});
  for (int w = 0; w < kWriters; w++) {
    for (int i = 0; i < kPutsEach; i++) {
      ASSERT_EQ(Get("w" + std::to_string(w) + "-" + UserKey(i)), KiB(i));
    }
  }
}

TEST_F(DBWriteTest, UserBytesCounted) {
  ASSERT_OK(Put("ab", "cde"));
  EXPECT_EQ(dbfull()->TEST_Counters().user_bytes_written, 5u);
  WriteBatch batch;
  batch.Put("k", "vv");
  batch.Delete("gone");
  ASSERT_OK(db_->Write(WriteOptions(), &batch));
  EXPECT_EQ(dbfull()->TEST_Counters().user_bytes_written, 5u + 3u + 4u);
  ASSERT_TRUE(db_->Put(WriteOptions(), std::string(kMaxKeySize + 1, 'k'), "v").IsInvalidArgument());
  EXPECT_EQ(dbfull()->TEST_Counters().user_bytes_written, 12u) << "a refused write counts nothing";
}

TEST_F(DBWriteTest, WalBytesAreTheBytesOfTheRecordsAndOneGroupIsOneRecord) {
  WriteBatch expected;
  expected.Put("ab", "cde");
  ASSERT_OK(Put("ab", "cde"));
  const DBCounters one = dbfull()->TEST_Counters();
  EXPECT_EQ(one.wal_bytes_written, expected.ApproximateSize());
  EXPECT_EQ(one.write_groups, 1u);
  EXPECT_EQ(one.wal_syncs, 1u);
  ASSERT_OK(Put("ab", "cde", /*sync=*/false));
  const DBCounters two = dbfull()->TEST_Counters();
  EXPECT_EQ(two.wal_bytes_written, 2 * expected.ApproximateSize());
  EXPECT_EQ(two.wal_syncs, 1u) << "a write that is not synced does not sync";
  EXPECT_EQ(two.write_groups, 2u);
}

}  // namespace
}  // namespace shale
