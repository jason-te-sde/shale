// Leveled compaction in the open database: when it runs and in what order against the flush and
// a caller's request, which files move whole and which are rewritten, what a snapshot keeps, and
// what a failure anywhere in it costs.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/db_impl.h"
#include "db/db_test_util.h"
#include "db/hook_env.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "io/table.h"
#include "shale/db.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/filename.h"

namespace shale {
namespace {

constexpr char kDb[] = "/db";

using Op = FaultInjectionEnv::Op;

// One entry of a table, as the table holds it.
struct InternalEntry {
  std::string user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;
};

class DBCompactionTest : public DBTestBase {
 protected:
  // The options of the tests that want a compaction of many outputs: a buffer that a few hundred
  // kilobytes do not fill, so that a table is made only when the test flushes, and outputs of the
  // smallest size there is.
  Options ManyOutputsOptions() {
    Options options = CurrentOptions();
    options.write_buffer_size = 262144;
    options.target_file_size = 16384;
    return options;
  }

  // Writes keys `round`, `round + 4`, ... below 160, a kilobyte each, and flushes them. The four
  // tables of rounds 0 to 3 span the same keys, 40 of the 160 in each, so all four are the inputs
  // of one compaction, and their 160 KiB make it ten outputs.
  void WriteSpanningTable(int round) {
    for (int i = round; i < 160; i += 4) {
      ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
    }
    ASSERT_OK(db_->Flush());
  }

  // Writes "w" keys until the memtable switches, so that one is waiting to be flushed, and returns
  // how many. For a hook that runs while a compaction does, which cannot wait for the flush.
  int FillTheMemtable(const std::string& prefix) {
    int written = 0;
    while (CountFiles(FileType::kLogFile) < 2) {
      // A buffer of 256 KiB is full after some 260 entries of a kilobyte.
      EXPECT_LT(written, 1000) << "the memtable never switched";
      if (written >= 1000) {
        break;
      }
      EXPECT_OK(Put(prefix + UserKey(written), KiB(written), /*sync=*/false));
      written++;
    }
    return written;
  }

  // Writes `count` entries, UserKey(first + i) with the value `tag` and its number, and flushes
  // them into a table of their own.
  void WriteTable(int first, int count, const std::string& tag) {
    for (int i = first; i < first + count; i++) {
      ASSERT_OK(Put(UserKey(i), tag + ValueOf(i), /*sync=*/false));
    }
    ASSERT_OK(db_->Flush());
  }

  // The files of `level`, as the current version has them.
  std::vector<FileMetaData> FilesAt(int level) {
    return dbfull()->TEST_VersionState().files[static_cast<size_t>(level)];
  }

  std::set<uint64_t> TableNumbers() {
    std::set<uint64_t> numbers;
    for (const std::vector<FileMetaData>& level : dbfull()->TEST_VersionState().files) {
      for (const FileMetaData& file : level) {
        numbers.insert(file.number);
      }
    }
    return numbers;
  }

  // The level of the deepest file, or 0 if there is none.
  int DeepestLevel() {
    const VersionState state = dbfull()->TEST_VersionState();
    int deepest = 0;
    for (int level = 0; level < kNumLevels; level++) {
      if (!state.files[static_cast<size_t>(level)].empty()) {
        deepest = level;
      }
    }
    return deepest;
  }

  // Every entry of every table of the current version, read from the files and not through the
  // database, so that what a compaction dropped from the tables, as well as from the answers, is
  // seen.
  std::vector<InternalEntry> TableEntries() {
    BlockCache block_cache(size_t{1} << 20);
    TableCache cache(dbname_, env_.get(), &block_cache, 100);
    std::vector<InternalEntry> entries;
    for (const std::vector<FileMetaData>& level : dbfull()->TEST_VersionState().files) {
      for (const FileMetaData& file : level) {
        const std::unique_ptr<Iterator> iter =
            cache.NewIterator(TableReadOptions{}, file.number, file.file_size);
        for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
          ParsedInternalKey parsed;
          EXPECT_TRUE(ParseInternalKey(iter->key(), &parsed));
          entries.push_back({std::string(parsed.user_key), parsed.sequence, parsed.type});
        }
        EXPECT_OK(iter->status());
      }
    }
    return entries;
  }

  int EntriesFor(const std::string& user_key) {
    int count = 0;
    for (const InternalEntry& entry : TableEntries()) {
      if (entry.user_key == user_key) {
        count++;
      }
    }
    return count;
  }

