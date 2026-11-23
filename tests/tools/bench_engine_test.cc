// The adapter that shale_bench drives the database through. The workloads are written against
// BenchEngine alone, so what a call means here is what it will have to mean for any other engine
// that is measured with them: these tests pin the meaning that the comparison depends on, which is
// that a Scan visits what it says it visits and that a write is synced exactly when it is asked to
// be.

#include "bench_engine.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bench_workloads.h"
#include "shale/env.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"

namespace shale {
namespace {

constexpr char kPath[] = "/benchdb";

std::string Key(int index) {
  char text[32];
  std::snprintf(text, sizeof(text), "%016d", index);
  return text;
}

class BenchEngineTest : public ::testing::Test {
 protected:
  BenchEngineTest() : env_(NewMemEnv()) { options_.env = env_.get(); }

  std::unique_ptr<BenchEngine> OpenFresh() {
    std::unique_ptr<BenchEngine> engine = NewShaleBenchEngine(options_);
    EXPECT_OK(engine->Open(kPath, /*fresh=*/true));
    return engine;
  }

  // Puts the keys 0, 1, ..., count - 1, each with the value "v<index>".
  static void PutKeys(BenchEngine* engine, int count) {
    for (int i = 0; i < count; i++) {
      ASSERT_OK(engine->Put(Key(i), "v" + std::to_string(i), /*sync=*/false));
    }
  }

  // What Scan() reports for `from` and `limit`. The count starts as a number that no scan reports,
  // so that a call that leaves it alone is not taken for one that visited that many.
  static size_t Seen(BenchEngine* engine, const std::string& from, size_t limit) {
    size_t seen = 12345;
    EXPECT_OK(engine->Scan(from, limit, &seen));
    return seen;
  }

  std::unique_ptr<Env> env_;
  Options options_;
};

TEST_F(BenchEngineTest, NamesTheEngine) { EXPECT_EQ(OpenFresh()->Name(), "shale"); }

TEST_F(BenchEngineTest, PutThenGetReturnsTheValueAndAMissingKeyIsNotFound) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  ASSERT_OK(engine->Put(Key(7), "seven", /*sync=*/false));

  std::string value;
  ASSERT_OK(engine->Get(Key(7), &value));
  EXPECT_EQ(value, "seven");

  const Status missing = engine->Get(Key(8), &value);
  EXPECT_TRUE(missing.IsNotFound()) << missing.ToString();
}

TEST_F(BenchEngineTest, OpenFreshDiscardsWhatWasThereAndOpenNotFreshKeepsIt) {
  std::unique_ptr<BenchEngine> engine = OpenFresh();
  ASSERT_OK(engine->Put(Key(1), "one", /*sync=*/false));
  engine->Close();

  ASSERT_OK(engine->Open(kPath, /*fresh=*/false));
  std::string value;
  ASSERT_OK(engine->Get(Key(1), &value)) << "a database opened as it was must still hold its key";
  EXPECT_EQ(value, "one");

  ASSERT_OK(engine->Open(kPath, /*fresh=*/true));  // replaces the open database
  const Status gone = engine->Get(Key(1), &value);
  EXPECT_TRUE(gone.IsNotFound()) << "a fresh database must start empty: " << gone.ToString();
}

TEST_F(BenchEngineTest, ScanVisitsFromAKeyUpToTheLimit) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 10);

  EXPECT_EQ(Seen(engine.get(), Key(3), 4), 4u);
  EXPECT_EQ(Seen(engine.get(), Key(8), 4), 2u) << "only keys 8 and 9 are at or after key 8";
  EXPECT_EQ(Seen(engine.get(), Key(3), 1), 1u);
}

TEST_F(BenchEngineTest, ScanWithLimitZeroGoesToTheEnd) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 10);

  EXPECT_EQ(Seen(engine.get(), "", 0), 10u);
  EXPECT_EQ(Seen(engine.get(), Key(3), 0), 7u);
}

TEST_F(BenchEngineTest, ScanFromTheEmptyKeyStartsAtTheFirstKey) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 10);
  EXPECT_EQ(Seen(engine.get(), "", 3), 3u);
}

TEST_F(BenchEngineTest, ScanFromBetweenKeysStartsAtTheNextOne) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 10);
  // "<key 3>." sorts after key 3 and before key 4, as the keys of readmissing do.
  EXPECT_EQ(Seen(engine.get(), Key(3) + ".", 0), 6u);
}

