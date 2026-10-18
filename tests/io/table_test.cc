// The table reader: iteration, point lookups, the bloom filter and the block cache, and above all
// what it does with a file that is not what the builder wrote.
//
// A table is read from a disk that fails and from a directory anyone can put a file in, so the
// reader's contract is stronger than getting right files right. Whatever the bytes are, Open() and
// everything after it return either the value that was written or a Corruption status: never a
// crash, never a read outside a buffer, never a value that was not stored under the key. The
// bit-flip tests are the check of that, and they only mean something under AddressSanitizer.

#include "io/table.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "io/block.h"
#include "io/block_builder.h"
#include "io/env_util.h"
#include "io/table_builder.h"
#include "io/table_format.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/dbformat.h"
#include "util/random.h"

namespace shale {
namespace {

// The sweeps over every byte of a table are single-threaded, so ThreadSanitizer has nothing to find
// in them and slows them down about tenfold. Under it they visit every 11th byte, which still puts
// damage in every block, trailer and the footer; everywhere else they visit all of them.
#if defined(__SANITIZE_THREAD__)
#define SHALE_TEST_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SHALE_TEST_UNDER_TSAN 1
#endif
#endif
#ifdef SHALE_TEST_UNDER_TSAN
constexpr size_t kSweepStride = 11;
#else
constexpr size_t kSweepStride = 1;
#endif

// What a full iteration returns, and how it ended.
struct Drained {
  Entries entries;
  Status status;
};

// Reads everything `it` returns from its first entry, or from where it is if `from_first` is
// false. Stops after `limit` entries, so that a reader that cannot stop is a failed test and not a
// test that never ends.
Drained Drain(Iterator* it, size_t limit = 1000000, bool from_first = true) {
  Drained drained;
  if (from_first) {
    it->SeekToFirst();
  }
  for (; it->Valid(); it->Next()) {
    drained.entries.emplace_back(std::string(it->key()), std::string(it->value()));
    if (drained.entries.size() > limit) {
      ADD_FAILURE() << "the iteration did not end after " << limit << " entries";
      break;
    }
  }
  drained.status = it->status();
  return drained;
}

class TableTest : public testing::Test {
 protected:
  // Writes `entries`, which must be sorted by internal key, to `path`, and returns the file size.
  uint64_t BuildTable(const std::string& path, const Entries& entries,
                      const TableBuilderOptions& options = {}) {
    std::unique_ptr<WritableFile> file;
    EXPECT_OK(env_->NewWritableFile(path, &file));
    TableBuilder builder(options, file.get());
    for (const auto& [key, value] : entries) {
      builder.Add(key, value);
    }
    EXPECT_OK(builder.Finish());
    EXPECT_OK(file->Close());
    uint64_t size = 0;
    EXPECT_OK(env_->GetFileSize(path, &size));
    EXPECT_EQ(size, builder.FileSize());
    return size;
  }

  Status OpenTable(const std::string& path, BlockCache* cache, std::shared_ptr<Table>* table,
                   uint64_t cache_id = 1) {
    std::unique_ptr<RandomAccessFile> file;
    Status s = env_->NewRandomAccessFile(path, &file);
    if (!s.ok()) {
      return s;
    }
    uint64_t size = 0;
    s = env_->GetFileSize(path, &size);
    if (!s.ok()) {
      return s;
    }
    return Table::Open(std::move(file), size, cache_id, cache, table);
  }

  // Builds `entries` and opens the result; the test stops if either fails.
  std::shared_ptr<Table> BuildAndOpen(const Entries& entries, BlockCache* cache = nullptr,
                                      const TableBuilderOptions& options = {},
                                      const std::string& path = "/t.sst") {
    BuildTable(path, entries, options);
    std::shared_ptr<Table> table;
    const Status s = OpenTable(path, cache, &table);
    EXPECT_OK(s);
    return table;
  }

  std::string ReadFile(const std::string& path) {
    std::string data;
    EXPECT_OK(ReadFileToString(env_.get(), path, &data));
    return data;
  }

  void WriteFile(const std::string& path, const std::string& data) {
    ASSERT_OK(WriteStringToFile(env_.get(), data, path, false));
  }

  std::unique_ptr<Env> env_ = NewMemEnv();
};

// A Get as a value to compare: what it returned, and for kFound the value.
struct Lookup {
  Status status;
  TableGetResult result;
};

Lookup Get(const Table& table, std::string_view user_key, SequenceNumber sequence,
           const TableReadOptions& options = {}) {
  Lookup lookup;
  lookup.status = table.Get(options, IKey(user_key, sequence, kValueTypeForSeek), &lookup.result);
  return lookup;
}

// A Table is made by Open(), which gives it to a shared pointer. One made any other way would
// throw from shared_from_this() the first time an iterator was asked for, and nothing under src/
// throws, so the way in is closed at compile time: the constructor takes a type that is private to
// Table, which no other code can name. The check is a concept, since an access error is a failure
// of substitution, and it is tried on a class that does have a public type of that name, so that a
// check that could not fail would show.
template <typename T>
concept NamesPrivateTag = requires { typename T::PrivateTag; };

struct HasPublicTag {
  struct PrivateTag {};
};

static_assert(NamesPrivateTag<HasPublicTag>, "the check can tell a public tag from a private one");
static_assert(!NamesPrivateTag<Table>,
              "Table::PrivateTag must be private, or anyone can build a Table");
static_assert(!std::is_default_constructible_v<Table>);
static_assert(!std::is_copy_constructible_v<Table>);

TEST_F(TableTest, IterateAndSeek) {
  const Entries entries = NumberedEntries(10000);
  const std::shared_ptr<Table> table = BuildAndOpen(entries);
  ASSERT_NE(table, nullptr);

  std::unique_ptr<Iterator> it = table->NewIterator({});
  EXPECT_FALSE(it->Valid()) << "not positioned until the first seek";
  const Drained drained = Drain(it.get());
  EXPECT_OK(drained.status);
  EXPECT_EQ(drained.entries, entries);

  it->Seek(IKey("k005000", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), IKey("k005000", 5001));
  EXPECT_EQ(it->value(), ValueOf(5000));
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), IKey("k005001", 5002));

  // Targets in between entries, before the first and after the last, and at the very last.
  it->Seek(IKey("k005000x", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()), "k005001");
  it->Seek(IKey("", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()), "k000000");
  it->Seek(IKey("k009999", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()), "k009999");
  it->Next();
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
  it->Seek(IKey("z", kMaxSequenceNumber));
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());

  // A seek to every thousandth key and the one after it, which crosses block boundaries.
  for (int i = 0; i < 10000; i += 997) {
    it->Seek(IKey(UserKey(i), kMaxSequenceNumber));
    ASSERT_TRUE(it->Valid()) << i;
    EXPECT_EQ(it->key(), entries[static_cast<size_t>(i)].first);
  }
}

TEST_F(TableTest, EveryEntryOfAManyBlockTableIsFoundBySeekAndGet) {
  // One entry to a block: every lookup and every seek crosses the index.
  TableBuilderOptions options;
  options.block_size = 1;
  const Entries entries = NumberedEntries(300);
  const std::shared_ptr<Table> table = BuildAndOpen(entries, nullptr, options);
  ASSERT_NE(table, nullptr);
  std::unique_ptr<Iterator> it = table->NewIterator({});
  for (int i = 0; i < 300; ++i) {
    it->Seek(entries[static_cast<size_t>(i)].first);
    ASSERT_TRUE(it->Valid()) << i;
    EXPECT_EQ(it->key(), entries[static_cast<size_t>(i)].first);
    const Lookup found = Get(*table, UserKey(i), kMaxSequenceNumber);
    ASSERT_OK(found.status);
    EXPECT_EQ(found.result.kind, TableGetResult::Kind::kFound) << i;
    EXPECT_EQ(found.result.value, ValueOf(i));
  }
  EXPECT_EQ(Drain(it.get()).entries, entries);
}

TEST_F(TableTest, GetFoundDeletedMissing) {
  Entries entries;
  entries.emplace_back(IKey("a", 5), "va");
  entries.emplace_back(IKey("b", 6, ValueType::kDeletion), "");
  const std::shared_ptr<Table> table = BuildAndOpen(entries);
  ASSERT_NE(table, nullptr);

  Lookup a = Get(*table, "a", kMaxSequenceNumber);
  ASSERT_OK(a.status);
  EXPECT_EQ(a.result.kind, TableGetResult::Kind::kFound);
  EXPECT_EQ(a.result.value, "va");

  const Lookup b = Get(*table, "b", kMaxSequenceNumber);
  ASSERT_OK(b.status);
  EXPECT_EQ(b.result.kind, TableGetResult::Kind::kDeleted);
  EXPECT_TRUE(b.result.value.empty());

  const Lookup c = Get(*table, "c", kMaxSequenceNumber);
  ASSERT_OK(c.status);
  EXPECT_EQ(c.result.kind, TableGetResult::Kind::kNotFound);

  // The entry for "a" is newer than a lookup at sequence 4, which cannot see it. The first entry
  // at or after the lookup key is then the one for "b", whose user key differs.
  const Lookup before = Get(*table, "a", 4);
  ASSERT_OK(before.status);
  EXPECT_EQ(before.result.kind, TableGetResult::Kind::kNotFound);
  // At exactly its own sequence number it is visible.
  const Lookup exact = Get(*table, "a", 5);
  ASSERT_OK(exact.status);
  EXPECT_EQ(exact.result.kind, TableGetResult::Kind::kFound);

  // A result object that was used before does not carry its old contents into a new lookup.
  TableGetResult reused;
  reused.kind = TableGetResult::Kind::kFound;
  reused.value = "stale";
  ASSERT_OK(table->Get({}, IKey("zzz", kMaxSequenceNumber), &reused));
  EXPECT_EQ(reused.kind, TableGetResult::Kind::kNotFound);
  EXPECT_TRUE(reused.value.empty());
}