  // The tables in the directory are exactly the ones the version names.
  void ExpectNoStrayTables() {
    const std::set<uint64_t> live = TableNumbers();
    for (const std::string& name : FilesOfType(FileType::kTableFile)) {
      uint64_t number = 0;
      FileType type = FileType::kCurrentFile;
      ASSERT_TRUE(ParseFileName(name, &number, &type));
      EXPECT_EQ(live.count(number), 1u) << name << " is in no version";
    }
    for (const uint64_t number : live) {
      EXPECT_TRUE(env_->FileExists(TableFileName(kDb, number))) << "table " << number << " is gone";
    }
  }
};

TEST_F(DBCompactionTest, Level0CompactionRunsAtTrigger) {
  Options options = CurrentOptions();
  options.l0_compaction_trigger = 4;
  Reopen(options);
  for (int round = 0; round < 4; round++) {
    ASSERT_NO_FATAL_FAILURE(WriteTable(0, 100, "r" + std::to_string(round) + "-"));
    if (round < 3) {
      EXPECT_EQ(NumTableFilesAtLevel(0), round + 1) << "a compaction ran below the trigger";
    }
  }
  scheduler_.RunPending();
  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  EXPECT_GE(NumTableFilesAtLevel(1), 1);
  for (int i = 0; i < 100; i++) {
    EXPECT_EQ(Get(UserKey(i)), "r3-" + ValueOf(i));
  }
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(counters.compactions, 1u);
  EXPECT_EQ(counters.trivial_moves, 0u);
  EXPECT_EQ(counters.shadowed_dropped, 300u) << "three older versions of each of 100 keys";
  EXPECT_EQ(counters.tombstones_dropped, 0u);
  EXPECT_GT(counters.compaction_bytes_read, 0u);
  EXPECT_EQ(counters.compaction_bytes_written, TotalFileSize(FilesAt(1)));
  ExpectNoStrayTables();
}

TEST_F(DBCompactionTest, SingleFileIsTriviallyMoved) {
  Options options = CurrentOptions();
  options.l0_compaction_trigger = 1;
  Reopen(options);
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  scheduler_.RunPending();

  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  const std::vector<FileMetaData> level1 = FilesAt(1);
  ASSERT_EQ(level1.size(), 1u);
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(counters.flushes, 1u);
  EXPECT_EQ(counters.trivial_moves, 1u);
  EXPECT_EQ(counters.compactions, 0u);
  EXPECT_EQ(counters.compaction_bytes_read, 0u) << "a moved file is not read";
  EXPECT_EQ(counters.compaction_bytes_written, 0u) << "and is not written again";
  EXPECT_EQ(level1[0].file_size, counters.flush_bytes_written) << "the file is the flushed one";
  EXPECT_EQ(CountFiles(FileType::kTableFile), 1) << "a moved file is not copied";
  EXPECT_EQ(dbfull()->TEST_VersionState().compact_pointers[0], level1[0].largest);
  EXPECT_EQ(Get("a"), "1");
}

TEST_F(DBCompactionTest, ATrivialMoveDoesNotGoThroughTheFilter) {
  // A table that holds a deletion and the value it deletes. A compaction would drop both, and a
  // move must not touch either: the file is moved unread.
  Options options = CurrentOptions();
  options.l0_compaction_trigger = 1;
  Reopen(options);
  ASSERT_OK(Put("k", "v"));
  ASSERT_OK(Delete("k"));
  ASSERT_OK(db_->Flush());
  scheduler_.RunPending();

  ASSERT_EQ(NumTableFilesAtLevel(1), 1);
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(counters.trivial_moves, 1u);
  EXPECT_EQ(counters.tombstones_dropped, 0u);
  EXPECT_EQ(counters.shadowed_dropped, 0u);
  EXPECT_EQ(EntriesFor("k"), 2) << "the move rewrote the file";
}

TEST_F(DBCompactionTest, ManualCompactionIsNeverATrivialMove) {
  // The same single file, on a database that would move it by itself: asked for, it is rewritten.
  ASSERT_OK(Put("k", "v"));
  ASSERT_OK(db_->Flush());
  const std::vector<FileMetaData> before = FilesAt(0);
  ASSERT_EQ(before.size(), 1u);
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));

  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  const std::vector<FileMetaData> after = FilesAt(1);
  ASSERT_EQ(after.size(), 1u);
  EXPECT_NE(after[0].number, before[0].number) << "the file was moved, not rewritten";
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(counters.compactions, 1u);
  EXPECT_EQ(counters.trivial_moves, 0u);
  EXPECT_FALSE(env_->FileExists(TableFileName(kDb, before[0].number)))
      << "the input was not deleted";
  EXPECT_EQ(Get("k"), "v");
}

TEST_F(DBCompactionTest, CompactRangeAllPushesDataDown) {
  // Small levels, so that the writes spread over several of them.
  Options options = SmallBuffer();
  options.max_bytes_for_level_base = 65536;
  options.level_size_multiplier = 2;
  Reopen(options);
  std::vector<std::string> expected(1000);
  for (int i = 0; i < 3000; i++) {
    const int key = (i * 7) % 1000;
    expected[static_cast<size_t>(key)] = "v" + std::to_string(i) + std::string(100, 'x');
    ASSERT_OK(Put(UserKey(key), expected[static_cast<size_t>(key)], /*sync=*/false));
  }
  // What a scan returns, which is what was written and which no compaction may change.
  std::string expected_contents;
  for (int key = 0; key < 1000; key++) {
    if (key > 0) {
      expected_contents.push_back(',');
    }
    expected_contents.append(UserKey(key)).append("=").append(expected[static_cast<size_t>(key)]);
  }
  ASSERT_EQ(Contents(), expected_contents) << "the scan before the compaction";
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  EXPECT_GE(DeepestLevel(), 1);
  EXPECT_EQ(Contents(), expected_contents) << "a compaction must not change what a scan returns";
  for (int key = 0; key < 1000; key++) {
    // Every key was written: i * 7 mod 1000 reaches each of the 1000 keys, three times over.
    EXPECT_EQ(Get(UserKey(key)), expected[static_cast<size_t>(key)]) << "key " << key;
  }
  EXPECT_OK(CheckInvariants(dbfull()->TEST_VersionState()));
  ExpectNoStrayTables();
  Reopen(options);
  for (int key = 0; key < 1000; key++) {
    EXPECT_EQ(Get(UserKey(key)), expected[static_cast<size_t>(key)]) << "key " << key;
  }
}

