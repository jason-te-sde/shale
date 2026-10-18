// Writing a table from an iterator, and the cache of open tables that a read goes through.
//
// BuildTable is where the data of a flush or a compaction first reaches a file, so what the tests
// pin is what a crash and a failing disk can do to it: the file is either complete, durable and
// named by the metadata the caller gets back, or it is not there at all. The sweeps over the Env's
// fault points are the check of that, one call at a time, since a path that leaves a partial file
// behind or ends the process on an abandoned builder only shows when that very call fails.

#include "db/builder.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "db/handle_check_env.h"
#include "db/hook_env.h"
#include "db/table_cache.h"
#include "io/iterator_util.h"
#include "io/memtable.h"
#include "io/table.h"
#include "io/table_builder.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/filename.h"
#include "util/random.h"

namespace shale {
namespace {

constexpr char kDb[] = "/db";

using Op = FaultInjectionEnv::Op;

// The name of `path` without its directory.
std::string BareName(const std::string& path) { return path.substr(std::string(kDb).size() + 1); }

// Everything a read of the table files needs, over an Env that can fail and lose power.
class DbFixture : public testing::Test {
 protected:
  DbFixture()
      : env_(11), block_cache_(size_t{1} << 20), table_cache_(kDb, &env_, &block_cache_, 100) {}

  void SetUp() override { ASSERT_OK(env_.CreateDir(kDb)); }

  // Every file the test opened for writing was closed, whatever became of the build.
  void TearDown() override { EXPECT_EQ(checked_env_.leaked(), 0) << "a file was not closed"; }

  // A memtable holding NumberedEntries(n), with `prefix` in front of every value.
  static std::shared_ptr<MemTable> NumberedMemTable(int n, const std::string& prefix = "") {
    auto mem = std::make_shared<MemTable>(1);
    for (int i = 0; i < n; i++) {
      mem->Add(static_cast<SequenceNumber>(i) + 1, ValueType::kValue, UserKey(i),
               prefix + ValueOf(i));
    }
    return mem;
  }

  // Builds table `number` from `mem` into `*meta`. A failure is fatal to the caller: call it as
  // ASSERT_NO_FATAL_FAILURE(Build(...)), so that nothing runs on a description of a table that was
  // not built.
  void Build(uint64_t number, const MemTable& mem, FileMetaData* meta,
             const TableBuilderOptions& options = {}) {
    meta->number = number;
    const std::unique_ptr<Iterator> iter = mem.NewIterator();
    ASSERT_OK(BuildTable(kDb, &checked_env_, options, &table_cache_, iter.get(), meta));
  }

  // The bare names in the database directory, in the order the Env lists them.
  std::vector<std::string> Children() {
    std::vector<std::string> names;
    EXPECT_OK(env_.GetChildren(kDb, &names));
    return names;
  }

  // Everything an iterator returns, as entries; its status is checked to be OK.
  static Entries Drain(Iterator* iter) {
    Entries entries;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      entries.emplace_back(std::string(iter->key()), std::string(iter->value()));
    }
    EXPECT_OK(iter->status());
    return entries;
  }