TEST_F(TableTest, GetPicksTheVersionTheSequenceNumberSees) {
  // Newest first, as they sort: a value, a deletion and an older value of the same key, then
  // another key whose versions also cross blocks.
  Entries entries;
  entries.emplace_back(IKey("a", 9), "new");
  entries.emplace_back(IKey("a", 5, ValueType::kDeletion), "");
  entries.emplace_back(IKey("a", 2), "old");
  entries.emplace_back(IKey("b", 3), "b3");
  for (const size_t block_size : {size_t{4096}, size_t{1}}) {
    TableBuilderOptions options;
    options.block_size = block_size;
    const std::shared_ptr<Table> table = BuildAndOpen(entries, nullptr, options);
    ASSERT_NE(table, nullptr);
    struct Case {
      SequenceNumber sequence;
      TableGetResult::Kind kind;
      std::string value;
    };
    const std::vector<Case> cases = {
        {kMaxSequenceNumber, TableGetResult::Kind::kFound, "new"},
        {9, TableGetResult::Kind::kFound, "new"},
        {8, TableGetResult::Kind::kDeleted, ""},
        {5, TableGetResult::Kind::kDeleted, ""},
        {4, TableGetResult::Kind::kFound, "old"},
        {2, TableGetResult::Kind::kFound, "old"},
        {1, TableGetResult::Kind::kNotFound, ""},
    };
    for (const Case& c : cases) {
      const Lookup got = Get(*table, "a", c.sequence);
      ASSERT_OK(got.status);
      EXPECT_EQ(got.result.kind, c.kind)
          << "block size " << block_size << " sequence " << c.sequence;
      EXPECT_EQ(got.result.value, c.value);
    }
  }
}

// The brief's check, with the absent keys inside the range of the present ones. Keys that sort
// after every present key never reach a data block, whether there is a filter or not, so they
// would pass without one. These sort between present keys, where only the filter turns a lookup
// away from the block that would otherwise be read.
TEST_F(TableTest, BloomSkipsDataBlocksForAbsentKeys) {
  Entries entries;
  for (int i = 0; i < 10000; ++i) {
    entries.emplace_back(IKey("m" + UserKey(2 * i), static_cast<SequenceNumber>(i) + 1), "v");
  }
  const std::shared_ptr<Table> table = BuildAndOpen(entries);
  ASSERT_NE(table, nullptr);

  const uint64_t before = table->data_block_reads();
  for (int i = 0; i < 1000; ++i) {
    const Lookup absent = Get(*table, "m" + UserKey(2 * i + 1), kMaxSequenceNumber);
    ASSERT_OK(absent.status);
    EXPECT_EQ(absent.result.kind, TableGetResult::Kind::kNotFound);
  }
  const uint64_t reads = table->data_block_reads() - before;
  EXPECT_LE(reads, uint64_t{50})
      << "1000 absent keys should pass a 10-bit filter about 1% of the time";

  // And present keys do read their block, one each: the counter counts.
  const uint64_t before_present = table->data_block_reads();
  for (int i = 0; i < 100; ++i) {
    const Lookup present = Get(*table, "m" + UserKey(2 * (i * 97)), kMaxSequenceNumber);
    ASSERT_OK(present.status);
    EXPECT_EQ(present.result.kind, TableGetResult::Kind::kFound);
  }
  EXPECT_EQ(table->data_block_reads() - before_present, uint64_t{100});
}

TEST_F(TableTest, WithoutAFilterEveryAbsentKeyInRangeReadsABlock) {
  // The control for the test above: the same lookups, in a table written without a filter, read
  // a block each. If they did not, the test above would prove nothing.
  Entries entries;
  for (int i = 0; i < 10000; ++i) {
    entries.emplace_back(IKey("m" + UserKey(2 * i), static_cast<SequenceNumber>(i) + 1), "v");
  }
  TableBuilderOptions options;
  options.bloom_bits_per_key = 0;
  const std::shared_ptr<Table> table = BuildAndOpen(entries, nullptr, options);
  ASSERT_NE(table, nullptr);
  const uint64_t before = table->data_block_reads();
  for (int i = 0; i < 1000; ++i) {
    const Lookup absent = Get(*table, "m" + UserKey(2 * i + 1), kMaxSequenceNumber);
    ASSERT_OK(absent.status);
    EXPECT_EQ(absent.result.kind, TableGetResult::Kind::kNotFound);
  }
  EXPECT_EQ(table->data_block_reads() - before, uint64_t{1000});
}

TEST_F(TableTest, BloomFilterDoesNotHideKeysThatArePresent) {
  // Every version of every key is found through the filter: it holds user keys, and a lookup asks
  // with one.
  Entries entries;
  for (int i = 0; i < 2000; ++i) {
    entries.emplace_back(IKey(UserKey(i), 20), "new" + std::to_string(i));
    entries.emplace_back(IKey(UserKey(i), 10), "old" + std::to_string(i));
  }
  const std::shared_ptr<Table> table = BuildAndOpen(entries);
  ASSERT_NE(table, nullptr);
  for (int i = 0; i < 2000; ++i) {
    const Lookup newest = Get(*table, UserKey(i), kMaxSequenceNumber);
    ASSERT_OK(newest.status);
    ASSERT_EQ(newest.result.kind, TableGetResult::Kind::kFound) << i;
    EXPECT_EQ(newest.result.value, "new" + std::to_string(i));
    const Lookup older = Get(*table, UserKey(i), 15);
    ASSERT_OK(older.status);
    ASSERT_EQ(older.result.kind, TableGetResult::Kind::kFound) << i;
    EXPECT_EQ(older.result.value, "old" + std::to_string(i));
  }
}

TEST_F(TableTest, BlockCacheServesRepeatReads) {
  BlockCache cache(1 << 20);
  const std::shared_ptr<Table> table = BuildAndOpen(NumberedEntries(10000), &cache);
  ASSERT_NE(table, nullptr);

  const uint64_t before = table->data_block_reads();
  const Lookup first = Get(*table, "k004321", kMaxSequenceNumber);
  ASSERT_OK(first.status);
  EXPECT_EQ(first.result.value, ValueOf(4321));
  EXPECT_EQ(table->data_block_reads(), before + 1);
  const Lookup second = Get(*table, "k004321", kMaxSequenceNumber);
  ASSERT_OK(second.status);
  EXPECT_EQ(second.result.value, ValueOf(4321));
  EXPECT_EQ(table->data_block_reads(), before + 1) << "the second lookup is served from the cache";
  EXPECT_GT(cache.TotalCharge(), size_t{0});

  // A neighbour in the same block is served from the cache too.
  const Lookup neighbour = Get(*table, "k004322", kMaxSequenceNumber);
  ASSERT_OK(neighbour.status);
  EXPECT_EQ(neighbour.result.value, ValueOf(4322));
  EXPECT_EQ(table->data_block_reads(), before + 1);
}

TEST_F(TableTest, IteratorsFillAndUseTheBlockCache) {
  BlockCache cache(8 << 20);
  const Entries entries = NumberedEntries(10000);
  const std::shared_ptr<Table> table = BuildAndOpen(entries, &cache);
  ASSERT_NE(table, nullptr);
  const uint64_t before = table->data_block_reads();
  {
    std::unique_ptr<Iterator> it = table->NewIterator({});
    EXPECT_EQ(Drain(it.get()).entries, entries);
  }
  const uint64_t first_pass = table->data_block_reads() - before;
  EXPECT_GT(first_pass, uint64_t{10});
  {
    std::unique_ptr<Iterator> it = table->NewIterator({});
    EXPECT_EQ(Drain(it.get()).entries, entries);
  }
  EXPECT_EQ(table->data_block_reads() - before, first_pass) << "the second pass reads no block";
}

TEST_F(TableTest, FillCacheOffReadsWithoutKeepingTheBlock) {
  BlockCache cache(1 << 20);
  const std::shared_ptr<Table> table = BuildAndOpen(NumberedEntries(10000), &cache);
  ASSERT_NE(table, nullptr);
  TableReadOptions options;
  options.fill_cache = false;
  const uint64_t before = table->data_block_reads();
  for (int i = 0; i < 3; ++i) {
    const Lookup got = Get(*table, "k001234", kMaxSequenceNumber, options);
    ASSERT_OK(got.status);
    EXPECT_EQ(got.result.value, ValueOf(1234));
  }
  EXPECT_EQ(table->data_block_reads(), before + 3)
      << "nothing was inserted, so every read is a read";
  EXPECT_EQ(cache.TotalCharge(), size_t{0});
  {
    std::unique_ptr<Iterator> it = table->NewIterator(options);
    EXPECT_EQ(Drain(it.get()).entries.size(), size_t{10000});
  }
  EXPECT_EQ(cache.TotalCharge(), size_t{0}) << "an iterator that was told not to fill does not";

  // A block that is already cached is used whatever fill_cache says.
  ASSERT_OK(Get(*table, "k001234", kMaxSequenceNumber).status);
  const uint64_t after_fill = table->data_block_reads();
  ASSERT_OK(Get(*table, "k001234", kMaxSequenceNumber, options).status);
  EXPECT_EQ(table->data_block_reads(), after_fill);
}