TEST_F(DBCompactionTest, CompactRangeFlushesTheMemtableFirst) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  EXPECT_EQ(NumTableFilesAtLevel(1), 1);
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "2");
}

TEST_F(DBCompactionTest, CompactRangeOnAnEmptyDatabaseDoesNothing) {
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  ASSERT_OK(db_->CompactRange("a", "b"));
  ASSERT_OK(db_->CompactRange("b", "a"));  // a range that ends before it starts
  EXPECT_EQ(CountFiles(FileType::kTableFile), 0);
  EXPECT_EQ(dbfull()->TEST_Counters().compactions, 0u);
  ASSERT_OK(Put("c", "1"));
  EXPECT_EQ(Get("c"), "1");
}

TEST_F(DBCompactionTest, CompactRangeTouchesOnlyTheFilesInTheRange) {
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 100, "low-"));
  ASSERT_NO_FATAL_FAILURE(WriteTable(500, 100, "high-"));
  const std::vector<FileMetaData> before = FilesAt(0);
  ASSERT_EQ(before.size(), 2u);
  ASSERT_OK(db_->CompactRange(UserKey(550), UserKey(560)));

  const std::vector<FileMetaData> level0 = FilesAt(0);
  ASSERT_EQ(level0.size(), 1u);
  EXPECT_EQ(level0[0].number, before[0].number) << "the file outside the range was compacted";
  EXPECT_EQ(NumTableFilesAtLevel(1), 1);
  EXPECT_EQ(Get(UserKey(10)), "low-" + ValueOf(10));
  EXPECT_EQ(Get(UserKey(550)), "high-" + ValueOf(550));
}

TEST_F(DBCompactionTest, OutputsRespectTargetSize) {
  Options options = SmallBuffer();
  options.target_file_size = 16384;
  Reopen(options);
  for (int i = 0; i < 1024; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
  }
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  const int deepest = DeepestLevel();
  EXPECT_GE(NumTableFilesAtLevel(deepest), 10);
  for (const FileMetaData& file : FilesAt(deepest)) {
    EXPECT_LT(file.file_size, 3 * options.target_file_size) << "file " << file.number;
  }
  EXPECT_OK(CheckInvariants(dbfull()->TEST_VersionState()));
  for (int i = 0; i < 1024; i++) {
    ASSERT_EQ(Get(UserKey(i)), KiB(i)) << "entry " << i;
  }
}

TEST_F(DBCompactionTest, TombstonesAreDroppedAtTheBase) {
  ASSERT_OK(Put("other", "1"));
  ASSERT_OK(Put("k", "v"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Delete("k"));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));

  EXPECT_GE(dbfull()->TEST_Counters().tombstones_dropped, 1u);
  EXPECT_EQ(Get("k"), "NOT_FOUND");
  EXPECT_EQ(Get("other"), "1");
  EXPECT_EQ(EntriesFor("k"), 0) << "a table still holds the key, or its deletion";
  EXPECT_EQ(EntriesFor("other"), 1);
}

TEST_F(DBCompactionTest, ATombstoneIsKeptWhileALevelBelowHoldsTheKey) {
  // The value is pushed down to level 2 by moves, which no filter sees. A deletion written after
  // it is compacted into level 1 first, over a level 2 that still holds the value, so it must
  // survive that step, or "a-special" would be back; the step after it, into the last level, is
  // the one that may drop both.
  Options options = CurrentOptions();
  options.l0_compaction_trigger = 1;
  options.max_bytes_for_level_base = 65536;
  Reopen(options);
  ASSERT_OK(Put("a-special", "v"));
  ASSERT_OK(db_->Flush());
  for (int table = 0; table < 8; table++) {
    for (int i = 0; i < 10; i++) {
      ASSERT_OK(Put(UserKey(table * 10 + i), KiB(i), /*sync=*/false));
    }
    ASSERT_OK(db_->Flush());
  }
  ASSERT_EQ(dbfull()->TEST_Counters().compactions, 0u) << "only moves were expected";
  ASSERT_EQ(DeepestLevel(), 2);
  const std::vector<FileMetaData> level2 = FilesAt(2);
  ASSERT_FALSE(level2.empty());
  ASSERT_EQ(ExtractUserKey(level2[0].smallest), "a-special") << "the value is not at level 2";

  options.l0_compaction_trigger = 4;  // so that the deletion waits at level 0 for the request
  Reopen(options);
  ASSERT_OK(Delete("a-special"));
  ASSERT_OK(db_->CompactRange("a-special", "a-special"));
  EXPECT_EQ(Get("a-special"), "NOT_FOUND") << "the value came back";
  EXPECT_EQ(EntriesFor("a-special"), 0);
  EXPECT_EQ(dbfull()->TEST_Counters().tombstones_dropped, 1u);
  EXPECT_EQ(dbfull()->TEST_Counters().compactions, 2u) << "level 0 into 1, and level 1 into 2";
  EXPECT_EQ(Get(UserKey(0)), KiB(0));
}