  FaultInjectionEnv env_;
  HandleCheckEnv checked_env_{&env_};  // what the builds write through; the reads use env_
  BlockCache block_cache_;
  TableCache table_cache_;
};

class BuilderTest : public DbFixture {};

TEST_F(BuilderTest, BuildsTableWithBounds) {
  // A small block size makes the table many blocks, so that the bounds and the contents are those
  // of a table with an index to search, and not of one block. Each table has a number of its own:
  // the block cache names a block by its table's number, and a number is never used twice.
  uint64_t number = 7;
  for (const size_t block_size : {size_t{4096}, size_t{128}}) {
    SCOPED_TRACE(block_size);
    const auto mem = NumberedMemTable(100);
    TableBuilderOptions options;
    options.block_size = block_size;
    FileMetaData meta;
    ASSERT_NO_FATAL_FAILURE(Build(number, *mem, &meta, options));

    const std::string path = TableFileName(kDb, number);
    ASSERT_TRUE(env_.FileExists(path));
    uint64_t size = 0;
    ASSERT_OK(env_.GetFileSize(path, &size));
    EXPECT_GT(size, 0u);
    EXPECT_EQ(meta.file_size, size);
    EXPECT_EQ(meta.number, number);
    EXPECT_EQ(meta.smallest, IKey(UserKey(0), 1));
    EXPECT_EQ(meta.largest, IKey(UserKey(99), 100));

    // The file is the table of the whole memtable, found through the cache that built it.
    std::shared_ptr<Table> table;
    ASSERT_OK(table_cache_.FindTable(number, meta.file_size, &table));
    EXPECT_EQ(Drain(table->NewIterator(TableReadOptions{}).get()), NumberedEntries(100));

    // Nothing else is left in the directory: no temporary file, no second copy.
    EXPECT_EQ(Children(), std::vector<std::string>{BareName(path)});
    table.reset();
    table_cache_.Evict(number);
    ASSERT_OK(env_.RemoveFile(path));
    number++;
  }
}

TEST_F(BuilderTest, BuildLeavesTheTableOpenInTheCache) {
  // The table is verified by opening it through the cache, which keeps it, so the first read of a
  // table that was just built does not open it again. Deleting the file shows where it is served
  // from.
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(7, *NumberedMemTable(10), &meta));
  ASSERT_OK(env_.RemoveFile(TableFileName(kDb, 7)));
  std::shared_ptr<Table> table;
  EXPECT_OK(table_cache_.FindTable(7, meta.file_size, &table));
}

TEST_F(BuilderTest, OneEntryTableHasEqualBounds) {
  const auto mem = NumberedMemTable(1);
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(3, *mem, &meta));
  EXPECT_EQ(meta.smallest, IKey(UserKey(0), 1));
  EXPECT_EQ(meta.largest, meta.smallest);
}

TEST_F(BuilderTest, EmptyIteratorCreatesNothing) {
  const auto mem = NumberedMemTable(0);
  FileMetaData meta;
  meta.number = 9;
  const std::unique_ptr<Iterator> iter = mem->NewIterator();
  ASSERT_OK(
      BuildTable(kDb, &checked_env_, TableBuilderOptions{}, &table_cache_, iter.get(), &meta));
  EXPECT_EQ(meta.file_size, 0u);
  EXPECT_TRUE(meta.smallest.empty());
  EXPECT_TRUE(meta.largest.empty());
  EXPECT_FALSE(env_.FileExists(TableFileName(kDb, 9)));
  EXPECT_TRUE(Children().empty());
}

TEST_F(BuilderTest, StaleMetadataIsClearedWhenNothingIsBuilt) {
  // The caller may reuse a FileMetaData; what is left in it from before must not read as the
  // description of a file that was not made.
  const auto mem = NumberedMemTable(0);
  FileMetaData meta;
  meta.number = 9;
  meta.file_size = 1234;
  meta.smallest = IKey("a", 1);
  meta.largest = IKey("z", 1);
  const std::unique_ptr<Iterator> iter = mem->NewIterator();
  ASSERT_OK(
      BuildTable(kDb, &checked_env_, TableBuilderOptions{}, &table_cache_, iter.get(), &meta));
  EXPECT_EQ(meta.file_size, 0u);
  EXPECT_TRUE(meta.smallest.empty());
  EXPECT_TRUE(meta.largest.empty());
}

TEST_F(BuilderTest, TheTableIsSyncedThenOpenedThenItsDirectoryIsSynced) {
  // The order that makes a table safe to name in the MANIFEST: its bytes are durable, it is seen to
  // be a table, and only then is the entry that names it. A sequence of calls, which a failing
  // call cannot show: each of them fails and leaves no file whichever comes first.
  HookEnv hook_env(&env_);
  TableCache cache(kDb, &hook_env, &block_cache_, 100);  // opens the table through the hook too
  std::vector<std::string> calls;
  hook_env.SetHook([&calls](std::string_view call, const std::string& path) {
    if (call == "sync" || call == "new_random_access" || call == "sync_dir") {
      calls.push_back(std::string(call) + " " + path);
    }
    return Status::OK();
  });
  const auto mem = NumberedMemTable(10);
  FileMetaData meta;
  meta.number = 7;
  const std::unique_ptr<Iterator> iter = mem->NewIterator();
  ASSERT_OK(BuildTable(kDb, &hook_env, TableBuilderOptions{}, &cache, iter.get(), &meta));
  EXPECT_EQ(calls, (std::vector<std::string>{"sync /db/000007.sst",
                                             "new_random_access /db/000007.sst", "sync_dir /db"}));
}