TEST_F(TableTest, TablesSharingACacheDoNotSeeEachOthersBlocks) {
  BlockCache cache(8 << 20);
  Entries a;
  Entries b;
  for (int i = 0; i < 500; ++i) {
    a.emplace_back(IKey(UserKey(i), 1), "from-a-" + std::to_string(i));
    b.emplace_back(IKey(UserKey(i), 1), "from-b-" + std::to_string(i));
  }
  BuildTable("/a.sst", a);
  BuildTable("/b.sst", b);
  std::shared_ptr<Table> table_a;
  std::shared_ptr<Table> table_b;
  ASSERT_OK(OpenTable("/a.sst", &cache, &table_a, 1));
  ASSERT_OK(OpenTable("/b.sst", &cache, &table_b, 2));
  for (int i = 0; i < 500; i += 7) {
    const Lookup from_a = Get(*table_a, UserKey(i), kMaxSequenceNumber);
    const Lookup from_b = Get(*table_b, UserKey(i), kMaxSequenceNumber);
    ASSERT_OK(from_a.status);
    ASSERT_OK(from_b.status);
    EXPECT_EQ(from_a.result.value, "from-a-" + std::to_string(i));
    EXPECT_EQ(from_b.result.value, "from-b-" + std::to_string(i));
  }
}

TEST_F(TableTest, IteratorKeepsTheTableAlive) {
  // The iterator holds the table, as it says: its file, its index and its filter are used after
  // every other owner is gone.
  BlockCache cache(1 << 20);
  const Entries entries = NumberedEntries(2000);
  std::shared_ptr<Table> table = BuildAndOpen(entries, &cache);
  ASSERT_NE(table, nullptr);
  std::unique_ptr<Iterator> it = table->NewIterator({});
  table.reset();
  const Drained drained = Drain(it.get());
  EXPECT_OK(drained.status);
  EXPECT_EQ(drained.entries, entries);
}

TEST_F(TableTest, BlockOfALiveIteratorSurvivesItsEviction) {
  // A cache that holds one block at a time, and an iterator that outlives each block it was in.
  BlockCache cache(4096 + 512, 0);
  const Entries entries = NumberedEntries(3000);
  const std::shared_ptr<Table> table = BuildAndOpen(entries, &cache);
  ASSERT_NE(table, nullptr);
  std::unique_ptr<Iterator> it = table->NewIterator({});
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  const std::string first_key(it->key());
  ASSERT_EQ(first_key, entries[0].first);
  // Evict the iterator's block by reading others, then read on from where the iterator was.
  for (int i = 2500; i < 2600; i += 50) {
    ASSERT_OK(Get(*table, UserKey(i), kMaxSequenceNumber).status);
  }
  EXPECT_EQ(it->key(), first_key);
  const Drained drained = Drain(it.get(), 1000000, false);
  EXPECT_OK(drained.status);
  EXPECT_EQ(drained.entries, entries) << "read on from the block the cache no longer holds";
}

TEST_F(TableTest, EmptyTable) {
  const std::shared_ptr<Table> table = BuildAndOpen({});
  ASSERT_NE(table, nullptr);
  std::unique_ptr<Iterator> it = table->NewIterator({});
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
  it->Seek(IKey("a", 1));
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
  const Lookup got = Get(*table, "a", kMaxSequenceNumber);
  ASSERT_OK(got.status);
  EXPECT_EQ(got.result.kind, TableGetResult::Kind::kNotFound);
  EXPECT_EQ(table->data_block_reads(), uint64_t{0});
}

TEST_F(TableTest, SingleEntryAndEmptyUserKey) {
  Entries entries;
  entries.emplace_back(IKey("", 1), "");
  const std::shared_ptr<Table> table = BuildAndOpen(entries);
  ASSERT_NE(table, nullptr);

  const Lookup got = Get(*table, "", kMaxSequenceNumber);
  ASSERT_OK(got.status);
  EXPECT_EQ(got.result.kind, TableGetResult::Kind::kFound);
  EXPECT_TRUE(got.result.value.empty());
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  EXPECT_OK(drained.status);
  EXPECT_EQ(drained.entries, entries);
  const Lookup other = Get(*table, "a", kMaxSequenceNumber);
  ASSERT_OK(other.status);
  EXPECT_EQ(other.result.kind, TableGetResult::Kind::kNotFound);
}

TEST_F(TableTest, KeysAndValuesOfAnyBytes) {
  // Zero bytes, newlines and bytes above 0x7f in keys and values, and a value larger than a block.
  Random rnd(7);
  std::map<std::string, std::string> by_user_key;
  while (by_user_key.size() < 300) {
    by_user_key[RandomBinaryString(&rnd, rnd.Uniform(40))] =
        RandomBinaryString(&rnd, rnd.Skewed(13));
  }
  Entries entries;
  SequenceNumber sequence = 1;
  for (const auto& [user_key, value] : by_user_key) {
    entries.emplace_back(IKey(user_key, sequence++), value);
  }
  const std::shared_ptr<Table> table = BuildAndOpen(entries);
  ASSERT_NE(table, nullptr);
  std::unique_ptr<Iterator> it = table->NewIterator({});
  EXPECT_EQ(Drain(it.get()).entries, entries);
  for (const auto& [user_key, value] : by_user_key) {
    const Lookup got = Get(*table, user_key, kMaxSequenceNumber);
    ASSERT_OK(got.status);
    ASSERT_EQ(got.result.kind, TableGetResult::Kind::kFound);
    EXPECT_EQ(got.result.value, value);
  }
}

// Checks Get() against a model, over tables of many shapes: for each user key the model keeps its
// versions, oldest first, and a lookup at a sequence number must return the newest version at or
// below it.
TEST_F(TableTest, GetAgreesWithAModelOverRandomTables) {
  struct Version {
    SequenceNumber sequence;
    bool deletion;
    std::string value;
  };
  for (uint64_t seed = 0; seed < 12; ++seed) {
    Random rnd(seed);
    std::map<std::string, std::vector<Version>> model;
    const int keys = 1 + static_cast<int>(rnd.Uniform(300));
    SequenceNumber sequence = 1;
    for (int k = 0; k < keys; ++k) {
      std::vector<Version>& versions = model[RandomString(&rnd, rnd.Uniform(12))];
      const uint32_t count = 1 + rnd.Uniform(4);
      for (uint32_t v = 0; v < count; ++v) {
        const bool deletion = rnd.OneIn(4);
        versions.push_back(
            {sequence++, deletion, deletion ? std::string() : RandomString(&rnd, rnd.Skewed(9))});
      }
    }
    // The table's order: user key ascending, and for one key the newest version first.
    Entries entries;
    for (const auto& [user_key, versions] : model) {
      for (auto v = versions.rbegin(); v != versions.rend(); ++v) {
        entries.emplace_back(
            IKey(user_key, v->sequence, v->deletion ? ValueType::kDeletion : ValueType::kValue),
            v->value);
      }
    }
    TableBuilderOptions options;
    options.block_size = 1 + rnd.Uniform(700);
    options.block_restart_interval = 1 + static_cast<int>(rnd.Uniform(20));
    options.bloom_bits_per_key = rnd.OneIn(4) ? 0 : 10;
    BlockCache cache(size_t{1} << 20);
    const std::shared_ptr<Table> table = BuildAndOpen(entries, rnd.OneIn(3) ? nullptr : &cache,
                                                      options, "/model-" + std::to_string(seed));
    ASSERT_NE(table, nullptr) << "seed " << seed;

    for (const auto& [user_key, versions] : model) {
      for (SequenceNumber at = 0; at <= sequence + 1; at += 1 + rnd.Uniform(3)) {
        const Lookup got = Get(*table, user_key, at);
        ASSERT_OK(got.status);
        const Version* expected = nullptr;
        for (const Version& version : versions) {
          if (version.sequence <= at) {
            expected = &version;
          }
        }
        if (expected == nullptr) {
          EXPECT_EQ(got.result.kind, TableGetResult::Kind::kNotFound)
              << "seed " << seed << " at " << at;
        } else if (expected->deletion) {
          EXPECT_EQ(got.result.kind, TableGetResult::Kind::kDeleted)
              << "seed " << seed << " at " << at;
        } else {
          ASSERT_EQ(got.result.kind, TableGetResult::Kind::kFound)
              << "seed " << seed << " at " << at;
          EXPECT_EQ(got.result.value, expected->value) << "seed " << seed << " at " << at;
        }
      }
    }
    // What is not there is not found: the model's keys are at most 11 bytes long.
    for (int i = 0; i < 50; ++i) {
      const Lookup got = Get(*table, RandomString(&rnd, 13 + rnd.Uniform(3)), kMaxSequenceNumber);
      ASSERT_OK(got.status);
      EXPECT_EQ(got.result.kind, TableGetResult::Kind::kNotFound);
    }
    std::unique_ptr<Iterator> it = table->NewIterator({});
    const Drained drained = Drain(it.get());
    EXPECT_OK(drained.status);
    EXPECT_EQ(drained.entries, entries) << "seed " << seed;
  }
}

