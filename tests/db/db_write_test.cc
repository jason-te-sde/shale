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
#include "io/log_format.h"
#include "io/write_batch_internal.h"
#include "shale/db.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/filename.h"
#include "util/random.h"

namespace shale {
namespace {

using Op = FaultInjectionEnv::Op;

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
  // Every task that was run was run for a stalled writer, which counted a wait before it blocked,
  // and that count was seen before the task was run: so there are at least as many waits as tasks.
  EXPECT_GE(dbfull()->TEST_Counters().write_stops, static_cast<uint64_t>(tasks_run));
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

// What the log file holds now: the bytes of the newest log, which the database writes to, as
// readers of the Env see them.
size_t LogFileSize(FaultInjectionEnv* env, const std::string& dbname) {
  return env->FileContents(NewestLogFile(env, dbname)).size();
}

TEST_F(DBWriteTest, WalBytesAreTheBytesOfTheLogFileHeadersIncluded) {
  // One put of "ab" and "cde" is a record of 12 bytes of header, 1 of type, and the lengths and
  // bytes of the key and the value: 20, and the log gives it a header of 7.
  WriteBatch record;
  record.Put("ab", "cde");
  ASSERT_EQ(record.ApproximateSize(), 20u);
  ASSERT_OK(Put("ab", "cde"));
  const DBCounters one = dbfull()->TEST_Counters();
  EXPECT_EQ(one.wal_bytes_written, 20u + log::kHeaderSize);
  EXPECT_EQ(one.wal_bytes_written, LogFileSize(env_.get(), dbname_));
  EXPECT_EQ(one.write_groups, 1u);
  EXPECT_EQ(one.wal_syncs, 1u);
  ASSERT_OK(Put("ab", "cde", /*sync=*/false));
  const DBCounters two = dbfull()->TEST_Counters();
  EXPECT_EQ(two.wal_bytes_written, 2 * (20u + log::kHeaderSize));
  EXPECT_EQ(two.wal_syncs, 1u) << "a write that is not synced does not sync";
  EXPECT_EQ(two.write_groups, 2u);
}

TEST_F(DBWriteTest, WalBytesFollowTheLogFileThroughFragmentsAndBlockPadding) {
  // Records of every size across the 32 KiB blocks of the log: a record that spans blocks takes a
  // header for each fragment, and the end of a block too short for a header is zeros that the next
  // record pays for. The counter is the size of the file at every step, whatever the sizes are.
  Random rnd(41);
  for (int i = 0; i < 60; i++) {
    const size_t size = i % 7 == 0 ? 70000 + rnd.Uniform(100) : 1 + rnd.Uniform(9000);
    ASSERT_OK(Put(UserKey(i), std::string(size, 'v'), /*sync=*/false)) << "write " << i;
    ASSERT_EQ(dbfull()->TEST_Counters().wal_bytes_written, LogFileSize(env_.get(), dbname_))
        << "after write " << i << " of " << size << " bytes";
  }
  EXPECT_GT(dbfull()->TEST_Counters().wal_bytes_written, 3u * log::kBlockSize);
}

TEST_F(DBWriteTest, OneGroupIsOneRecordWithOneHeader) {
  Reopen(ThreadedOptions());
  HoldLeaders();
  StartPut("first", "1", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(1));
  StartPut("a", "2", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(2));
  StartPut("b", "3", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(3));
  StartPut("c", "4", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(4));
  ReleaseLeader();
  JoinWriters();

  WriteBatch first;
  first.Put("first", "1");
  WriteBatch group;  // what the three that queued became: their operations, under one header
  group.Put("a", "2");
  group.Put("b", "3");
  group.Put("c", "4");
  const DBCounters counters = dbfull()->TEST_Counters();
  ASSERT_EQ(counters.write_groups, 2u);
  EXPECT_EQ(counters.wal_bytes_written,
            first.ApproximateSize() + group.ApproximateSize() + 2 * log::kHeaderSize)
      << "the group is one record, with one header and not three";
  EXPECT_EQ(counters.wal_bytes_written, LogFileSize(env_.get(), dbname_));
}

TEST_F(DBWriteTest, AGroupWhoseSyncFailedCountsItsBytesWhichAreInTheFile) {
  // The bytes were written, and a reopen finds them: the counter says what the log took, and not
  // what was acknowledged. The group is not counted as one that was made.
  ASSERT_OK(Put("before", "1"));
  const uint64_t before = dbfull()->TEST_Counters().wal_bytes_written;
  env_->FailOpAfter(Op::kSync, 0);
  ASSERT_TRUE(Put("lost", "2").IsIOError());
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_GT(counters.wal_bytes_written, before) << "the record of the failed group is in the file";
  EXPECT_EQ(counters.wal_bytes_written, LogFileSize(env_.get(), dbname_));
  EXPECT_EQ(counters.user_bytes_written, 6u + 1u) << "and the writes of it are not counted as made";
  EXPECT_EQ(counters.wal_syncs, 1u) << "only the sync that worked";
}

TEST_F(DBWriteTest, AnAppendThatTheFileRefusedCountsNothing) {
  env_->FailOpAfter(Op::kAppend, 0);
  ASSERT_TRUE(Put("lost", "1", /*sync=*/false).IsIOError());
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(counters.wal_bytes_written, 0u) << "not even the header went in";
  EXPECT_EQ(counters.write_groups, 0u) << "no record was appended";
  EXPECT_EQ(counters.bg_errors, 1u);
}

TEST_F(DBWriteTest, AGroupOfWritersSharesOneSlowdownSleep) {
  Options options = ThreadedOptions();
  options.l0_compaction_trigger = 10;
  options.l0_slowdown_trigger = 2;
  options.l0_stop_trigger = 10;
  Reopen(options);
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 2);
  ASSERT_EQ(dbfull()->TEST_Counters().write_slowdowns, 0u);
  const uint64_t groups_before = dbfull()->TEST_Counters().write_groups;