TEST_F(DBCompactionTest, CompactionKeepsTheVersionsASnapshotCanSee) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("gone", "x"));
  const Snapshot* snapshot = db_->GetSnapshot();
  // EXPECT, not ASSERT: a snapshot that is not released aborts the close.
  EXPECT_OK(Put("a", "2"));
  EXPECT_OK(Delete("gone"));
  EXPECT_OK(db_->CompactRange(std::nullopt, std::nullopt));

  EXPECT_EQ(Get("a", snapshot), "1");
  EXPECT_EQ(Get("a"), "2");
  EXPECT_EQ(Get("gone", snapshot), "x");
  EXPECT_EQ(Get("gone"), "NOT_FOUND");
  EXPECT_EQ(EntriesFor("a"), 2) << "the version the snapshot sees was dropped";
  EXPECT_EQ(EntriesFor("gone"), 2) << "the deletion, and the value beneath it, must stay";
  EXPECT_EQ(dbfull()->TEST_Counters().tombstones_dropped, 0u);

  // Once the snapshot is gone nothing needs them. The range is now in level 1 alone, and
  // CompactRange() rewrites the deepest populated level only by merging a shallower one into it
  // (see DB::CompactRange()), so a new write over the same range gives it one to merge.
  db_->ReleaseSnapshot(snapshot);
  EXPECT_OK(Put("a", "3"));
  EXPECT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  EXPECT_EQ(EntriesFor("a"), 1);
  EXPECT_EQ(EntriesFor("gone"), 0);
  EXPECT_EQ(Get("a"), "3");
  EXPECT_EQ(Get("gone"), "NOT_FOUND");
}

TEST_F(DBCompactionTest, InputsAreDeletedAfterACompaction) {
  for (int round = 0; round < 4; round++) {
    ASSERT_NO_FATAL_FAILURE(WriteTable(0, 50, "r" + std::to_string(round) + "-"));
  }
  scheduler_.RunPending();
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  EXPECT_EQ(static_cast<size_t>(CountFiles(FileType::kTableFile)), TableNumbers().size())
      << "the inputs of the compaction are still in the directory";
  ExpectNoStrayTables();
}

TEST_F(DBCompactionTest, ATableThatAReaderHoldsIsNotDeletedUnderIt) {
  // A read that began before the compaction holds the version it began with, and with it the
  // tables the compaction replaces. The sweep leaves them until nothing holds them.
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 50, "r0-"));
  const std::set<uint64_t> old_tables = TableNumbers();
  std::shared_ptr<const Version> held = dbfull()->TEST_CurrentVersion();
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  for (const uint64_t number : old_tables) {
    EXPECT_TRUE(env_->FileExists(TableFileName(kDb, number)))
        << "table " << number << " was deleted under a reader";
  }
  held.reset();
  ASSERT_OK(Put("later", "1"));
  ASSERT_OK(db_->Flush());  // the next sweep
  for (const uint64_t number : old_tables) {
    EXPECT_FALSE(env_->FileExists(TableFileName(kDb, number)))
        << "table " << number << " is still there, with nothing holding it";
  }
}

TEST_F(DBCompactionTest, ATableDeletedByACompactionIsEvictedFromTheTableCache) {
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 50, "r0-"));
  const std::vector<FileMetaData> inputs = FilesAt(0);
  ASSERT_EQ(inputs.size(), 1u);
  std::shared_ptr<Table> table;
  ASSERT_OK(dbfull()->TEST_TableCache()->FindTable(inputs[0].number, inputs[0].file_size, &table))
      << "the table the flush built is in the cache";
  table.reset();
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  ASSERT_FALSE(env_->FileExists(TableFileName(kDb, inputs[0].number)));
  // A cache that still held the table would serve it, though its file is gone.
  const Status s =
      dbfull()->TEST_TableCache()->FindTable(inputs[0].number, inputs[0].file_size, &table);
  EXPECT_FALSE(s.ok()) << "the cache still serves a table whose file was deleted";
}

TEST_F(DBCompactionTest, CompactionFailureSetsBackgroundError) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  env_->FailOpAfter(Op::kNewWritable, 0);  // the first file the compaction makes is its output
  const Status s = db_->CompactRange(std::nullopt, std::nullopt);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  const Status put = Put("b", "2");
  EXPECT_TRUE(put.IsIOError()) << put.ToString();
  EXPECT_EQ(put.ToString(), s.ToString()) << "every write returns the first error";
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
  EXPECT_EQ(dbfull()->TEST_Counters().compactions, 0u);
  EXPECT_EQ(CountFiles(FileType::kTableFile), 1) << "a partial output was left";
  EXPECT_EQ(Get("a"), "1");
  EXPECT_TRUE(db_->CompactRange(std::nullopt, std::nullopt).IsIOError())
      << "CompactRange returns the error that stopped writes";
  EXPECT_EQ(scheduler_.pending(), 0u);

  env_->ClearFaults();
  Reopen();
  EXPECT_EQ(Get("a"), "1");
  ASSERT_OK(Put("b", "2"));
}

TEST_F(DBCompactionTest, AFailedEditKeepsTheOutputsThatItMayHaveNamed) {
  // The MANIFEST append of the edit is the second sync of the compaction, after the output's. When
  // it fails the MANIFEST may hold the edit all the same, and then the output is a file that the
  // next open's version names: deleting it would lose the data the inputs held.
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(db_->Flush());
  const std::set<uint64_t> inputs = TableNumbers();
  ASSERT_EQ(inputs.size(), 2u);

  env_->FailOpAfter(Op::kSync, 1);
  const Status s = db_->CompactRange(std::nullopt, std::nullopt);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(env_->injected_errors(), 1u);
  env_->ClearFaults();
  EXPECT_EQ(TableNumbers(), inputs) << "the failed edit was installed";
  const std::vector<std::string> tables = FilesOfType(FileType::kTableFile);
  EXPECT_EQ(tables.size(), 3u) << "the inputs and the output must all still be there";
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "2");

  // A crash, and the next open: whichever version the MANIFEST holds, nothing is lost.
  env_->SimulateProcessCrash();
  Reopen();
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "2");
  ExpectNoStrayTables();
}