TEST_F(TableTest, FlippedDataByteIsCorruptionNotWrongValue) {
  const Entries entries = NumberedEntries(10000);
  BuildTable("/good.sst", entries);
  std::string bytes = ReadFile("/good.sst");
  bytes[3] = static_cast<char>(bytes[3] ^ 0x01);  // inside the first data block
  WriteFile("/bad.sst", bytes);
  std::shared_ptr<Table> table;
  ASSERT_OK(OpenTable("/bad.sst", nullptr, &table)) << "the index and filter are intact";

  const Lookup damaged = Get(*table, "k000000", kMaxSequenceNumber);
  EXPECT_TRUE(damaged.status.IsCorruption()) << damaged.status.ToString();
  EXPECT_NE(damaged.result.kind, TableGetResult::Kind::kFound);

  // A full iteration stops, with the error.
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  EXPECT_TRUE(drained.status.IsCorruption()) << drained.status.ToString();
  EXPECT_TRUE(drained.entries.empty()) << "the damaged block is the first, so nothing precedes it";

  // The other blocks are fine.
  for (const int i : {5000, 9999}) {
    const Lookup ok = Get(*table, UserKey(i), kMaxSequenceNumber);
    ASSERT_OK(ok.status);
    EXPECT_EQ(ok.result.kind, TableGetResult::Kind::kFound);
    EXPECT_EQ(ok.result.value, ValueOf(i));
  }
  it = table->NewIterator({});
  it->Seek(IKey("k005000", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value(), ValueOf(5000));
}

TEST_F(TableTest, DamageInALaterBlockEndsAnIterationAfterTheEntriesBeforeIt) {
  const Entries entries = NumberedEntries(10000);
  BuildTable("/good.sst", entries);
  // Find the third data block by reading the table with its own parts.
  std::shared_ptr<Table> good;
  ASSERT_OK(OpenTable("/good.sst", nullptr, &good));
  std::string bytes = ReadFile("/good.sst");
  // The first block's size is in the first index entry; rather than parse the index, damage the
  // file a little over two blocks in, which is inside the third block for 4 KiB blocks.
  const size_t position = 2 * 4096 + 1000;
  bytes[position] = static_cast<char>(bytes[position] ^ 0x20);
  WriteFile("/bad.sst", bytes);
  std::shared_ptr<Table> table;
  ASSERT_OK(OpenTable("/bad.sst", nullptr, &table));
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  EXPECT_TRUE(drained.status.IsCorruption()) << drained.status.ToString();
  ASSERT_FALSE(drained.entries.empty());
  ASSERT_LT(drained.entries.size(), entries.size());
  EXPECT_TRUE(std::equal(drained.entries.begin(), drained.entries.end(), entries.begin()))
      << "what came before the damage is what was written";
}

TEST_F(TableTest, CorruptBlockIsNotCached) {
  BlockCache cache(1 << 20);
  const Entries entries = NumberedEntries(10000);
  BuildTable("/good.sst", entries);
  std::string bytes = ReadFile("/good.sst");
  bytes[3] = static_cast<char>(bytes[3] ^ 0x01);
  WriteFile("/bad.sst", bytes);
  std::shared_ptr<Table> table;
  ASSERT_OK(OpenTable("/bad.sst", &cache, &table));
  const uint64_t before = table->data_block_reads();
  EXPECT_TRUE(Get(*table, "k000000", kMaxSequenceNumber).status.IsCorruption());
  EXPECT_TRUE(Get(*table, "k000000", kMaxSequenceNumber).status.IsCorruption());
  EXPECT_EQ(table->data_block_reads(), before + 2) << "a block that failed is read again, not kept";
  EXPECT_EQ(cache.TotalCharge(), size_t{0});
}

TEST_F(TableTest, UnverifiedReadDoesNotLeaveAnUnverifiedBlockInTheCache) {
  const std::string stored(40, 'v');
  Entries entries;
  entries.emplace_back(IKey("a", 1), stored);
  entries.emplace_back(IKey("b", 2), "other");
  BuildTable("/good.sst", entries);
  std::string bytes = ReadFile("/good.sst");
  const size_t at = bytes.find(stored);
  ASSERT_NE(at, std::string::npos);
  bytes[at + 10] = static_cast<char>(bytes[at + 10] ^ 0x01);  // inside the value
  WriteFile("/bad.sst", bytes);

  BlockCache cache(1 << 20);
  std::shared_ptr<Table> table;
  ASSERT_OK(OpenTable("/bad.sst", &cache, &table));
  TableReadOptions unverified;
  unverified.verify_checksums = false;

  // The reader that asked for no checking gets what is in the file, changed byte and all.
  const Lookup first = Get(*table, "a", kMaxSequenceNumber, unverified);
  ASSERT_OK(first.status);
  ASSERT_EQ(first.result.kind, TableGetResult::Kind::kFound);
  EXPECT_NE(first.result.value, stored);
  EXPECT_EQ(cache.TotalCharge(), size_t{0}) << "a block that was not checked is not kept";

  // A reader that asks for checking is not given that block by the cache.
  const Lookup second = Get(*table, "a", kMaxSequenceNumber);
  EXPECT_TRUE(second.status.IsCorruption()) << second.status.ToString();
  EXPECT_NE(second.result.kind, TableGetResult::Kind::kFound)
      << "a value that was never stored came back as found: " << second.result.value;

  // The same for iterators, in either order.
  {
    std::unique_ptr<Iterator> it = table->NewIterator(unverified);
    EXPECT_EQ(Drain(it.get()).entries.size(), entries.size());
  }
  EXPECT_EQ(cache.TotalCharge(), size_t{0});
  {
    std::unique_ptr<Iterator> it = table->NewIterator({});
    const Drained drained = Drain(it.get());
    EXPECT_TRUE(drained.status.IsCorruption()) << drained.status.ToString();
    EXPECT_TRUE(drained.entries.empty());
  }
}

TEST_F(TableTest, UnverifiedReadStillUsesABlockThatWasVerified) {
  // The other direction costs nothing and is kept: a block a verifying read cached has been
  // checked, so a read that does not check may use it.
  BlockCache cache(1 << 20);
  const std::shared_ptr<Table> table = BuildAndOpen(NumberedEntries(2000), &cache);
  ASSERT_NE(table, nullptr);
  ASSERT_OK(Get(*table, "k000100", kMaxSequenceNumber).status);
  const uint64_t reads = table->data_block_reads();
  TableReadOptions unverified;
  unverified.verify_checksums = false;
  const Lookup got = Get(*table, "k000100", kMaxSequenceNumber, unverified);
  ASSERT_OK(got.status);
  EXPECT_EQ(got.result.value, ValueOf(100));
  EXPECT_EQ(table->data_block_reads(), reads);
}

TEST_F(TableTest, TruncatedFileIsCorruption) {
  const uint64_t size = BuildTable("/t.sst", NumberedEntries(1000));
  const std::string bytes = ReadFile("/t.sst");
  ASSERT_EQ(bytes.size(), size);
  WriteFile("/cut.sst", bytes.substr(0, bytes.size() - 10));
  std::shared_ptr<Table> table;
  const Status s = OpenTable("/cut.sst", nullptr, &table);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(table, nullptr);
}

TEST_F(TableTest, EveryPrefixOfATableIsCorruption) {
  const uint64_t size = BuildTable("/t.sst", NumberedEntries(40));
  const std::string bytes = ReadFile("/t.sst");
  ASSERT_EQ(bytes.size(), size);
  for (size_t length = 0; length < bytes.size(); ++length) {
    WriteFile("/cut.sst", bytes.substr(0, length));
    std::shared_ptr<Table> table;
    const Status s = OpenTable("/cut.sst", nullptr, &table);
    EXPECT_TRUE(s.IsCorruption()) << "a prefix of " << length << " bytes: " << s.ToString();
    EXPECT_EQ(table, nullptr);
  }
}

TEST_F(TableTest, FileSizeThatDisagreesWithTheFileIsCorruption) {
  const uint64_t size = BuildTable("/t.sst", NumberedEntries(40));
  for (const uint64_t claimed : {size + 1, size + 100, size - 1, size - 48, uint64_t{0}}) {
    std::unique_ptr<RandomAccessFile> file;
    ASSERT_OK(env_->NewRandomAccessFile("/t.sst", &file));
    std::shared_ptr<Table> table;
    const Status s = Table::Open(std::move(file), claimed, 1, nullptr, &table);
    EXPECT_TRUE(s.IsCorruption()) << "claimed size " << claimed << ": " << s.ToString();
  }
}

TEST_F(TableTest, RandomBytesAreCorruption) {
  Random rnd(11);
  WriteFile("/random.sst", RandomBinaryString(&rnd, 4096));
  std::shared_ptr<Table> table;
  const Status s = OpenTable("/random.sst", nullptr, &table);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(table, nullptr);

  // Files of every small size, and some large ones, from many seeds.
  for (uint64_t seed = 0; seed < 300; ++seed) {
    Random r(seed);
    const size_t length = seed < 150 ? static_cast<size_t>(seed) : r.Uniform(5000);
    WriteFile("/random.sst", RandomBinaryString(&r, length));
    std::shared_ptr<Table> t;
    EXPECT_TRUE(OpenTable("/random.sst", nullptr, &t).IsCorruption()) << "seed " << seed;
  }
}

// A table in a FaultInjectionEnv, so that a read of its file can be made to fail. The file is
// sound: what fails is the disk, and a table that cannot be read is not a table that is damaged.
class TableIOErrorTest : public testing::Test {
 protected:
  void SetUp() override {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(env_.NewWritableFile("/t.sst", &file));
    TableBuilderOptions options;
    options.block_size = 512;
    TableBuilder builder(options, file.get());
    for (const auto& [key, value] : NumberedEntries(500)) {
      builder.Add(key, value);
    }
    ASSERT_OK(builder.Finish());
    ASSERT_OK(file->Close());
    ASSERT_OK(env_.GetFileSize("/t.sst", &size_));
  }

  Status OpenTable(BlockCache* cache, std::shared_ptr<Table>* table) {
    std::unique_ptr<RandomAccessFile> file;
    Status s = env_.NewRandomAccessFile("/t.sst", &file);
    if (!s.ok()) {
      return s;
    }
    return Table::Open(std::move(file), size_, 1, cache, table);
  }

  FaultInjectionEnv env_{1};
  uint64_t size_ = 0;
};

TEST_F(TableIOErrorTest, FailedReadWhileOpeningIsAnIOError) {
  // Open reads the footer, then the index block, then the filter block. A failure of any of them
  // is the disk's, whatever the table is.
  for (int reads_before = 0; reads_before < 3; ++reads_before) {
    env_.FailOpAfter(FaultInjectionEnv::Op::kRead, reads_before);
    std::shared_ptr<Table> table;
    const Status s = OpenTable(nullptr, &table);
    EXPECT_TRUE(s.IsIOError()) << "failure of read " << reads_before << ": " << s.ToString();
    EXPECT_EQ(table, nullptr);
    env_.ClearFaults();
  }
  std::shared_ptr<Table> table;
  ASSERT_OK(OpenTable(nullptr, &table)) << "and the table opens when the disk does not fail";
}

TEST_F(TableIOErrorTest, FailedReadInGetIsAnIOErrorAndNothingIsRemembered) {
  BlockCache cache(1 << 20);
  std::shared_ptr<Table> table;
  ASSERT_OK(OpenTable(&cache, &table));

  env_.FailOpAfter(FaultInjectionEnv::Op::kRead, 0);
  TableGetResult result;
  const Status failed = table->Get({}, IKey(UserKey(250), kMaxSequenceNumber), &result);
  EXPECT_TRUE(failed.IsIOError()) << failed.ToString();
  EXPECT_NE(result.kind, TableGetResult::Kind::kFound);
  EXPECT_EQ(cache.TotalCharge(), size_t{0});

  // The failure was one read; the next lookup of the same key reads the block and finds it.
  ASSERT_OK(table->Get({}, IKey(UserKey(250), kMaxSequenceNumber), &result));
  EXPECT_EQ(result.kind, TableGetResult::Kind::kFound);
  EXPECT_EQ(result.value, ValueOf(250));
}

TEST_F(TableIOErrorTest, FailedReadInAnIteratorIsAnIOError) {
  std::shared_ptr<Table> table;
  ASSERT_OK(OpenTable(nullptr, &table));
  // The first read of the iteration succeeds and a later one fails.
  env_.FailOpAfter(FaultInjectionEnv::Op::kRead, 2);
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  EXPECT_TRUE(drained.status.IsIOError()) << drained.status.ToString();
  EXPECT_FALSE(drained.entries.empty()) << "what came before the failure was returned";
  EXPECT_LT(drained.entries.size(), size_t{500});
  // And it is the end: the iterator does not skip the block that failed.
  const Entries all = NumberedEntries(500);
  EXPECT_TRUE(std::equal(drained.entries.begin(), drained.entries.end(), all.begin()));
}

// ---- Tables with checksums that pass and contents that are wrong ----

// Builds a file out of blocks written by hand, each with a trailer that is right for what is in
// it, so that the damage a test puts in a block is found by the reader's own decoding of the
// block and not by a checksum.
class RawTable {
 public:
  BlockHandle AddBlock(std::string_view contents) {
    const BlockHandle handle{file_.size(), contents.size()};
    file_.append(contents);
    std::string trailer(1, '\0');
    const uint32_t crc =
        crc32c::Extend(crc32c::Value(contents.data(), contents.size()), trailer.data(), 1);
    PutFixed32(&trailer, crc32c::Mask(crc));
    file_.append(trailer);
    return handle;
  }

  // A block with one entry for each of `entries`, which are in the order given.
  BlockHandle AddEntries(const Entries& entries, int restart_interval = 1) {
    BlockBuilder builder(restart_interval);
    for (const auto& [key, value] : entries) {
      builder.Add(key, value);
    }
    return AddBlock(builder.Finish());
  }

  // The index block for data blocks given as (last key, handle).
  BlockHandle AddIndex(const std::vector<std::pair<std::string, BlockHandle>>& blocks) {
    Entries entries;
    for (const auto& [key, handle] : blocks) {
      std::string encoded;
      handle.EncodeTo(&encoded);
      entries.emplace_back(key, encoded);
    }
    return AddEntries(entries);
  }

  // The file, ended with a footer that names `filter` and `index`.
  std::string Finish(const BlockHandle& filter, const BlockHandle& index) {
    Footer footer;
    footer.filter = filter;
    footer.index = index;
    footer.EncodeTo(&file_);
    return file_;
  }

 private:
  std::string file_;
};

class HostileTableTest : public TableTest {
 protected:
  // Opens the table in `bytes`.
  Status Open(const std::string& bytes, std::shared_ptr<Table>* table) {
    WriteFile("/hostile.sst", bytes);
    return OpenTable("/hostile.sst", nullptr, table);
  }

  void ExpectOpenFails(const std::string& bytes, const std::string& what) {
    std::shared_ptr<Table> table;
    const Status s = Open(bytes, &table);
    EXPECT_TRUE(s.IsCorruption()) << what << ": " << s.ToString();
    EXPECT_EQ(table, nullptr) << what;
  }

  // A table of one data block with the keys and values of `block`, which `last_key` is the index's
  // word for the last key of.
  std::string OneBlockTable(const Entries& block, const std::string& last_key,
                            const std::string& filter = "") {
    RawTable raw;
    const BlockHandle data = raw.AddEntries(block, 16);
    const BlockHandle filter_handle = raw.AddBlock(filter);
    const BlockHandle index = raw.AddIndex({{last_key, data}});
    return raw.Finish(filter_handle, index);
  }
};

TEST_F(HostileTableTest, WellFormedRawTableIsReadable) {
  // The control for the rest: the raw writer makes a table the reader accepts.
  const Entries block = {{IKey("a", 5), "va"}, {IKey("b", 6), "vb"}};
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(OneBlockTable(block, block.back().first), &table));
  const Lookup got = Get(*table, "b", kMaxSequenceNumber);
  ASSERT_OK(got.status);
  EXPECT_EQ(got.result.kind, TableGetResult::Kind::kFound);
  EXPECT_EQ(got.result.value, "vb");
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  EXPECT_OK(drained.status);
  EXPECT_EQ(drained.entries, block);
}