TEST_F(BuilderTest, FailedSyncRemovesPartialFile) {
  const auto mem = NumberedMemTable(100);
  FileMetaData meta;
  meta.number = 4;
  const std::unique_ptr<Iterator> iter = mem->NewIterator();
  env_.FailOpAfter(Op::kSync, 0);
  const Status s =
      BuildTable(kDb, &checked_env_, TableBuilderOptions{}, &table_cache_, iter.get(), &meta);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_FALSE(env_.FileExists(TableFileName(kDb, 4)));
  EXPECT_TRUE(Children().empty());
  EXPECT_EQ(meta.file_size, 0u);
}

TEST_F(BuilderTest, EveryFailingCallLeavesNoFileAndAnIOError) {
  // Fail the first, the second, the third... call of each kind the build makes, until a build
  // succeeds because there are no more of them. Each failure must come back as the IOError that
  // was injected, with no file left under the table's name and no metadata that describes one.
  // The Ops below are all that BuildTable makes use of; the first call of each must be one it
  // makes, or the sweep would pass for a build that never opens the file it wrote, never syncs the
  // directory, and so on.
  const std::vector<Op> ops = {Op::kNewWritable, Op::kAppend,          Op::kSync, Op::kClose,
                               Op::kSyncDir,     Op::kNewRandomAccess, Op::kRead};
  const auto mem = NumberedMemTable(100);
  for (const Op op : ops) {
    SCOPED_TRACE(static_cast<int>(op));
    int failures = 0;
    for (int successes = 0; successes < 64; successes++) {
      env_.ClearFaults();
      env_.FailOpAfter(op, successes);
      FileMetaData meta;
      meta.number = 5;
      const std::unique_ptr<Iterator> iter = mem->NewIterator();
      const Status s =
          BuildTable(kDb, &checked_env_, TableBuilderOptions{}, &table_cache_, iter.get(), &meta);
      env_.ClearFaults();
      if (s.ok()) {
        EXPECT_GT(meta.file_size, 0u);
        ASSERT_OK(env_.RemoveFile(TableFileName(kDb, 5)));
        table_cache_.Evict(5);
        break;
      }
      failures++;
      EXPECT_TRUE(s.IsIOError()) << s.ToString();
      EXPECT_FALSE(env_.FileExists(TableFileName(kDb, 5)));
      EXPECT_TRUE(Children().empty());
      EXPECT_EQ(meta.file_size, 0u);
      // A table that failed to build is not left in the cache of open tables either.
      std::shared_ptr<Table> table;
      EXPECT_TRUE(table_cache_.FindTable(5, 1, &table).IsIOError());
    }
    EXPECT_GT(failures, 0) << "the build never made a call of this kind";
  }
}

TEST_F(BuilderTest, FailingInputRemovesFileAndReportsItsError) {
  // An iterator that fails after some entries is not the end of the data. Writing the entries it
  // did return as a table would lose the rest without a word.
  Entries entries = NumberedEntries(10);
  const Status error = Status::Corruption("injected input damage");
  const std::unique_ptr<Iterator> iter = NewFailingIterator(entries, error);
  FileMetaData meta;
  meta.number = 6;
  const Status s =
      BuildTable(kDb, &checked_env_, TableBuilderOptions{}, &table_cache_, iter.get(), &meta);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(s.ToString(), error.ToString());
  EXPECT_FALSE(env_.FileExists(TableFileName(kDb, 6)));
  EXPECT_TRUE(Children().empty());
  EXPECT_EQ(meta.file_size, 0u);
}