TEST_F(DBCompactionTest, ReopenSchedulesCompactionWhenRecoveryAddsTheTableThatMakesItDue) {
  // Two tables, below the trigger of three; the third is what the next open makes of the writes
  // that were only in the log, and nothing but the open can notice that it makes a compaction due.
  Options options = CurrentOptions();
  options.l0_compaction_trigger = 3;
  Reopen(options);
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 10, "a-"));
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 10, "b-"));
  ASSERT_EQ(NumTableFilesAtLevel(0), 2);
  ASSERT_OK(Put(UserKey(3), "unflushed"));
  Reopen(options);
  ASSERT_EQ(NumTableFilesAtLevel(0), 3) << "the open should have flushed the replayed write";
  EXPECT_EQ(scheduler_.pending(), 1u) << "no compaction was scheduled for the third table";
  scheduler_.RunPending();
  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  EXPECT_GE(NumTableFilesAtLevel(1), 1);
  EXPECT_EQ(Get(UserKey(3)), "unflushed");
  EXPECT_EQ(Get(UserKey(4)), "b-" + ValueOf(4));
}

TEST_F(DBCompactionTest, ReopenSchedulesCompactionForTablesThatAreAlreadyDue) {
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 10, "a-"));
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 10, "b-"));
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 10, "c-"));
  ASSERT_EQ(NumTableFilesAtLevel(0), 3) << "three tables are below the default trigger of four";
  Options options = CurrentOptions();
  options.l0_compaction_trigger = 3;
  Reopen(options);
  EXPECT_EQ(scheduler_.pending(), 1u);
  scheduler_.RunPending();
  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  EXPECT_EQ(Get(UserKey(5)), "c-" + ValueOf(5));
}

TEST_F(DBCompactionTest, TheFlushComesBeforeACompactionThatIsAlsoDue) {
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 10, "a-"));
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 10, "b-"));
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 10, "c-"));
  Options options = SmallBuffer();
  options.l0_compaction_trigger = 3;
  Reopen(options);  // three tables are due now, and a task is queued for them
  ASSERT_EQ(scheduler_.pending(), 1u);
  const int written = PutUntilSwitched();  // and a memtable is waiting, with that task still queued
  ASSERT_GT(written, 1);
  ASSERT_EQ(NumTableFilesAtLevel(0), 3);

  ASSERT_TRUE(scheduler_.RunOne());
  EXPECT_EQ(dbfull()->TEST_Counters().flushes, 1u) << "the first task did not write the memtable";
  EXPECT_EQ(dbfull()->TEST_Counters().compactions, 0u) << "a compaction went first";
  EXPECT_EQ(NumTableFilesAtLevel(0), 4);
  ASSERT_EQ(scheduler_.pending(), 1u) << "the compaction that is due was not queued";
  ASSERT_TRUE(scheduler_.RunOne());
  EXPECT_EQ(dbfull()->TEST_Counters().compactions, 1u);
  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  for (int i = 0; i < written; i++) {
    EXPECT_EQ(Get(UserKey(i)), KiB(i));
  }
}

TEST_F(DBCompactionTest, ACallersCompactionComesBeforeOneThatIsDueByItself) {
  // Three tables at level 0: "a" and "b" over one range and "c" over another, so that the
  // compaction the database would pick, which starts from the oldest table, is the move of "a"
  // alone, and the one asked for is the merge of "b" and "c".
  ASSERT_NO_FATAL_FAILURE(WriteTable(0, 10, "a-"));
  ASSERT_NO_FATAL_FAILURE(WriteTable(500, 10, "b-"));
  ASSERT_NO_FATAL_FAILURE(WriteTable(500, 10, "c-"));
  const std::vector<FileMetaData> before = FilesAt(0);
  ASSERT_EQ(before.size(), 3u);
  Options options = CurrentOptions();
  options.l0_compaction_trigger = 3;
  Reopen(options);  // due now, with a task queued
  ASSERT_EQ(scheduler_.pending(), 1u);

  ASSERT_OK(db_->CompactRange(UserKey(500), UserKey(509)));
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(counters.trivial_moves, 0u) << "the automatic compaction went first, and moved a file";
  EXPECT_EQ(counters.compactions, 1u);
  const std::vector<FileMetaData> level0 = FilesAt(0);
  ASSERT_EQ(level0.size(), 1u);
  EXPECT_EQ(level0[0].number, before[0].number);
  EXPECT_EQ(Get(UserKey(505)), "c-" + ValueOf(505));
}