TEST_F(HostileTableTest, IndexEntryThatIsNotAHandle) {
  RawTable raw;
  const BlockHandle data = raw.AddEntries({{IKey("a", 1), "v"}}, 16);
  const BlockHandle filter = raw.AddBlock("");
  const BlockHandle index = raw.AddEntries({{IKey("a", 1), std::string(11, '\xff')}});
  ExpectOpenFails(raw.Finish(filter, index), "a value of 11 bytes that all continue a varint");
  (void)data;

  RawTable trailing;
  const BlockHandle data2 = trailing.AddEntries({{IKey("a", 1), "v"}}, 16);
  const BlockHandle filter2 = trailing.AddBlock("");
  std::string encoded;
  data2.EncodeTo(&encoded);
  encoded += "extra";
  const BlockHandle index2 = trailing.AddEntries({{IKey("a", 1), encoded}});
  ExpectOpenFails(trailing.Finish(filter2, index2), "a handle followed by other bytes");

  RawTable empty_value;
  const BlockHandle filter3 = empty_value.AddBlock("");
  const BlockHandle index3 = empty_value.AddEntries({{IKey("a", 1), ""}});
  ExpectOpenFails(empty_value.Finish(filter3, index3), "an empty value");
}

TEST_F(HostileTableTest, IndexHandleOutsideTheDataOfTheFile) {
  const std::string key = IKey("a", 1);
  struct Case {
    BlockHandle handle;
    const char* what;
  };
  for (const Case& c : {Case{{uint64_t{1} << 40, 10}, "far beyond the end"},
                        Case{{0, uint64_t{1} << 62}, "a size no file holds"},
                        Case{{~uint64_t{0}, ~uint64_t{0}}, "the largest handle there is"},
                        Case{{~uint64_t{0} - 2, 10}, "an end that overflows"}}) {
    RawTable raw;
    raw.AddEntries({{key, "v"}}, 16);
    const BlockHandle filter = raw.AddBlock("");
    const BlockHandle index = raw.AddIndex({{key, c.handle}});
    ExpectOpenFails(raw.Finish(filter, index), c.what);
  }
}

TEST_F(HostileTableTest, IndexHandleNamingAnotherKindOfBlock) {
  // A handle that points at the filter block or the index block would have the reader decode one
  // as the other, and a lookup could then return the bytes of a handle as a value.
  const std::string key = IKey("a", 1);
  {
    RawTable raw;
    raw.AddEntries({{key, "v"}}, 16);
    const BlockHandle filter = raw.AddBlock("a filter");
    const BlockHandle index = raw.AddIndex({{key, filter}});
    ExpectOpenFails(raw.Finish(filter, index), "an index entry naming the filter block");
  }
  {
    RawTable raw;
    const BlockHandle data = raw.AddEntries({{key, "v"}}, 16);
    const BlockHandle filter = raw.AddBlock("");
    const BlockHandle index = raw.AddIndex({{key, data}});
    ExpectOpenFails(raw.Finish(index, filter), "the filter and index handles swapped");
  }
  {
    // One block as both: its bytes would be read as a filter, and say what they like about keys.
    RawTable raw;
    const BlockHandle data = raw.AddEntries({{key, "v"}}, 16);
    const BlockHandle index = raw.AddIndex({{key, data}});
    ExpectOpenFails(raw.Finish(index, index), "the index block as the filter block too");
  }
}