TEST_F(BuilderTest, InputThatFailsBeforeItsFirstEntryIsAnError) {
  // An empty result and a failure look alike to Valid(). Only the status tells them apart.
  const std::unique_ptr<Iterator> iter = NewErrorIterator(Status::IOError("injected"));
  FileMetaData meta;
  meta.number = 6;
  const Status s =
      BuildTable(kDb, &checked_env_, TableBuilderOptions{}, &table_cache_, iter.get(), &meta);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_TRUE(Children().empty());
}

TEST_F(BuilderTest, BuiltTableSurvivesPowerLoss) {
  // BuildTable is done when the table can no longer be lost: its bytes synced and its directory
  // entry too. The harshest power loss keeps only what was made durable.
  const auto mem = NumberedMemTable(100);
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(8, *mem, &meta));
  env_.SimulatePowerLossDropUnsynced();

  // Every handle from before the loss is dead, the cache's among them.
  BlockCache block_cache(size_t{1} << 20);
  TableCache fresh(kDb, &env_, &block_cache, 10);
  std::shared_ptr<Table> table;
  ASSERT_OK(fresh.FindTable(8, meta.file_size, &table));
  EXPECT_EQ(Drain(table->NewIterator(TableReadOptions{}).get()), NumberedEntries(100));
}

// TableCache

class TableCacheTest : public DbFixture {};

TEST_F(TableCacheTest, FindTableOpensOnceAndKeepsTheTable) {
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(10), &meta));
  std::shared_ptr<Table> first;
  std::shared_ptr<Table> second;
  ASSERT_OK(table_cache_.FindTable(2, meta.file_size, &first));
  ASSERT_OK(table_cache_.FindTable(2, meta.file_size, &second));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
}

TEST_F(TableCacheTest, FindTableOfAMissingFileIsAnIOError) {
  std::shared_ptr<Table> table;
  const Status s = table_cache_.FindTable(77, 100, &table);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(table, nullptr);
}

TEST_F(TableCacheTest, FindTableWithTheWrongSizeIsCorruption) {
  // BuildTable leaves its table in the cache that built it, and the cache serves by number alone,
  // so a cache of its own is what opens the file again.
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(10), &meta));
  TableCache fresh(kDb, &env_, &block_cache_, 10);
  std::shared_ptr<Table> table;
  const Status s = fresh.FindTable(2, meta.file_size - 1, &table);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_NE(s.ToString().find(TableFileName(kDb, 2)), std::string::npos)
      << "the message names the file: " << s.ToString();
  EXPECT_EQ(table, nullptr);
  // A table that did not open is not remembered: the next ask, with the right size, opens it.
  EXPECT_OK(fresh.FindTable(2, meta.file_size, &table));
}

TEST_F(TableCacheTest, EvictForcesAReopen) {
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(10), &meta));
  std::shared_ptr<Table> before;
  ASSERT_OK(table_cache_.FindTable(2, meta.file_size, &before));
  table_cache_.Evict(2);
  std::shared_ptr<Table> after;
  ASSERT_OK(table_cache_.FindTable(2, meta.file_size, &after));
  EXPECT_NE(before, after);
  // Evicting a number that was never opened is not an error.
  table_cache_.Evict(12345);
}

TEST_F(TableCacheTest, EvictedAndDeletedFileIsGone) {
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(10), &meta));
  std::shared_ptr<Table> table;
  ASSERT_OK(table_cache_.FindTable(2, meta.file_size, &table));
  table.reset();
  ASSERT_OK(env_.RemoveFile(TableFileName(kDb, 2)));

  // The cache still holds the open table, so the file's deletion is not seen until it is evicted:
  // that is why the engine evicts a table as it deletes the file.
  EXPECT_OK(table_cache_.FindTable(2, meta.file_size, &table));
  table.reset();
  table_cache_.Evict(2);
  EXPECT_TRUE(table_cache_.FindTable(2, meta.file_size, &table).IsIOError());
}