TEST_F(DBCompactionTest, TheMemtableThatIsWaitingIsFlushedBetweenOutputs) {
  Options options = ManyOutputsOptions();
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  Reopen(options);
  for (int round = 0; round < 3; round++) {
    ASSERT_NO_FATAL_FAILURE(WriteSpanningTable(round));
  }

  // The fourth flush makes the compaction due. While it writes its second output, the test fills
  // the memtable, so that a switch leaves one waiting. No task can be scheduled for it: the
  // compaction is the task, and what writes the memtable out is the compaction itself, between two
  // of its outputs.
  int tables_created = 0;
  bool inside = false;
  uint64_t flushes_when_third_output_began = 0;
  uint64_t compactions_when_third_output_began = 0;
  int written = 0;
  hook_env->SetHook([&](std::string_view call, const std::string& path) {
    if (call != "new_writable" || !path.ends_with(".sst") || inside) {
      return Status::OK();
    }
    tables_created++;
    // 1 is the flush of the fourth memtable, 2 and 3 the first two outputs of the compaction.
    if (tables_created == 3) {
      inside = true;
      written = FillTheMemtable("w");
      inside = false;
    }
    // 4 is the flush of that memtable, and 5 the third output of the compaction.
    if (tables_created == 5) {
      const DBCounters counters = dbfull()->TEST_Counters();
      flushes_when_third_output_began = counters.flushes;
      compactions_when_third_output_began = counters.compactions;
    }
    return Status::OK();
  });
  ASSERT_NO_FATAL_FAILURE(WriteSpanningTable(3));
  hook_env->SetHook(nullptr);

  ASSERT_GT(written, 0);
  EXPECT_GE(tables_created, 6) << "the compaction should have had several outputs";
  EXPECT_EQ(flushes_when_third_output_began, 5u)
      << "the waiting memtable was not written out between two outputs";
  EXPECT_EQ(compactions_when_third_output_began, 0u) << "the compaction had already ended";
  EXPECT_EQ(dbfull()->TEST_Counters().compactions, 1u);
  EXPECT_EQ(NumTableFilesAtLevel(0), 1) << "the flush of the waiting memtable is not an input";
  EXPECT_EQ(scheduler_.pending(), 0u);
  for (int i = 0; i < 160; i++) {
    EXPECT_EQ(Get(UserKey(i)), KiB(i)) << "entry " << i;
  }
  for (int i = 0; i < written; i++) {
    EXPECT_EQ(Get("w" + UserKey(i)), KiB(i)) << "written during the compaction: " << i;
  }
  EXPECT_OK(CheckInvariants(dbfull()->TEST_VersionState()));
  ExpectNoStrayTables();
}

TEST_F(DBCompactionTest, AFailedFlushBetweenOutputsDiscardsTheCompaction) {
  // The compaction is not told that the flush it makes between two outputs failed: it goes on, and
  // the database finds the background error when it is done. Nothing is installed then, and the
  // outputs, which no edit names, are removed.
  Options options = ManyOutputsOptions();
  options.l0_compaction_trigger = 100;  // only the request below compacts
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  Reopen(options);
  for (int round = 0; round < 4; round++) {
    ASSERT_NO_FATAL_FAILURE(WriteSpanningTable(round));
  }
  const std::set<uint64_t> inputs = TableNumbers();
  ASSERT_EQ(inputs.size(), 4u);

  // 1 and 2 are the first two outputs. After the second, the memtable that the test filled is
  // written out between outputs, as table number 3, and that is the one that fails. 4 and on are
  // the outputs that the compaction goes on to make.
  int tables_created = 0;
  bool inside = false;
  int written = 0;
  hook_env->SetHook([&](std::string_view call, const std::string& path) {
    if (call != "new_writable" || !path.ends_with(".sst") || inside) {
      return Status::OK();
    }
    tables_created++;
    if (tables_created == 2) {
      inside = true;
      written = FillTheMemtable("w");
      inside = false;
    }
    if (tables_created == 3) {
      return Status::IOError("injected: no table for the waiting memtable");
    }
    return Status::OK();
  });
  const Status s = db_->CompactRange(std::nullopt, std::nullopt);
  hook_env->SetHook(nullptr);

  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_GE(tables_created, 5) << "the compaction should have gone on after the failed flush";
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(counters.bg_errors, 1u);
  EXPECT_EQ(counters.compactions, 0u) << "a compaction was installed after the error";
  EXPECT_EQ(counters.flushes, 4u);
  EXPECT_EQ(TableNumbers(), inputs) << "the version changed";
  EXPECT_EQ(CountFiles(FileType::kTableFile), 4) << "outputs of the discarded compaction remain";
  ExpectNoStrayTables();
  EXPECT_EQ(scheduler_.pending(), 0u);
  const Status put = Put("more", "x");
  EXPECT_TRUE(put.IsIOError()) << "writes must have stopped: " << put.ToString();
  EXPECT_EQ(put.ToString(), s.ToString());
  EXPECT_EQ(Get(UserKey(7)), KiB(7));
  EXPECT_EQ(Get("w" + UserKey(written - 1)), KiB(written - 1));

  // The memtable that did not become a table is in its log, and nothing else was touched.
  Reopen(options);
  for (int i = 0; i < 160; i++) {
    EXPECT_EQ(Get(UserKey(i)), KiB(i)) << "entry " << i;
  }
  for (int i = 0; i < written; i++) {
    EXPECT_EQ(Get("w" + UserKey(i)), KiB(i)) << "written during the compaction: " << i;
  }
  ExpectNoStrayTables();
}