TEST_F(BenchEngineTest, ScanPastTheLastKeyVisitsNothingAndSucceeds) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 10);
  EXPECT_EQ(Seen(engine.get(), Key(10), 0), 0u);
  EXPECT_EQ(Seen(engine.get(), Key(10), 5), 0u);
}

TEST_F(BenchEngineTest, ScanOfAnEmptyDatabaseVisitsNothing) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  EXPECT_EQ(Seen(engine.get(), "", 0), 0u);
}

// fillsync is only a sync benchmark if its writes are synced, and every other write benchmark only
// an unsynced one if they are not. A Sync() that fails shows which is which: it fails the write
// that asked for one, and cannot touch the write that did not.
TEST_F(BenchEngineTest, TheSyncFlagDecidesWhetherTheLogIsSynced) {
  FaultInjectionEnv faulty(1);
  options_.env = &faulty;
  const std::unique_ptr<BenchEngine> engine = OpenFresh();

  faulty.SetFailureProbability(FaultInjectionEnv::Op::kSync, 1.0);
  EXPECT_OK(engine->Put(Key(1), "unsynced", /*sync=*/false));
  const Status synced = engine->Put(Key(2), "synced", /*sync=*/true);
  EXPECT_FALSE(synced.ok()) << "a write that asked for a sync must have gone through the Sync()";
  EXPECT_GT(faulty.injected_errors(), 0u);
}

TEST_F(BenchEngineTest, CompactAllDoesTheWorkOfACompactionAndKeepsEveryKey) {
  // Small tables, so that the writes make several files and the compaction has work to do. The
  // scheduler runs a task only when the database waits for one, so that nothing happens in the
  // background between the two readings of the write amplification below.
  ManualScheduler scheduler;
  options_.scheduler = &scheduler;
  options_.write_buffer_size = 16384;
  options_.target_file_size = 16384;
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  constexpr int kKeys = 2000;
  for (int i = 0; i < kKeys; i++) {
    ASSERT_OK(engine->Put(Key(i), std::string(100, static_cast<char>('a' + i % 26)), false));
  }

  // A compaction writes: what is left in the memtable becomes a table, and the tables are merged.
  // An engine whose CompactAll() returned at once would leave the figure where it was.
  const std::string before = engine->WriteAmplification();
  ASSERT_OK(engine->CompactAll());
  const std::string after = engine->WriteAmplification();
  EXPECT_GT(std::stod(after), std::stod(before))
      << "write amplification " << before << " and then " << after;

  EXPECT_EQ(Seen(engine.get(), "", 0), static_cast<size_t>(kKeys));
  std::string value;
  for (int i = 0; i < kKeys; i += 97) {
    ASSERT_OK(engine->Get(Key(i), &value));
    EXPECT_EQ(value, std::string(100, static_cast<char>('a' + i % 26)));
  }
}

TEST_F(BenchEngineTest, WriteAmplificationIsTheDatabasesOwnFigure) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  EXPECT_EQ(engine->WriteAmplification(), "0.00") << "nothing is written yet";

  PutKeys(engine.get(), 100);
  const std::string figure = engine->WriteAmplification();
  ASSERT_NE(figure, "n/a") << "the engine reports it";
  // The log alone holds every byte that the callers wrote and the headers of its records.
  EXPECT_GE(std::stod(figure), 1.0) << figure;
}

TEST_F(BenchEngineTest, CallsOnAnEngineThatIsNotOpenFailInsteadOfCrashing) {
  const std::unique_ptr<BenchEngine> engine = NewShaleBenchEngine(options_);
  std::string value;
  size_t seen = 0;
  EngineState state;
  EXPECT_FALSE(engine->Put("k", "v", false).ok());
  EXPECT_FALSE(engine->Get("k", &value).ok());
  EXPECT_FALSE(engine->Scan("", 0, &seen).ok());
  EXPECT_FALSE(engine->CompactAll().ok());
  EXPECT_FALSE(engine->GetState(&state).ok());
  EXPECT_EQ(engine->WriteAmplification(), "n/a");
  engine->Close();  // closing what is not open is allowed
}

// ---- What the engine says of its tables and of its background work. ----------------------------