TEST_F(TableCacheTest, CapacityCountsTablesNotBytes) {
  TableCache small(kDb, &env_, &block_cache_, 2);
  FileMetaData metas[3];
  for (uint64_t i = 0; i < 3; i++) {
    ASSERT_NO_FATAL_FAILURE(Build(i + 1, *NumberedMemTable(10), &metas[i]));
  }
  std::weak_ptr<Table> weak[3];
  for (size_t i = 0; i < 3; i++) {
    std::shared_ptr<Table> table;
    ASSERT_OK(small.FindTable(i + 1, metas[i].file_size, &table));
    weak[i] = table;
  }
  // Three tables went through a cache of two, nothing else held them, and the least recently used
  // is the one that was closed.
  EXPECT_TRUE(weak[0].expired());
  EXPECT_FALSE(weak[1].expired());
  EXPECT_FALSE(weak[2].expired());
}

TEST_F(TableCacheTest, DamagedDataBlockIsCorruptionNamingTheFile) {
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(30), &meta));
  std::string contents = env_.FileContents(TableFileName(kDb, 2));
  ASSERT_GT(contents.size(), 16u);
  contents[10] ^= 0x01;  // in the first data block, which Open() does not read
  env_.SetFileContents(TableFileName(kDb, 2), contents);

  BlockCache blocks(size_t{1} << 20);
  TableCache fresh(kDb, &env_, &blocks, 10);
  TableGetResult result;
  const LookupKey key(UserKey(0), kMaxSequenceNumber);
  const Status s = fresh.Get(TableReadOptions{}, 2, meta.file_size, key.internal_key(), &result);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_NE(s.ToString().find(TableFileName(kDb, 2)), std::string::npos)
      << "the message names the file: " << s.ToString();
}

TEST_F(TableCacheTest, IteratorServesTheEntriesOfTheTable) {
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(30), &meta));
  const std::unique_ptr<Iterator> iter =
      table_cache_.NewIterator(TableReadOptions{}, 2, meta.file_size);
  EXPECT_EQ(Drain(iter.get()), NumberedEntries(30));
  iter->Seek(IKey(UserKey(20), kMaxSequenceNumber));
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), IKey(UserKey(20), 21));
  EXPECT_EQ(iter->value(), ValueOf(20));
  EXPECT_OK(iter->status());
}

TEST_F(TableCacheTest, DamagedDataBlockInAnIteratorIsCorruptionNamingTheFile) {
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(30), &meta));
  std::string contents = env_.FileContents(TableFileName(kDb, 2));
  ASSERT_GT(contents.size(), 16u);
  contents[10] ^= 0x01;  // in the first data block, which Open() does not read
  env_.SetFileContents(TableFileName(kDb, 2), contents);

  BlockCache blocks(size_t{1} << 20);
  TableCache fresh(kDb, &env_, &blocks, 10);
  const std::unique_ptr<Iterator> iter = fresh.NewIterator(TableReadOptions{}, 2, meta.file_size);
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  const Status s = iter->status();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  const std::string path = TableFileName(kDb, 2);
  EXPECT_NE(s.ToString().find(path), std::string::npos)
      << "the message names the file: " << s.ToString();
  EXPECT_EQ(s.ToString().find(path), s.ToString().rfind(path)) << "once: " << s.ToString();
}

TEST_F(TableCacheTest, NoShardHoldsFewerThanSixteenTables) {
  // The cache is least recently used within a shard and not across them, so it is divided only as
  // far as every shard can hold a useful number of tables.
  EXPECT_EQ(TableCache::ShardBitsFor(0), 0);
  EXPECT_EQ(TableCache::ShardBitsFor(1), 0);
  EXPECT_EQ(TableCache::ShardBitsFor(20), 0) << "max_open_files at its minimum is one shard";
  EXPECT_EQ(TableCache::ShardBitsFor(31), 0);
  EXPECT_EQ(TableCache::ShardBitsFor(32), 1);
  EXPECT_EQ(TableCache::ShardBitsFor(63), 1);
  EXPECT_EQ(TableCache::ShardBitsFor(64), 2);
  EXPECT_EQ(TableCache::ShardBitsFor(127), 2);
  EXPECT_EQ(TableCache::ShardBitsFor(128), 3);
  EXPECT_EQ(TableCache::ShardBitsFor(255), 3);
  EXPECT_EQ(TableCache::ShardBitsFor(256), 4);
  EXPECT_EQ(TableCache::ShardBitsFor(1000), 4) << "the default max_open_files";
  EXPECT_EQ(TableCache::ShardBitsFor(std::numeric_limits<size_t>::max()), 4);
}