TEST_F(HostileTableTest, IndexHandlesThatOverlapOrGoBackwards) {
  const std::string key_a = IKey("a", 1);
  const std::string key_b = IKey("b", 1);
  RawTable raw;
  const BlockHandle first = raw.AddEntries({{key_a, "v"}}, 16);
  const BlockHandle second = raw.AddEntries({{key_b, "v"}}, 16);
  const BlockHandle filter = raw.AddBlock("");
  {
    RawTable same = raw;
    const BlockHandle index = same.AddIndex({{key_a, first}, {key_b, first}});
    ExpectOpenFails(same.Finish(filter, index), "two index entries for one block");
  }
  {
    RawTable backwards = raw;
    const BlockHandle index = backwards.AddIndex({{key_a, second}, {key_b, first}});
    ExpectOpenFails(backwards.Finish(filter, index), "blocks listed out of the order of the file");
  }
  {
    RawTable ok = raw;
    const BlockHandle index = ok.AddIndex({{key_a, first}, {key_b, second}});
    std::shared_ptr<Table> table;
    ASSERT_OK(Open(ok.Finish(filter, index), &table)) << "the same two blocks in order are fine";
  }
}

TEST_F(HostileTableTest, IndexKeyThatIsNotAnInternalKey) {
  for (const std::string& bad_key : {std::string(), std::string("abc"), std::string(7, 'k')}) {
    RawTable raw;
    const BlockHandle data = raw.AddEntries({{IKey("a", 1), "v"}}, 16);
    const BlockHandle filter = raw.AddBlock("");
    const BlockHandle index = raw.AddIndex({{bad_key, data}});
    ExpectOpenFails(raw.Finish(filter, index),
                    "an index key of " + std::to_string(bad_key.size()) + " bytes");
  }
}

TEST_F(HostileTableTest, IndexKeysOutOfOrder) {
  RawTable raw;
  const BlockHandle first = raw.AddEntries({{IKey("b", 1), "v"}}, 16);
  const BlockHandle second = raw.AddEntries({{IKey("a", 1), "v"}}, 16);
  const BlockHandle filter = raw.AddBlock("");
  const BlockHandle index = raw.AddIndex({{IKey("b", 1), first}, {IKey("a", 1), second}});
  ExpectOpenFails(raw.Finish(filter, index), "index keys going down");

  RawTable repeated;
  const BlockHandle one = repeated.AddEntries({{IKey("a", 2), "v"}}, 16);
  const BlockHandle two = repeated.AddEntries({{IKey("a", 1), "v"}}, 16);
  const BlockHandle filter2 = repeated.AddBlock("");
  const BlockHandle index2 = repeated.AddIndex({{IKey("a", 2), one}, {IKey("a", 2), two}});
  ExpectOpenFails(repeated.Finish(filter2, index2), "the same index key twice");
}

TEST_F(HostileTableTest, IndexBlockThatIsNotABlock) {
  for (const std::string& contents : {std::string(), std::string("abc"), std::string(4, '\0'),
                                      std::string("\x05\0\0\0", 4), std::string(40, '\xff')}) {
    RawTable raw;
    raw.AddEntries({{IKey("a", 1), "v"}}, 16);
    const BlockHandle filter = raw.AddBlock("");
    const BlockHandle index = raw.AddBlock(contents);
    ExpectOpenFails(raw.Finish(filter, index),
                    "an index block of " + std::to_string(contents.size()) + " bytes");
  }
}

TEST_F(HostileTableTest, FooterHandlesOutsideTheFile) {
  RawTable base;
  const BlockHandle data = base.AddEntries({{IKey("a", 1), "v"}}, 16);
  const BlockHandle filter = base.AddBlock("");
  const BlockHandle index = base.AddIndex({{IKey("a", 1), data}});
  for (const BlockHandle& bad :
       {BlockHandle{uint64_t{1} << 40, 10}, BlockHandle{0, uint64_t{1} << 62},
        BlockHandle{~uint64_t{0}, 1}, BlockHandle{index.offset, index.size + 1},
        BlockHandle{index.offset + index.size + kBlockTrailerSize, 0}}) {
    {
      RawTable t = base;
      ExpectOpenFails(t.Finish(bad, index), "filter handle " + std::to_string(bad.offset) + "+" +
                                                std::to_string(bad.size));
    }
    {
      RawTable t = base;
      ExpectOpenFails(t.Finish(filter, bad), "index handle " + std::to_string(bad.offset) + "+" +
                                                 std::to_string(bad.size));
    }
  }
}

TEST_F(HostileTableTest, DataBlockWithKeysTooShortToBeInternalKeys) {
  // The index is sound, so the table opens; the block is read when a lookup or a scan gets there.
  const std::string last = IKey("z", 1);
  const Entries block = {{"ab", "v"}};
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(OneBlockTable(block, last), &table));
  EXPECT_TRUE(Get(*table, "a", kMaxSequenceNumber).status.IsCorruption());
  std::unique_ptr<Iterator> it = table->NewIterator({});
  EXPECT_TRUE(Drain(it.get()).status.IsCorruption());
  it->Seek(IKey("b", 1));
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());

  // The first key sound and a later one not.
  const Entries later = {{IKey("a", 1), "v"}, {"x", "v"}};
  ASSERT_OK(Open(OneBlockTable(later, last), &table));
  std::unique_ptr<Iterator> it2 = table->NewIterator({});
  const Drained drained = Drain(it2.get());
  EXPECT_TRUE(drained.status.IsCorruption());
  EXPECT_EQ(drained.entries.size(), size_t{1});
}

TEST_F(HostileTableTest, DataBlockWithAnImpossibleValueType) {
  // A tag whose type byte is neither a value nor a deletion. A lookup that reaches it does not
  // guess what the entry is.
  std::string bad_key = "a";
  PutFixed64(&bad_key, (uint64_t{5} << 8) | 7);
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(OneBlockTable({{bad_key, "v"}}, bad_key), &table));
  const Lookup got = Get(*table, "a", kMaxSequenceNumber);
  EXPECT_TRUE(got.status.IsCorruption()) << got.status.ToString();
  EXPECT_NE(got.result.kind, TableGetResult::Kind::kFound);
}

TEST_F(HostileTableTest, DataBlockThatDoesNotHoldTheKeyItsIndexEntryPromises) {
  // The index says this block ends at "m"; it holds only "a". A lookup for "m" is sent here and
  // finds nothing at or after it. That is a table that contradicts itself and not an absent key,
  // and a reader that answered "not found" would be wrong about a key the index vouches for.
  const Entries block = {{IKey("a", 1), "v"}};
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(OneBlockTable(block, IKey("m", 1)), &table));
  const Lookup got = Get(*table, "m", kMaxSequenceNumber);
  EXPECT_TRUE(got.status.IsCorruption()) << got.status.ToString();
  // A key that is before everything is still answered from the block.
  const Lookup before = Get(*table, "a", kMaxSequenceNumber);
  ASSERT_OK(before.status);
  EXPECT_EQ(before.result.kind, TableGetResult::Kind::kFound);
}

TEST_F(HostileTableTest, DataBlockThatIsNotABlock) {
  for (const std::string& contents :
       {std::string(), std::string("xyz"), std::string(64, '\xff'), std::string("\x01\0\0\0", 4)}) {
    RawTable raw;
    const BlockHandle data = raw.AddBlock(contents);
    const BlockHandle filter = raw.AddBlock("");
    const BlockHandle index = raw.AddIndex({{IKey("a", 1), data}});
    std::shared_ptr<Table> table;
    ASSERT_OK(Open(raw.Finish(filter, index), &table));
    EXPECT_TRUE(Get(*table, "a", kMaxSequenceNumber).status.IsCorruption())
        << "a data block of " << contents.size() << " bytes";
    std::unique_ptr<Iterator> it = table->NewIterator({});
    EXPECT_TRUE(Drain(it.get()).status.IsCorruption());
  }
}

// A table of the blocks given, each with the key the index names it by. The blocks are intact and
// their checksums are right; what is wrong with them is the order of their keys, which only the
// reader's own checks can see.
class OrderTableTest : public HostileTableTest {
 protected:
  std::string TableOf(const std::vector<std::pair<std::string, Entries>>& blocks) {
    RawTable raw;
    std::vector<std::pair<std::string, BlockHandle>> index_entries;
    for (const auto& [index_key, entries] : blocks) {
      index_entries.emplace_back(index_key, raw.AddEntries(entries, 16));
    }
    const BlockHandle filter = raw.AddBlock("");
    const BlockHandle index = raw.AddIndex(index_entries);
    return raw.Finish(filter, index);
  }

  // Iterates the table in `bytes` and returns what came back before the iteration stopped.
  Drained Iterate(const std::string& bytes) {
    std::shared_ptr<Table> table;
    EXPECT_OK(Open(bytes, &table));
    Drained drained;
    if (table != nullptr) {
      std::unique_ptr<Iterator> it = table->NewIterator({});
      drained = Drain(it.get());
    }
    return drained;
  }
};

