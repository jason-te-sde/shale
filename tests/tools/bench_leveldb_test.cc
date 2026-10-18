// The LevelDB engine that shale_bench measures shale against. It exists only in a build that found
// LevelDB, and this test with it: CI never runs it. What it pins is what makes the comparison fair,
// as bench_engine_test.cc pins it for shale: the same meaning for every call of BenchEngine, a
// write that is synced exactly when it is asked to be, and the settings that the two engines are
// held to, as far as they can be seen from outside the engine.

#include "bench_leveldb.h"

#include <gtest/gtest.h>
#include <leveldb/env.h>
#include <stdlib.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "bench_engine.h"
#include "counting_leveldb_env.h"
#include "shale/status.h"
#include "testkit/test_util.h"

namespace shale {
namespace {

namespace fs = std::filesystem;

std::string Key(int index) {
  char text[32];
  std::snprintf(text, sizeof(text), "%016d", index);
  return text;
}

class BenchLevelDBTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* tmpdir = ::getenv("TMPDIR");
    std::string pattern = std::string(tmpdir != nullptr && *tmpdir != '\0' ? tmpdir : "/tmp") +
                          "/bench_leveldb_test.XXXXXX";
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    root_ = pattern;
    path_ = root_ + "/db";
  }

  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  std::unique_ptr<BenchEngine> OpenFresh(leveldb::Env* env = nullptr) {
    std::unique_ptr<BenchEngine> engine = NewLevelDBBenchEngine(env);
    EXPECT_OK(engine->Open(path_, /*fresh=*/true));
    return engine;
  }

  static void PutKeys(BenchEngine* engine, int count) {
    for (int i = 0; i < count; i++) {
      ASSERT_OK(engine->Put(Key(i), "v" + std::to_string(i), /*sync=*/false));
    }
  }

  static size_t Seen(BenchEngine* engine, const std::string& from, size_t limit) {
    size_t seen = 12345;
    EXPECT_OK(engine->Scan(from, limit, &seen));
    return seen;
  }

  // The table files of the database, which LevelDB 1.23 names "<number>.ldb".
  std::vector<fs::path> Tables() const {
    std::vector<fs::path> tables;
    for (const fs::directory_entry& entry : fs::directory_iterator(path_)) {
      if (entry.path().extension() == ".ldb") {
        tables.push_back(entry.path());
      }
    }
    return tables;
  }

  static std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  std::string root_;
  std::string path_;
};

TEST_F(BenchLevelDBTest, NamesTheEngine) { EXPECT_EQ(OpenFresh()->Name(), "leveldb"); }

TEST_F(BenchLevelDBTest, PutThenGetReturnsTheValueAndAMissingKeyIsNotFound) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  ASSERT_OK(engine->Put(Key(7), "seven", /*sync=*/false));

  std::string value;
  ASSERT_OK(engine->Get(Key(7), &value));
  EXPECT_EQ(value, "seven");

  const Status missing = engine->Get(Key(8), &value);
  EXPECT_TRUE(missing.IsNotFound()) << missing.ToString();
}

TEST_F(BenchLevelDBTest, OpenFreshDiscardsWhatWasThereAndOpenNotFreshKeepsIt) {
  std::unique_ptr<BenchEngine> engine = OpenFresh();
  ASSERT_OK(engine->Put(Key(1), "one", /*sync=*/false));
  engine->Close();

  ASSERT_OK(engine->Open(path_, /*fresh=*/false));
  std::string value;
  ASSERT_OK(engine->Get(Key(1), &value)) << "a database opened as it was must still hold its key";
  EXPECT_EQ(value, "one");

  ASSERT_OK(engine->Open(path_, /*fresh=*/true));  // replaces the open database
  const Status gone = engine->Get(Key(1), &value);
  EXPECT_TRUE(gone.IsNotFound()) << "a fresh database must start empty: " << gone.ToString();
}

TEST_F(BenchLevelDBTest, ScanVisitsFromAKeyUpToTheLimit) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 10);

  EXPECT_EQ(Seen(engine.get(), Key(3), 4), 4u);
  EXPECT_EQ(Seen(engine.get(), Key(8), 4), 2u) << "only keys 8 and 9 are at or after key 8";
  EXPECT_EQ(Seen(engine.get(), Key(3), 1), 1u);
}

TEST_F(BenchLevelDBTest, ScanWithLimitZeroGoesToTheEnd) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 10);

  EXPECT_EQ(Seen(engine.get(), "", 0), 10u);
  EXPECT_EQ(Seen(engine.get(), Key(3), 0), 7u);
}

TEST_F(BenchLevelDBTest, ScanFromTheEmptyKeyStartsAtTheFirstKey) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 10);
  EXPECT_EQ(Seen(engine.get(), "", 3), 3u);
}

TEST_F(BenchLevelDBTest, ScanFromBetweenKeysStartsAtTheNextOne) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 10);
  EXPECT_EQ(Seen(engine.get(), Key(3) + ".", 0), 6u);
}

TEST_F(BenchLevelDBTest, ScanPastTheLastKeyVisitsNothingAndSucceeds) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 10);
  EXPECT_EQ(Seen(engine.get(), Key(10), 0), 0u);
  EXPECT_EQ(Seen(engine.get(), Key(10), 5), 0u);
}

TEST_F(BenchLevelDBTest, ScanOfAnEmptyDatabaseVisitsNothing) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  EXPECT_EQ(Seen(engine.get(), "", 0), 0u);
}