TEST_F(DBCompactionTest, ClosingDoesNotWaitForTheWholeCompaction) {
  // A compaction of ten outputs is held up at its second. Closing begins, and then the compaction
  // goes on: it stops before its third output, removes what it made, and closing returns without
  // an error having been recorded and without anything installed.
  Options options = ManyOutputsOptions();
  options.scheduler = nullptr;  // a thread, since closing waits on another one
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  Reopen(options);
  for (int round = 0; round < 3; round++) {
    ASSERT_NO_FATAL_FAILURE(WriteSpanningTable(round));
  }

  std::mutex mu;
  std::condition_variable cv;
  bool held = false;       // guarded by mu: the compaction is waiting in the hook
  bool released = false;   // guarded by mu
  int tables_created = 0;  // guarded by mu
  hook_env->SetHook([&](std::string_view call, const std::string& path) {
    if (call != "new_writable" || !path.ends_with(".sst")) {
      return Status::OK();
    }
    std::unique_lock<std::mutex> lock(mu);
    // 1 is the flush of the fourth memtable, 2 and 3 are the first outputs of the compaction.
    if (++tables_created == 3) {
      held = true;
      cv.notify_all();
      cv.wait_for(lock, std::chrono::seconds(30), [&] { return released; });
    }
    return Status::OK();
  });
  ASSERT_NO_FATAL_FAILURE(WriteSpanningTable(3));
  const std::set<uint64_t> inputs = TableNumbers();
  ASSERT_EQ(inputs.size(), 4u);
  {
    std::unique_lock<std::mutex> lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return held; }));
  }

  std::thread closer([this] { Close(); });
  // Closing says so in the info log once it has begun, which is after it can be seen by the job.
  bool closing = false;
  for (int i = 0; i < 2000 && !closing; i++) {
    closing = env_->FileContents("/db/LOG").find("closing") != std::string::npos;
    if (!closing) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  EXPECT_TRUE(closing) << "close never began";
  {
    const std::lock_guard<std::mutex> lock(mu);
    released = true;
  }
  cv.notify_all();
  closer.join();
  hook_env->SetHook(nullptr);

  EXPECT_EQ(CountFiles(FileType::kTableFile), 4)
      << "the compaction finished, or left its outputs: only its four inputs belong";
  // The counters are gone with the database, so what says that the compaction stopped without an
  // error is its log, which the next open renames out of the way.
  const std::string log = env_->FileContents("/db/LOG");
  EXPECT_NE(log.find("stopped a compaction"), std::string::npos) << "the job did not stop: " << log;
  EXPECT_EQ(log.find("write path stopped"), std::string::npos)
      << "closing recorded a background error: " << log;
  Reopen(options);
  for (int i = 0; i < 160; i++) {
    EXPECT_EQ(Get(UserKey(i)), KiB(i)) << "entry " << i;
  }
  ASSERT_OK(Put("after", "1"));
  EXPECT_EQ(Get("after"), "1");
}

TEST_F(DBCompactionTest, ASnapshotTakenWhileACompactionRunsReadsItsOwnState) {
  // The job knows the snapshots there were when it began. One that is taken after sees what is
  // there then, which the job can only have kept, since every entry it reads is older.
  Options options = ManyOutputsOptions();
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  Reopen(options);
  for (int round = 0; round < 3; round++) {
    ASSERT_NO_FATAL_FAILURE(WriteSpanningTable(round));
  }
  // The fourth table has the keys that the others lack, and newer versions of the keys of the
  // first, so that the compaction drops the old ones.
  for (int i = 0; i < 160; i++) {
    if (i % 4 == 3) {
      ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
    } else if (i % 4 == 0) {
      ASSERT_OK(Put(UserKey(i), "new" + KiB(i), /*sync=*/false));
    }
  }
  const Snapshot* snapshot = nullptr;
  int tables_created = 0;
  hook_env->SetHook([&](std::string_view call, const std::string& path) {
    if (call != "new_writable" || !path.ends_with(".sst")) {
      return Status::OK();
    }
    // 1 is the flush of the fourth memtable, 2 and 3 the first two outputs of the compaction.
    if (++tables_created == 3) {
      EXPECT_OK(Put("w", "first"));
      snapshot = db_->GetSnapshot();
      EXPECT_OK(Put("w", "second"));
      EXPECT_OK(Put(UserKey(0), "newer"));
    }
    return Status::OK();
  });
  ASSERT_OK(db_->Flush());
  hook_env->SetHook(nullptr);
  ASSERT_NE(snapshot, nullptr) << "the compaction never reached its third output";
  EXPECT_EQ(dbfull()->TEST_Counters().compactions, 1u);

  const auto expect_snapshot_reads = [&](const char* when) {
    SCOPED_TRACE(when);
    EXPECT_EQ(Get("w", snapshot), "first");
    EXPECT_EQ(Get(UserKey(0), snapshot), "new" + KiB(0));
    for (int i = 1; i < 160; i++) {
      EXPECT_EQ(Get(UserKey(i), snapshot), i % 4 == 0 ? "new" + KiB(i) : KiB(i)) << "entry " << i;
    }
  };
  EXPECT_EQ(Get("w"), "second");
  EXPECT_EQ(Get(UserKey(0)), "newer");
  expect_snapshot_reads("after the compaction");
  // And a later compaction, which does know of it, keeps what it sees.
  EXPECT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  expect_snapshot_reads("after a compaction that knows the snapshot");
  EXPECT_EQ(Get("w"), "second");
  db_->ReleaseSnapshot(snapshot);
}