TEST_F(OrderTableTest, KeysOutOfOrderInsideABlockEndTheIteration) {
  const Entries block = {{IKey("b", 1), "vb"}, {IKey("a", 1), "va"}};
  const Drained drained = Iterate(TableOf({{IKey("b", 1), block}}));
  EXPECT_TRUE(drained.status.IsCorruption()) << drained.status.ToString();
  EXPECT_EQ(drained.entries, (Entries{{IKey("b", 1), "vb"}}))
      << "up to the key that is out of order";

  // A repeated key is out of order too: the keys of a table are strictly increasing.
  const Entries twice = {{IKey("a", 1), "x"}, {IKey("a", 1), "y"}};
  const Drained repeated = Iterate(TableOf({{IKey("a", 1), twice}}));
  EXPECT_TRUE(repeated.status.IsCorruption());
  EXPECT_EQ(repeated.entries.size(), size_t{1});
}

TEST_F(OrderTableTest, FirstKeyOfABlockNotAfterTheEndOfThePreviousBlockEndsTheIteration) {
  // The second block starts with "b", which is before the "c" the first block ends at.
  const Drained drained =
      Iterate(TableOf({{IKey("c", 1), {{IKey("a", 1), "va"}, {IKey("c", 1), "vc"}}},
                       {IKey("d", 1), {{IKey("b", 1), "vb"}, {IKey("d", 1), "vd"}}}}));
  EXPECT_TRUE(drained.status.IsCorruption()) << drained.status.ToString();
  EXPECT_EQ(drained.entries, (Entries{{IKey("a", 1), "va"}, {IKey("c", 1), "vc"}}));

  // The same key at the end of one block and the start of the next.
  const Drained equal =
      Iterate(TableOf({{IKey("c", 1), {{IKey("a", 1), "va"}, {IKey("c", 1), "vc"}}},
                       {IKey("d", 1), {{IKey("c", 1), "vc"}, {IKey("d", 1), "vd"}}}}));
  EXPECT_TRUE(equal.status.IsCorruption());
  EXPECT_EQ(equal.entries.size(), size_t{2});
}

TEST_F(OrderTableTest, KeyAfterTheIndexKeyOfItsBlockEndsTheIteration) {
  // The index says the block ends at "m", and it holds "z".
  const Drained drained =
      Iterate(TableOf({{IKey("m", 1), {{IKey("a", 1), "va"}, {IKey("z", 1), "vz"}}}}));
  EXPECT_TRUE(drained.status.IsCorruption()) << drained.status.ToString();
  EXPECT_EQ(drained.entries, (Entries{{IKey("a", 1), "va"}}));
}

TEST_F(OrderTableTest, SeekIntoABlockChecksItToo) {
  const std::string bytes = TableOf({{IKey("c", 1), {{IKey("a", 1), "va"}, {IKey("c", 1), "vc"}}},
                                     {IKey("m", 1), {{IKey("e", 1), "ve"}, {IKey("z", 1), "vz"}}}});
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(bytes, &table));
  std::unique_ptr<Iterator> it = table->NewIterator({});
  it->Seek(IKey("d", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), IKey("e", 1))
      << "the seek is sent to the second block and finds its first key";
  it->Next();
  EXPECT_FALSE(it->Valid()) << "and the key after it is past the end the index names";
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST_F(OrderTableTest, SeekThatLandsBeforeItsTargetEndsTheIteration) {
  // The first block says it ends at "e" and holds only "a" and "c", so a seek to "d" finds nothing
  // in it and moves on to the second block, whose first key is "b". The seek was for "d" or after.
  const std::string bytes = TableOf({{IKey("e", 1), {{IKey("a", 1), "va"}, {IKey("c", 1), "vc"}}},
                                     {IKey("m", 1), {{IKey("b", 1), "vb"}, {IKey("m", 1), "vm"}}}});
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(bytes, &table));
  std::unique_ptr<Iterator> it = table->NewIterator({});
  it->Seek(IKey("d", kMaxSequenceNumber));
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
}

TEST_F(OrderTableTest, WellOrderedBlocksAreNotRefused) {
  // The control: the same tables with their keys in order are read to the end.
  const Entries first = {{IKey("a", 1), "va"}, {IKey("c", 1), "vc"}};
  const Entries second = {{IKey("d", 1), "vd"}, {IKey("m", 1), "vm"}};
  const Drained drained = Iterate(TableOf({{IKey("c", 1), first}, {IKey("m", 1), second}}));
  EXPECT_OK(drained.status);
  Entries expected = first;
  expected.insert(expected.end(), second.begin(), second.end());
  EXPECT_EQ(drained.entries, expected);
}

TEST_F(OrderTableTest, GetRefusesAnEntryAfterTheIndexKeyOfItsBlock) {
  // The block ends, by its index entry, at "a" at sequence 5, and holds "a" at sequence 3, which
  // sorts after that. A lookup at sequence 9 is sent to this block and the first entry it finds is
  // that one: its user key is the one asked for, and nothing else in the table says it is wrong.
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(TableOf({{IKey("a", 5), {{IKey("a", 3), "v3"}}}}), &table));
  const Lookup got = Get(*table, "a", 9);
  EXPECT_TRUE(got.status.IsCorruption()) << got.status.ToString();
  EXPECT_NE(got.result.kind, TableGetResult::Kind::kFound);
}

TEST_F(HostileTableTest, FilterThatCannotBeReadFailsOpen) {
  // A filter the reader cannot make sense of says nothing about the key, and the lookup goes on to
  // the block: a damaged filter costs a read and not a key that is there.
  const Entries block = {{IKey("a", 1), "va"}};
  for (const std::string& filter : {std::string("\xff"), std::string(64, '\xff'),
                                    std::string(1, '\x1f'), std::string("\0", 1)}) {
    std::shared_ptr<Table> table;
    ASSERT_OK(Open(OneBlockTable(block, block.back().first, filter), &table));
    const Lookup got = Get(*table, "a", kMaxSequenceNumber);
    ASSERT_OK(got.status);
    EXPECT_EQ(got.result.kind, TableGetResult::Kind::kFound) << "filter of " << filter.size();
  }
}

// ---- Damage everywhere ----

// A small table with several blocks, taken apart so that a test can damage a byte of one block and
// seal the block again, and a reader that does everything a reader can do to a table.
class TableDamageTest : public TableTest {
 protected:
  void SetUp() override {
    Random rnd(5);
    for (int i = 0; i < 60; ++i) {
      entries_.emplace_back(IKey(UserKey(i * 3), static_cast<SequenceNumber>(i) + 1),
                            RandomString(&rnd, 4 + rnd.Uniform(20)));
    }
    TableBuilderOptions options;
    options.block_size = 200;
    options.block_restart_interval = 4;
    BuildTable("/small.sst", entries_, options);
    bytes_ = ReadFile("/small.sst");

    // The blocks of the file, found from its footer and index by the format's own pieces.
    Footer footer;
    ASSERT_OK(footer.DecodeFrom(std::string_view(bytes_).substr(bytes_.size() - kFooterSize)));
    std::unique_ptr<RandomAccessFile> file;
    ASSERT_OK(env_->NewRandomAccessFile("/small.sst", &file));
    BlockContents index_contents;
    ASSERT_OK(ReadBlock(file.get(), footer.index, true, &index_contents));
    std::unique_ptr<Iterator> index = NewBlockIterator(
        std::make_shared<const Block>(std::move(index_contents)), CompareInternalKey, 8);
    for (index->SeekToFirst(); index->Valid(); index->Next()) {
      std::string_view value = index->value();
      BlockHandle handle;
      ASSERT_OK(handle.DecodeFrom(&value));
      blocks_.push_back(handle);
    }
    ASSERT_GE(blocks_.size(), size_t{5}) << "the table is meant to have several data blocks";
    blocks_.push_back(footer.filter);
    blocks_.push_back(footer.index);
  }

  // Changes the byte at `position` by xor with `mask`.
  static void Flip(std::string* bytes, size_t position, int mask) {
    (*bytes)[position] = static_cast<char>(static_cast<unsigned char>((*bytes)[position]) ^ mask);
  }

  // Writes the trailer of `block` anew, for the contents it has now in `*bytes`.
  static void Reseal(std::string* bytes, const BlockHandle& block) {
    const size_t offset = static_cast<size_t>(block.offset);
    const size_t size = static_cast<size_t>(block.size);
    const uint32_t crc = crc32c::Value(bytes->data() + offset, size + 1);
    EncodeFixed32(bytes->data() + offset + size + 1, crc32c::Mask(crc));
  }

  struct ReadChecks {
    // Checksums are verified when the table is read.
    bool verify = true;
    // What is read is held to what was written: the entries an iteration returns before it stops
    // are the first ones written, a lookup that succeeds returns the stored value, and a key that
    // is absent is reported absent. Right for a file in which every changed byte is covered by a
    // checksum, and wrong for one whose checksums were made right after the change.
    bool must_match_written = false;
    // The table must be found damaged, at Open() or somewhere in the reading.
    bool must_find_damage = false;
  };