// fillsync is only a sync benchmark if its writes are synced, and every other write benchmark only
// an unsynced one if they are not. The log is the file that a write is synced through, so its syncs
// are counted: one for the write that asked, none for the write that did not.
TEST_F(BenchLevelDBTest, TheSyncFlagDecidesWhetherTheLogIsSynced) {
  CountingLevelDBEnv env;
  const std::unique_ptr<BenchEngine> engine = OpenFresh(&env);

  const int before = env.log_syncs();
  ASSERT_OK(engine->Put(Key(1), "unsynced", /*sync=*/false));
  EXPECT_EQ(env.log_syncs(), before) << "a write that did not ask for a sync must not sync the log";
  ASSERT_OK(engine->Put(Key(2), "synced", /*sync=*/true));
  EXPECT_EQ(env.log_syncs(), before + 1) << "a write that asked for a sync syncs the log once";
  ASSERT_OK(engine->Put(Key(3), "synced again", /*sync=*/true));
  EXPECT_EQ(env.log_syncs(), before + 2);
}

// The adapter reports no write amplification for LevelDB. Its statistics are a table of the bytes
// each level read and wrote, not the ratio that shale's property is, so the comparison shows the
// figure for shale alone, and a figure of the adapter's own making would not be LevelDB's.
TEST_F(BenchLevelDBTest, WriteAmplificationIsNotAvailable) {
  const std::unique_ptr<BenchEngine> engine = NewLevelDBBenchEngine();
  EXPECT_EQ(engine->WriteAmplification(), "n/a") << "no database is open";
  ASSERT_OK(engine->Open(path_, /*fresh=*/true));
  PutKeys(engine.get(), 100);
  EXPECT_EQ(engine->WriteAmplification(), "n/a");
}

TEST_F(BenchLevelDBTest, CompactAllReturnsWithEveryKeyStillThere) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  constexpr int kKeys = 3000;
  for (int i = 0; i < kKeys; i++) {
    ASSERT_OK(engine->Put(Key(i), std::string(100, static_cast<char>('a' + i % 26)), false));
  }
  ASSERT_OK(engine->CompactAll());

  EXPECT_FALSE(Tables().empty()) << "a compaction leaves the writes in tables, not in the log";
  EXPECT_EQ(Seen(engine.get(), "", 0), static_cast<size_t>(kKeys));
  std::string value;
  for (int i = 0; i < kKeys; i += 97) {
    ASSERT_OK(engine->Get(Key(i), &value));
    EXPECT_EQ(value, std::string(100, static_cast<char>('a' + i % 26)));
  }
}

TEST_F(BenchLevelDBTest, CallsOnAnEngineThatIsNotOpenFailInsteadOfCrashing) {
  const std::unique_ptr<BenchEngine> engine = NewLevelDBBenchEngine();
  std::string value;
  size_t seen = 0;
  EXPECT_FALSE(engine->Put("k", "v", false).ok());
  EXPECT_FALSE(engine->Get("k", &value).ok());
  EXPECT_FALSE(engine->Scan("", 0, &seen).ok());
  EXPECT_FALSE(engine->CompactAll().ok());
  engine->Close();  // closing what is not open is allowed
}

// ---- The settings the engines are held to, as far as a file shows them.
// ---------------------------

// Compression is off: a value that Snappy would make a few bytes of takes its whole size in the
// table. shale writes no compressed blocks, so a LevelDB that compressed would measure less I/O.
TEST_F(BenchLevelDBTest, TablesAreNotCompressed) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  constexpr size_t kValueSize = 100 * 1024;
  constexpr int kKeys = 40;
  for (int i = 0; i < kKeys; i++) {
    ASSERT_OK(engine->Put(Key(i), std::string(kValueSize, 'a'), /*sync=*/false));
  }
  ASSERT_OK(engine->CompactAll());

  uintmax_t table_bytes = 0;
  for (const fs::path& table : Tables()) {
    table_bytes += fs::file_size(table);
  }
  EXPECT_GE(table_bytes, kKeys * kValueSize) << "the values were written as they are";
}

// The filter policy is a Bloom filter, which a table says by naming it in its meta index.
TEST_F(BenchLevelDBTest, TablesCarryABloomFilter) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  PutKeys(engine.get(), 1000);
  ASSERT_OK(engine->CompactAll());

  const std::vector<fs::path> tables = Tables();
  ASSERT_FALSE(tables.empty());
  for (const fs::path& table : tables) {
    EXPECT_NE(ReadAll(table).find("filter.leveldb.BuiltinBloomFilter2"), std::string::npos)
        << table;
  }
}

// The write buffer is 4 MiB: a database that has been given less than that holds all of it in the
// memtable and the log, with no table, and one that has been given more has written a table out.
TEST_F(BenchLevelDBTest, TheMemtableIsWrittenOutAtFourMebibytes) {
  const std::unique_ptr<BenchEngine> engine = OpenFresh();
  constexpr size_t kValueSize = 1024;
  const std::string value(kValueSize, 'x');
  int next = 0;
  // 3 MiB, which is under the buffer. The value repeats, which is not a problem with no
  // compression: the buffer is counted in the bytes that were written, not the ones that differ.
  for (; next < 3 * 1024; next++) {
    ASSERT_OK(engine->Put(Key(next), value, /*sync=*/false));
  }
  EXPECT_TRUE(Tables().empty()) << "3 MiB is not enough to write a table out";

  // 3 MiB more, which is over it. The table is written by a thread of the database, so wait for it.
  for (; next < 6 * 1024; next++) {
    ASSERT_OK(engine->Put(Key(next), value, /*sync=*/false));
  }
  for (int waited_ms = 0; Tables().empty() && waited_ms < 10000; waited_ms += 10) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_FALSE(Tables().empty()) << "6 MiB must have made the memtable full, at least once";
}

}  // namespace
}  // namespace shale