TEST_F(DBCompactionTest, AFailureAnywhereInACompactionLosesNothing) {
  // Every call that a manual compaction makes through the Env fails in turn, and the process or
  // the machine then dies. The compaction that failed may have left outputs, and the MANIFEST may
  // or may not hold the edit that names them: whichever version the next open finds, every write
  // that was acknowledged is in it.
  //
  // In the second round the first remove fails too. A remove is made only to clean up, after a
  // failure or when files are obsolete, so it is the cleanup that fails: a table stays in the
  // directory that the compaction meant to delete, and the next open's sweep has to find that no
  // version names it, or that it is still needed, and act on that.
  const std::vector<Op> ops = {Op::kNewWritable, Op::kAppend,          Op::kFlush,
                               Op::kSync,        Op::kClose,           Op::kSyncDir,
                               Op::kRename,      Op::kNewRandomAccess, Op::kRead};
  enum class Crash { kProcess, kPowerRandom, kPowerDropUnsynced };
  int failed = 0;
  int failed_with_a_failed_remove = 0;
  for (const Crash crash : {Crash::kProcess, Crash::kPowerRandom, Crash::kPowerDropUnsynced}) {
    for (const bool remove_fails_too : {false, true}) {
      for (const Op op : ops) {
        for (int n = 0; n < 16; n++) {
          Close();
          env_ = std::make_unique<FaultInjectionEnv>(static_cast<uint64_t>(n) + 1);
          Options options = SmallBuffer();
          options.target_file_size = 16384;
          Reopen(options);
          std::vector<std::string> expected(60);
          for (int round = 0; round < 3; round++) {
            for (int i = 0; i < 60; i++) {
              expected[static_cast<size_t>(i)] = "r" + std::to_string(round) + KiB(i);
              ASSERT_OK(Put(UserKey(i), expected[static_cast<size_t>(i)], /*sync=*/false));
            }
            ASSERT_OK(db_->Flush());
          }
          scheduler_.RunPending();
          env_->FailOpAfter(op, n);
          if (remove_fails_too) {
            env_->FailOpAfter(Op::kRemove, 0);
          }
          const uint64_t before = env_->injected_errors();
          const Status s = db_->CompactRange(std::nullopt, std::nullopt);
          const uint64_t injected = env_->injected_errors() - before;
          const bool fired = injected > 0;
          env_->ClearFaults();
          if (fired && !s.ok()) {
            failed++;
            if (injected > 1) {
              failed_with_a_failed_remove++;
            }
            EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
            EXPECT_FALSE(Put("more", "x").ok());
          }
          for (int i = 0; i < 60; i++) {
            EXPECT_EQ(Get(UserKey(i)), expected[static_cast<size_t>(i)]);
          }
          switch (crash) {
            case Crash::kProcess:
              env_->SimulateProcessCrash();
              break;
            case Crash::kPowerRandom:
              env_->SimulatePowerLoss();
              break;
            case Crash::kPowerDropUnsynced:
              env_->SimulatePowerLossDropUnsynced();
              break;
          }
          Reopen(options);
          for (int i = 0; i < 60; i++) {
            EXPECT_EQ(Get(UserKey(i)), expected[static_cast<size_t>(i)])
                << "entry " << i << " after the failure of call " << n << " of op "
                << static_cast<int>(op);
          }
          ExpectNoStrayTables();
          EXPECT_OK(CheckInvariants(dbfull()->TEST_VersionState()));
          if (!fired) {
            break;
          }
        }
      }
    }
  }
  EXPECT_GT(failed, 60) << "the sweep hardly made a compaction fail";
  EXPECT_GT(failed_with_a_failed_remove, 20) << "the sweep hardly made a cleanup fail";
}

TEST_F(DBCompactionTest, CompactionRunsOnAThreadScheduler) {
  // Writes go on while the compactions run on another thread, which flushes the memtable that
  // waits between its outputs and installs its edits in between the flushes' own.
  Options options = SmallBuffer();
  options.scheduler = nullptr;  // the database runs its own
  options.target_file_size = 16384;
  Reopen(options);
  std::vector<std::string> expected(300);
  for (int i = 0; i < 3000; i++) {
    const size_t key = static_cast<size_t>((i * 13) % 300);
    expected[key] = "v" + std::to_string(i) + std::string(512, 'y');
    ASSERT_OK(Put(UserKey(static_cast<int>(key)), expected[key], /*sync=*/false));
    if (i % 500 == 499) {
      for (size_t k = 0; k < 300; k++) {
        if (!expected[k].empty()) {
          ASSERT_EQ(Get(UserKey(static_cast<int>(k))), expected[k]);
        }
      }
    }
  }
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  EXPECT_GT(dbfull()->TEST_Counters().compactions + dbfull()->TEST_Counters().trivial_moves, 0u);
  for (size_t k = 0; k < 300; k++) {
    ASSERT_EQ(Get(UserKey(static_cast<int>(k))), expected[k]) << "key " << k;
  }
  EXPECT_OK(CheckInvariants(dbfull()->TEST_VersionState()));
  Reopen(options);
  for (size_t k = 0; k < 300; k++) {
    ASSERT_EQ(Get(UserKey(static_cast<int>(k))), expected[k]) << "key " << k;
  }
}

TEST_F(DBCompactionTest, CompactRangeCallsFromTwoThreadsTakeTurns) {
  // One request at a time: the second caller waits for the first's, and both come back.
  Options options = SmallBuffer();
  options.scheduler = nullptr;
  options.target_file_size = 16384;
  Reopen(options);
  for (int i = 0; i < 600; i++) {
    ASSERT_OK(Put(UserKey(i % 200), KiB(i), /*sync=*/false));
  }
  std::vector<Status> results(2);
  std::vector<std::thread> threads;
  for (size_t t = 0; t < results.size(); t++) {
    threads.emplace_back([this, &results, t] {
      for (int round = 0; round < 3 && (round == 0 || results[t].ok()); round++) {
        results[t] = db_->CompactRange(std::nullopt, std::nullopt);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_OK(results[0]);
  EXPECT_OK(results[1]);
  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  for (int i = 0; i < 200; i++) {
    EXPECT_EQ(Get(UserKey(i)), KiB(i + 400)) << "key " << i;
  }
  EXPECT_OK(CheckInvariants(dbfull()->TEST_VersionState()));
}

}  // namespace
}  // namespace shale