TEST_F(TableCacheTest, CacheOfTwentyHoldsTwentyTables) {
  // The behaviour the shard count is for: twenty tables in a cache for twenty, one shard, are all
  // kept. Sixteen shards of one or two would drop some of them whatever the hash.
  constexpr uint64_t kTables = 20;
  TableCache cache(kDb, &env_, &block_cache_, kTables);
  std::vector<std::weak_ptr<Table>> weak;
  for (uint64_t number = 1; number <= kTables; number++) {
    FileMetaData meta;
    ASSERT_NO_FATAL_FAILURE(Build(number, *NumberedMemTable(5), &meta));
    std::shared_ptr<Table> table;
    ASSERT_OK(cache.FindTable(number, meta.file_size, &table));
    weak.push_back(table);
  }
  for (const std::weak_ptr<Table>& table : weak) {
    EXPECT_FALSE(table.expired());
  }
}

TEST_F(TableCacheTest, ConcurrentFindGetIterateAndEvictAgree) {
  // Several threads work on a few tables through a cache that holds fewer than that, so the tables
  // are evicted and opened again all the time, and Evict() takes them out from under the readers.
  // Every answer must be the table's own, and ThreadSanitizer must find nothing.
  constexpr uint64_t kTables = 8;
  constexpr int kThreads = 4;
  constexpr int kOperations = 1500;
  FileMetaData metas[kTables];
  for (uint64_t i = 0; i < kTables; i++) {
    ASSERT_NO_FATAL_FAILURE(
        Build(i + 1, *NumberedMemTable(10, "t" + std::to_string(i + 1) + "-"), &metas[i]));
  }
  TableCache cache(kDb, &env_, &block_cache_, 3);

  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t] {
      Random rnd(static_cast<uint64_t>(t) + 100);
      for (int op = 0; op < kOperations; op++) {
        const uint64_t i = rnd.Uniform(static_cast<uint32_t>(kTables));
        const uint64_t number = i + 1;
        const std::string prefix = "t" + std::to_string(number) + "-";
        switch (rnd.Uniform(4)) {
          case 0: {
            std::shared_ptr<Table> table;
            if (!cache.FindTable(number, metas[i].file_size, &table).ok() || table == nullptr) {
              wrong++;
            }
            break;
          }
          case 1: {
            TableGetResult result;
            const LookupKey key(UserKey(5), kMaxSequenceNumber);
            const Status s = cache.Get(TableReadOptions{}, number, metas[i].file_size,
                                       key.internal_key(), &result);
            if (!s.ok() || result.kind != TableGetResult::Kind::kFound ||
                result.value != prefix + ValueOf(5)) {
              wrong++;
            }
            break;
          }
          case 2: {
            const std::unique_ptr<Iterator> iter =
                cache.NewIterator(TableReadOptions{}, number, metas[i].file_size);
            int count = 0;
            for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
              if (iter->value() != prefix + ValueOf(count)) {
                wrong++;
              }
              count++;
            }
            if (count != 10 || !iter->status().ok()) {
              wrong++;
            }
            break;
          }
          default:
            cache.Evict(number);
            break;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(wrong.load(), 0);
}

TEST_F(TableCacheTest, ZeroCapacityStillServesReads) {
  TableCache none(kDb, &env_, &block_cache_, 0);
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(10), &meta));
  std::shared_ptr<Table> table;
  ASSERT_OK(none.FindTable(2, meta.file_size, &table));
  TableGetResult result;
  const LookupKey key(UserKey(3), kMaxSequenceNumber);
  ASSERT_OK(none.Get(TableReadOptions{}, 2, meta.file_size, key.internal_key(), &result));
  EXPECT_EQ(result.kind, TableGetResult::Kind::kFound);
  EXPECT_EQ(result.value, ValueOf(3));
}