// The value of the counter `name` in `state`, which must have it.
uint64_t Counter(const EngineState& state, const std::string& name) {
  for (const auto& [counter, value] : state.counters) {
    if (counter == name) {
      return value;
    }
  }
  ADD_FAILURE() << "no counter " << name;
  return 0;
}

TEST_F(BenchEngineTest, TheStateHasEveryLevelTheCountersInTheirOrderAndTheWriteAmplification) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  EngineState state;
  ASSERT_OK(engine->GetState(&state));
  EXPECT_EQ(state.files_per_level, std::vector<uint64_t>(7, 0)) << "seven levels, all empty";
  EXPECT_EQ(state.bytes_per_level, std::vector<uint64_t>(7, 0));
  EXPECT_FALSE(state.background_busy);
  EXPECT_EQ(state.highest_score, 0);
  std::vector<std::string> names;
  for (const auto& [name, value] : state.counters) {
    names.push_back(name);
  }
  // The order of the property "shale.counters", which the JSON keeps.
  EXPECT_EQ(names,
            (std::vector<std::string>{
                "flushes", "compactions", "trivial_moves", "tombstones_dropped", "shadowed_dropped",
                "user_bytes_written", "wal_bytes_written", "flush_bytes_written",
                "compaction_bytes_read", "compaction_bytes_written", "write_groups", "wal_syncs",
                "write_slowdowns", "write_stops", "bg_errors"}));
  EXPECT_EQ(state.write_amplification, "0.00");

  PutKeys(engine.get(), 100);
  ASSERT_OK(engine->GetState(&state));
  EXPECT_EQ(Counter(state, "user_bytes_written"), 100u * 16 + 10 * 2 + 90 * 3)
      << "the key and value bytes of the puts: 16-digit keys, and the values v0 to v99";
  EXPECT_EQ(state.write_amplification, engine->WriteAmplification());
}

// The flush that a full memtable queues is background work until it has run: a scheduler that runs
// a task only when it is told to shows the work queued, and then done.
TEST_F(BenchEngineTest, QueuedWorkIsBusyAndWorkThatHasRunIsNot) {
  ManualScheduler scheduler;
  options_.scheduler = &scheduler;
  options_.write_buffer_size = 16384;
  const std::unique_ptr<BenchEngine> engine = OpenFresh();

  EngineState state;
  for (int i = 0; i < 10000 && !state.background_busy; i++) {
    ASSERT_OK(engine->Put(Key(i), std::string(100, 'v'), /*sync=*/false));
    ASSERT_OK(engine->GetState(&state));
  }
  ASSERT_TRUE(state.background_busy) << "a full memtable queues its flush";
  EXPECT_EQ(state.files_per_level[0], 0u) << "which nothing has run yet";

  EXPECT_GT(scheduler.RunPending(), 0u);
  ASSERT_OK(engine->GetState(&state));
  EXPECT_FALSE(state.background_busy);
  EXPECT_EQ(state.files_per_level[0], 1u);
  EXPECT_GT(state.bytes_per_level[0], 0u);
  EXPECT_EQ(Counter(state, "flushes"), 1u);
}

// Level 0 is due for compaction at four files. With the fourth flush done and its compaction
// queued, the state says so: busy, and a level at a score of 1. Once the compaction has run,
// neither.
TEST_F(BenchEngineTest, ALevelThatIsDueScoresOneOrMoreUntilItsCompactionHasRun) {
  ManualScheduler scheduler;
  options_.scheduler = &scheduler;
  options_.write_buffer_size = 16384;
  const std::unique_ptr<BenchEngine> engine = OpenFresh();

  // Each flush is run as soon as it is queued, and alone: RunOne() leaves queued what the task
  // queues, which after the fourth is the compaction. The keys are spread over the same range in
  // every memtable, so that the four tables overlap and the compaction merges them, where tables
  // of keys in order would each be moved down alone.
  EngineState state;
  ASSERT_OK(engine->GetState(&state));
  for (int i = 0; i < 40000 && state.files_per_level[0] < 4; i++) {
    ASSERT_OK(engine->Put(Key(i * 7919 % 1000), std::string(100, 'v'), /*sync=*/false));
    ASSERT_OK(engine->GetState(&state));
    if (state.background_busy) {
      ASSERT_TRUE(scheduler.RunOne()) << "the flush";
      ASSERT_OK(engine->GetState(&state));
    }
  }
  ASSERT_EQ(state.files_per_level[0], 4u) << "four flushes";
  EXPECT_TRUE(state.background_busy) << "the fourth flush queued a compaction of level 0";
  EXPECT_EQ(state.highest_score, 1.0) << "four files of a trigger of four";

  EXPECT_GT(scheduler.RunPending(), 0u);
  ASSERT_OK(engine->GetState(&state));
  EXPECT_FALSE(state.background_busy);
  EXPECT_EQ(state.files_per_level[0], 0u);
  EXPECT_LT(state.highest_score, 1.0);
  EXPECT_EQ(Counter(state, "compactions"), 1u);
}