  // Opens `bytes` as a table and does everything a reader can: a full iteration, a lookup of every
  // key that was written and of one that was not for each, and a seek to each. Every status that
  // comes back must be OK or Corruption, since no I/O fails here; nothing may crash, or read
  // outside a buffer, which AddressSanitizer is there to see; and the iteration must end.
  void ReadEverything(const std::string& bytes, const ReadChecks& checks, const std::string& what) {
    WriteFile("/damaged.sst", bytes);
    std::shared_ptr<Table> table;
    const Status opened = OpenTable("/damaged.sst", nullptr, &table);
    if (!opened.ok()) {
      EXPECT_TRUE(opened.IsCorruption()) << what << ": " << opened.ToString();
      EXPECT_EQ(table, nullptr) << what;
      return;
    }
    TableReadOptions options;
    options.verify_checksums = checks.verify;

    std::unique_ptr<Iterator> it = table->NewIterator(options);
    const Drained drained = Drain(it.get(), 100000);
    EXPECT_TRUE(drained.status.ok() || drained.status.IsCorruption())
        << what << ": " << drained.status.ToString();
    bool found_damage = !drained.status.ok();
    // Whatever the bytes are, what an iteration returns before it stops is in strictly increasing
    // order: a compaction feeds it to a TableBuilder, which aborts on a key that is not.
    for (size_t i = 1; i < drained.entries.size(); ++i) {
      ASSERT_LT(CompareInternalKey(drained.entries[i - 1].first, drained.entries[i].first), 0)
          << what << ": entry " << i << " is not after the one before it";
    }
    if (checks.must_match_written) {
      ASSERT_LE(drained.entries.size(), entries_.size()) << what;
      for (size_t i = 0; i < drained.entries.size(); ++i) {
        EXPECT_EQ(drained.entries[i], entries_[i]) << what << ": entry " << i;
      }
      if (drained.status.ok()) {
        EXPECT_EQ(drained.entries.size(), entries_.size())
            << what << ": an iteration that ended well";
      }
    }

    size_t index = 0;
    for (const auto& [key, value] : entries_) {
      const std::string user_key(ExtractUserKey(key));
      TableGetResult result;
      const Status s = table->Get(options, IKey(user_key, kMaxSequenceNumber), &result);
      EXPECT_TRUE(s.ok() || s.IsCorruption()) << what << ": " << s.ToString();
      found_damage = found_damage || !s.ok();
      if (s.ok() && checks.must_match_written) {
        ASSERT_EQ(result.kind, TableGetResult::Kind::kFound) << what << ": " << user_key;
        EXPECT_EQ(result.value, value) << what << ": " << user_key;
      }

      // Absent keys are asked about for every third present key, which is enough to meet each block
      // and each gap between blocks.
      if (index++ % 3 == 0) {
        const Status absent =
            table->Get(options, IKey(user_key + "?", kMaxSequenceNumber), &result);
        EXPECT_TRUE(absent.ok() || absent.IsCorruption()) << what << ": " << absent.ToString();
        found_damage = found_damage || !absent.ok();
        if (absent.ok() && checks.must_match_written) {
          EXPECT_EQ(result.kind, TableGetResult::Kind::kNotFound)
              << what << ": " << user_key << "?";
        }
      }
      const std::string target = IKey(user_key, kMaxSequenceNumber);
      it->Seek(target);
      EXPECT_TRUE(it->status().ok() || it->status().IsCorruption()) << what;
      if (it->Valid()) {
        // A seek lands at or after its target, and what follows is after that.
        const std::string landed(it->key());
        EXPECT_GE(CompareInternalKey(landed, target), 0) << what << ": seek to " << user_key;
        it->Next();
        if (it->Valid()) {
          EXPECT_GT(CompareInternalKey(it->key(), landed), 0) << what << ": after " << user_key;
        }
      }
    }
    if (checks.must_find_damage) {
      EXPECT_TRUE(found_damage) << what << ": the table opened and read as if nothing were wrong";
    }
  }

  Entries entries_;
  std::string bytes_;
  std::vector<BlockHandle> blocks_;  // the data blocks, then the filter block and the index block
};

TEST_F(TableDamageTest, UndamagedTableReadsBack) {
  ReadChecks checks;
  checks.must_match_written = true;
  ReadEverything(bytes_, checks, "undamaged");
  WriteFile("/ok.sst", bytes_);
  std::shared_ptr<Table> table;
  ASSERT_OK(OpenTable("/ok.sst", nullptr, &table));
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  EXPECT_OK(drained.status);
  EXPECT_EQ(drained.entries, entries_);
}

TEST_F(TableDamageTest, AnyChangedByteIsFoundAndNeverReadAsAnotherValue) {
  // Every byte of the file, whether it is in a block, a trailer or the footer: a CRC32C finds any
  // change to a run of 32 bits, so with checksums verified the damage must show, at Open() or in
  // a read, and nothing that was read before it may be wrong. Without verification nothing is
  // promised about the values, only that the reader stays inside its buffers.
  ReadChecks verified;
  verified.must_match_written = true;
  verified.must_find_damage = true;
  ReadChecks unverified;
  unverified.verify = false;
  for (size_t i = 0; i < bytes_.size(); i += kSweepStride) {
    for (const int mask : {0x01, 0xff}) {
      std::string damaged = bytes_;
      Flip(&damaged, i, mask);
      const std::string what = "byte " + std::to_string(i) + " xor " + std::to_string(mask);
      ReadEverything(damaged, verified, what);
      if (mask == 0xff) {
        ReadEverything(damaged, unverified, what);
      }
      if (HasFailure()) {
        return;
      }
    }
  }
}

TEST_F(TableDamageTest, AnyTruncationOrExtensionIsCorruption) {
  for (size_t length = 0; length < bytes_.size(); ++length) {
    WriteFile("/prefix.sst", bytes_.substr(0, length));
    std::shared_ptr<Table> table;
    const Status s = OpenTable("/prefix.sst", nullptr, &table);
    ASSERT_TRUE(s.IsCorruption()) << "a prefix of " << length << " bytes: " << s.ToString();
    EXPECT_EQ(table, nullptr);
  }
  // Bytes after the footer leave it somewhere other than the end.
  WriteFile("/extended.sst", bytes_ + "trailing bytes");
  std::shared_ptr<Table> table;
  EXPECT_TRUE(OpenTable("/extended.sst", nullptr, &table).IsCorruption());
}

TEST_F(TableDamageTest, DamageWithTheChecksumMadeRightAgain) {
  // What a fuzzer that knows the format does, and what a bug in a writer could: a byte of a
  // block's contents changed and the block's trailer recomputed to match, so that nothing is left
  // to catch it but the decoders. The reader may return other values, since the block it was given
  // says so, but only ever Corruption as an error, and never by leaving a buffer.
  ReadChecks checks;
  size_t variants = 0;
  for (const BlockHandle& block : blocks_) {
    for (size_t at = 0; at < block.size; at += kSweepStride) {
      for (const int mask : {0x01, 0xff}) {
        std::string damaged = bytes_;
        Flip(&damaged, static_cast<size_t>(block.offset) + at, mask);
        Reseal(&damaged, block);
        ReadEverything(damaged, checks,
                       "block at " + std::to_string(block.offset) + " byte " + std::to_string(at) +
                           " xor " + std::to_string(mask));
        ++variants;
        if (HasFailure()) {
          return;
        }
      }
    }
  }
  EXPECT_GT(variants, size_t{1000} / kSweepStride) << "the test is meant to damage a lot";
}

TEST_F(TableDamageTest, IndexBytesReplacedAndResealedAreNeverFollowedOutOfTheFile) {
  // The index is the part whose damage would send a read to the wrong place. Every byte of it
  // replaced by values that set a continuation bit, clear it, or are zero, trailer made right.
  const BlockHandle index = blocks_.back();
  ReadChecks checks;
  for (size_t at = 0; at < static_cast<size_t>(index.size); at += kSweepStride) {
    for (const int replacement : {0x00, 0x7f, 0x80, 0xff}) {
      std::string damaged = bytes_;
      damaged[static_cast<size_t>(index.offset) + at] = static_cast<char>(replacement);
      Reseal(&damaged, index);
      ReadEverything(damaged, checks,
                     "index byte " + std::to_string(at) + " := " + std::to_string(replacement));
      if (HasFailure()) {
        return;
      }
    }
  }
}

TEST_F(TableTest, ConcurrentReadersShareATableAndACache) {
  // Get() and the iterators are const, and the cache and the read counter are shared. Under
  // ThreadSanitizer this is the check that nothing the readers touch is unguarded; elsewhere it
  // checks that every reader sees the right values while blocks are being evicted under it.
  const Entries entries = NumberedEntries(4000);
  BlockCache cache(40 * 1024, 0);  // about ten blocks of a table of far more
  const std::shared_ptr<Table> table = BuildAndOpen(entries, &cache);
  ASSERT_NE(table, nullptr);
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      Random rnd(static_cast<uint64_t>(t));
      for (int i = 0; i < 400; ++i) {
        const int k = static_cast<int>(rnd.Uniform(4000));
        TableGetResult result;
        const Status s = table->Get({}, IKey(UserKey(k), kMaxSequenceNumber), &result);
        if (!s.ok() || result.kind != TableGetResult::Kind::kFound || result.value != ValueOf(k)) {
          ++wrong;
        }
      }
      std::unique_ptr<Iterator> it = table->NewIterator({});
      it->Seek(IKey(UserKey(t * 1000), kMaxSequenceNumber));
      for (int i = 0; i < 500 && it->Valid(); ++i, it->Next()) {
        if (it->key() != entries[static_cast<size_t>(t * 1000 + i)].first) {
          ++wrong;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(wrong.load(), 0);
}

}  // namespace
}  // namespace shale