  const uint64_t t0 = env_->NowMicros();
  HoldLeaders();
  StartPut("first", "1", /*sync=*/false);  // the first group: its leader has slept already
  ASSERT_TRUE(WaitForQueued(1));
  StartPut("w1", "1", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(2));
  StartPut("w2", "2", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(3));
  StartPut("w3", "3", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(4));
  ReleaseLeader();
  JoinWriters();

  const DBCounters counters = dbfull()->TEST_Counters();
  ASSERT_EQ(counters.write_groups - groups_before, 2u);
  EXPECT_EQ(counters.write_slowdowns, 2u) << "four writes, two groups, a sleep for each group";
  EXPECT_EQ(env_->NowMicros() - t0, 2000u) << "the three that share a group slept once";
}

TEST_F(DBWriteTest, AWriteWithRoomInTheMemtableDoesNotWaitAtTheStopTrigger) {
  // Two tables are in level 0, which is the stop trigger, and a compaction is queued and not run.
  // The memtable has room, and a write that does not have to switch it has nothing to wait for:
  // it goes through, and the compaction is still queued. The write that has to switch waits, and
  // runs the compaction.
  Options options = SmallBuffer();
  options.l0_compaction_trigger = 100;  // taken to be the stop trigger
  options.l0_slowdown_trigger = 2;
  options.l0_stop_trigger = 2;
  Reopen(options);
  PutUntilSwitched();
  ASSERT_TRUE(scheduler_.RunOne()) << "the flush";
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  PutUntilSwitched();
  ASSERT_TRUE(scheduler_.RunOne()) << "the flush";
  ASSERT_EQ(NumTableFilesAtLevel(0), 2);
  ASSERT_EQ(scheduler_.pending(), 1u) << "the compaction that level 0 is due for";

  ASSERT_OK(Put("room", "1", /*sync=*/false));
  EXPECT_EQ(dbfull()->TEST_Counters().write_stops, 0u);
  EXPECT_EQ(NumTableFilesAtLevel(0), 2) << "the write waited for the compaction, or ran it";
  EXPECT_EQ(scheduler_.pending(), 1u) << "the compaction was run by a write that needed no room";
  EXPECT_EQ(Get("room"), "1");

  for (int i = 0; dbfull()->TEST_Counters().write_stops == 0 && i < 100; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
  }
  EXPECT_GE(dbfull()->TEST_Counters().write_stops, 1u) << "the switch did not wait";
  EXPECT_LT(NumTableFilesAtLevel(0), 2) << "and what it waited for was the compaction";
  EXPECT_GE(dbfull()->TEST_Counters().compactions + dbfull()->TEST_Counters().trivial_moves, 1u);
}

TEST_F(DBWriteTest, ABackgroundErrorInTheMiddleOfAGroupLetsTheGroupFinish) {
  // The group has been built and is waiting to be appended when a flush fails. The log is whole,
  // so the group is written and acknowledged, and the next write returns the error.
  Options options = SmallBuffer();
  HookEnv* const hook_env = NewHookEnv();
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
  PutUntilSwitched();  // the memtable is waiting for a flush that the gate holds
  ASSERT_EQ(dbfull()->TEST_Counters().bg_errors, 0u);

  HoldLeaders();
  const size_t mid = StartPut("mid", "group", /*sync=*/false);
  ASSERT_TRUE(WaitForQueued(1));
  fail_tables->store(true);
  ASSERT_TRUE(gate_->RunOne());  // the flush fails while the group waits for its turn at the log
  ASSERT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
  ReleaseLeader();
  JoinWriters();

  EXPECT_OK(statuses_[mid]) << "the group was built before the error and the log is whole";
  EXPECT_EQ(Get("mid"), "group");
  const Status next = Put("next", "1", /*sync=*/false);
  EXPECT_TRUE(next.IsIOError()) << next.ToString();
  std::string error;
  ASSERT_TRUE(db_->GetProperty("shale.background-error", &error));
  EXPECT_EQ(next.ToString(), error);
  EXPECT_EQ(Get("next"), "NOT_FOUND");
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
  hook_env->SetHook(nullptr);
}

// Closing with a write in progress aborts, so this runs in a child process.
class DBWriteDeathTest : public DBWriteTest {};

TEST_F(DBWriteDeathTest, ClosingWithAWriteInProgressAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        Reopen(ThreadedOptions());
        HoldLeaders();  // the leader waits in the hook, with its write in the queue
        StartPut("k", "v");
        static_cast<void>(WaitForQueued(1));
        db_.reset();
      },
      "DB closed while a write or a flush is in progress");
}

}  // namespace
}  // namespace shale