// A database opened without a scheduler of the options' runs its background work on a thread of
// its own, as shale's default does. Settle() waits for it until it has nothing left to do, and
// nothing is queued again while nothing is written.
TEST_F(BenchEngineTest, TheBackgroundOfADatabaseWithAThreadOfItsOwnSettles) {
  options_.write_buffer_size = 16384;
  options_.target_file_size = 16384;
  options_.max_bytes_for_level_base = 65536;
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  for (int i = 0; i < 20000; i++) {
    ASSERT_OK(engine->Put(Key(i * 7919 % 20000), std::string(100, 'v'), /*sync=*/false));
  }

  SettleOptions quick;
  quick.poll = std::chrono::milliseconds(1);
  SettleResult settled;
  ASSERT_OK(Settle(engine.get(), quick, &settled));
  EXPECT_TRUE(settled.state.Idle());
  EXPECT_LT(settled.state.files_per_level[0], 4u);
  EXPECT_GT(Counter(settled.state, "flushes"), 10u);
  EXPECT_GT(Counter(settled.state, "compactions"), 0u);

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EngineState later;
  ASSERT_OK(engine->GetState(&later));
  EXPECT_FALSE(later.background_busy);
  EXPECT_EQ(later.counters, settled.state.counters) << "nothing ran after it was idle";
  EXPECT_EQ(later.files_per_level, settled.state.files_per_level);
}

// The score is the rule that shale's picker applies, which the engine does not report: level 0 is
// its files over the trigger, and the smaller of the two triggers that it has, each level below
// it its bytes over its limit, and the last level, which has nowhere to go, is never due.
TEST(ShaleHighestScoreTest, IsTheRuleOfThePicker) {
  Options options;  // a trigger of 4 files, a stop at 12, 10 MiB for level 1, ten times each level
  std::vector<uint64_t> files(7, 0);
  std::vector<uint64_t> bytes(7, 0);
  EXPECT_EQ(ShaleHighestScore(files, bytes, options), 0);

  files[0] = 3;
  EXPECT_DOUBLE_EQ(ShaleHighestScore(files, bytes, options), 0.75);
  bytes[1] = uint64_t{15} << 20;
  EXPECT_DOUBLE_EQ(ShaleHighestScore(files, bytes, options), 1.5) << "15 MiB of level 1's 10";
  bytes[2] = uint64_t{250} << 20;
  EXPECT_DOUBLE_EQ(ShaleHighestScore(files, bytes, options), 2.5) << "250 MiB of level 2's 100";
  bytes[5] = uint64_t{50000} << 20;
  EXPECT_DOUBLE_EQ(ShaleHighestScore(files, bytes, options), 2.5)
      << "50000 MiB of level 5's 100000 MiB is half, so level 2 still leads";
  bytes[6] = uint64_t{1} << 50;
  EXPECT_DOUBLE_EQ(ShaleHighestScore(files, bytes, options), 2.5) << "the last level is never due";

  bytes[5] = 0;
  options.max_bytes_for_level_base = uint64_t{20} << 20;
  options.level_size_multiplier = 5;
  EXPECT_DOUBLE_EQ(ShaleHighestScore(files, bytes, options), 2.5) << "250 MiB of level 2's 100";
  bytes[2] = 0;
  EXPECT_DOUBLE_EQ(ShaleHighestScore(files, bytes, options), 0.75) << "15 MiB of level 1's 20";

  options.l0_stop_trigger = 2;
  EXPECT_DOUBLE_EQ(ShaleHighestScore(files, bytes, options), 1.5)
      << "level 0 is due at the smaller trigger: 3 files of 2";
}

}  // namespace
}  // namespace shale
