// The adapter that shale_bench drives the database through. The workloads are written against
// BenchEngine alone, so what a call means here is what it will have to mean for any other engine
// that is measured with them: these tests pin the meaning that the comparison depends on, which is
// that a Scan visits what it says it visits and that a write is synced exactly when it is asked to
// be.

#include "bench_engine.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

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
  EXPECT_FALSE(engine->Put("k", "v", false).ok());
  EXPECT_FALSE(engine->Get("k", &value).ok());
  EXPECT_FALSE(engine->Scan("", 0, &seen).ok());
  EXPECT_FALSE(engine->CompactAll().ok());
  EXPECT_EQ(engine->WriteAmplification(), "n/a");
  engine->Close();  // closing what is not open is allowed
}

}  // namespace
}  // namespace shale