TEST_F(TableCacheTest, GetFindsFoundAndAbsentKeys) {
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(10), &meta));
  TableGetResult result;
  const LookupKey present(UserKey(4), kMaxSequenceNumber);
  ASSERT_OK(
      table_cache_.Get(TableReadOptions{}, 2, meta.file_size, present.internal_key(), &result));
  EXPECT_EQ(result.kind, TableGetResult::Kind::kFound);
  EXPECT_EQ(result.value, ValueOf(4));

  const LookupKey absent("not-there", kMaxSequenceNumber);
  ASSERT_OK(
      table_cache_.Get(TableReadOptions{}, 2, meta.file_size, absent.internal_key(), &result));
  EXPECT_EQ(result.kind, TableGetResult::Kind::kNotFound);

  // Entry 4 was written at sequence 5; a read as of 4 does not see it.
  const LookupKey too_old(UserKey(4), 4);
  ASSERT_OK(
      table_cache_.Get(TableReadOptions{}, 2, meta.file_size, too_old.internal_key(), &result));
  EXPECT_EQ(result.kind, TableGetResult::Kind::kNotFound);
}

TEST_F(TableCacheTest, GetOfAMissingTableIsAnError) {
  TableGetResult result;
  const LookupKey key(UserKey(1), kMaxSequenceNumber);
  const Status s = table_cache_.Get(TableReadOptions{}, 50, 100, key.internal_key(), &result);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
}

TEST_F(TableCacheTest, BlockCacheIsKeyedByFileNumber) {
  // Two tables with the same keys, so with a data block at the same offset, and different values.
  // If their blocks went into the shared cache under one name, the second read would be served the
  // first table's block.
  FileMetaData a;
  ASSERT_NO_FATAL_FAILURE(Build(10, *NumberedMemTable(20, "a-"), &a));
  FileMetaData b;
  ASSERT_NO_FATAL_FAILURE(Build(11, *NumberedMemTable(20, "b-"), &b));
  const LookupKey key(UserKey(5), kMaxSequenceNumber);
  for (int round = 0; round < 2; round++) {
    TableGetResult from_a;
    TableGetResult from_b;
    ASSERT_OK(table_cache_.Get(TableReadOptions{}, 10, a.file_size, key.internal_key(), &from_a));
    ASSERT_OK(table_cache_.Get(TableReadOptions{}, 11, b.file_size, key.internal_key(), &from_b));
    EXPECT_EQ(from_a.value, "a-" + ValueOf(5));
    EXPECT_EQ(from_b.value, "b-" + ValueOf(5));
  }
}

TEST_F(TableCacheTest, NewIteratorReadsTheTable) {
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(30), &meta));
  const std::unique_ptr<Iterator> iter =
      table_cache_.NewIterator(TableReadOptions{}, 2, meta.file_size);
  EXPECT_EQ(Drain(iter.get()), NumberedEntries(30));
}

TEST_F(TableCacheTest, NewIteratorOfAMissingTableReportsTheError) {
  const std::unique_ptr<Iterator> iter = table_cache_.NewIterator(TableReadOptions{}, 50, 100);
  ASSERT_NE(iter, nullptr);
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().IsIOError()) << iter->status().ToString();
}

TEST_F(TableCacheTest, NewIteratorOfADamagedTableReportsCorruption) {
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(30), &meta));
  TableCache fresh(kDb, &env_, &block_cache_, 10);
  const std::unique_ptr<Iterator> iter =
      fresh.NewIterator(TableReadOptions{}, 2, meta.file_size + 1);
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().IsCorruption()) << iter->status().ToString();
}

TEST_F(TableCacheTest, IteratorKeepsItsTableAliveAcrossEviction) {
  FileMetaData meta;
  ASSERT_NO_FATAL_FAILURE(Build(2, *NumberedMemTable(30), &meta));
  std::unique_ptr<Iterator> iter = table_cache_.NewIterator(TableReadOptions{}, 2, meta.file_size);
  table_cache_.Evict(2);
  ASSERT_OK(env_.RemoveFile(TableFileName(kDb, 2)));
  EXPECT_EQ(Drain(iter.get()), NumberedEntries(30));
}

}  // namespace
}  // namespace shale
