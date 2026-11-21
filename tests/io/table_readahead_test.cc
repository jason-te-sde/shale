// What a table iterator reads, and in how many reads: a scan reads ahead, a seek does not, a point
// lookup never does, and every block that comes out of the readahead buffer is checked on its own,
// as one read alone would have been.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/env_util.h"
#include "io/readahead.h"
#include "io/table.h"
#include "io/table_builder.h"
#include "io/table_format.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/counting_env.h"
#include "testkit/raw_table.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/random.h"

namespace shale {
namespace {

// A file over a string, whose bytes a test can damage as they are read or cut short while the
// table that reads it is open.
class ScriptedFile final : public RandomAccessFile {
 public:
  explicit ScriptedFile(std::string bytes) : bytes_(std::move(bytes)) {}

  // Flips bit 0 of the byte at `position` in the first read that covers it, and in no other.
  void FlipOnce(uint64_t position) {
    flip_at_ = position;
    flip_pending_ = true;
  }
  // From now on the file ends at `size`.
  void CutAt(uint64_t size) { size_ = std::min<uint64_t>(size, bytes_.size()); }
  // From now on a read of more than `bytes` fails with an IOError.
  void FailReadsLongerThan(size_t bytes) { max_read_ = bytes; }
  // From now on a read that overlaps [`begin`, `end`) fails with an IOError.
  void FailReadsOverlapping(uint64_t begin, uint64_t end) {
    fail_begin_ = begin;
    fail_end_ = end;
  }
  int flips() const { return flips_; }

  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    if (n > max_read_ || (offset < fail_end_ && offset + n > fail_begin_)) {
      return Status::IOError("injected read error");
    }
    const uint64_t from = std::min<uint64_t>(offset, size_);
    const size_t len = static_cast<size_t>(std::min<uint64_t>(n, size_ - from));
    std::memcpy(scratch, bytes_.data() + from, len);
    if (flip_pending_ && flip_at_ >= from && flip_at_ < from + len) {
      scratch[flip_at_ - from] = static_cast<char>(scratch[flip_at_ - from] ^ 0x01);
      flip_pending_ = false;
      ++flips_;
    }
    *result = std::string_view(scratch, len);
    return Status::OK();
  }

 private:
  const std::string bytes_;
  uint64_t size_ = bytes_.size();
  uint64_t flip_at_ = 0;
  size_t max_read_ = ~size_t{0};
  uint64_t fail_begin_ = 0;
  uint64_t fail_end_ = 0;
  mutable bool flip_pending_ = false;
  mutable int flips_ = 0;
};

struct Drained {
  Entries entries;
  Status status;
};

Drained Drain(Iterator* it, bool from_first = true) {
  Drained drained;
  if (from_first) {
    it->SeekToFirst();
  }
  for (; it->Valid(); it->Next()) {
    drained.entries.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  drained.status = it->status();
  return drained;
}

Entries Prefix(const Entries& entries, size_t n) {
  return Entries(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(n));
}

constexpr size_t kBlockSize = 4096;

// One table of 100-byte values in 4096-byte blocks, built once for every test of the suite, and a
// map from entries to blocks.
class TableReadaheadTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    Random rnd(11);
    entries_ = new Entries;
    for (int i = 0; i < 40000; ++i) {
      entries_->emplace_back(IKey(UserKey(i), static_cast<SequenceNumber>(i) + 1),
                             RandomString(&rnd, 100));
    }
    std::unique_ptr<Env> env = NewMemEnv();
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(env->NewWritableFile("/t.sst", &file));
    TableBuilderOptions options;
    options.block_size = kBlockSize;
    TableBuilder builder(options, file.get());
    for (const auto& [key, value] : *entries_) {
      builder.Add(key, value);
    }
    ASSERT_OK(builder.Finish());
    ASSERT_OK(file->Close());
    bytes_ = new std::string;
    ASSERT_OK(ReadFileToString(env.get(), "/t.sst", bytes_));
    handles_ = new std::vector<BlockHandle>(DataBlockHandles(env.get(), "/t.sst"));
    ASSERT_GE(handles_->size(), 1000u);

    // The first entry of each block, found by reading the table without a cache: a block is read
    // once, when the iterator reaches it, and the count of reads says which block each entry is in.
    std::unique_ptr<RandomAccessFile> raw;
    ASSERT_OK(env->NewRandomAccessFile("/t.sst", &raw));
    std::shared_ptr<Table> table;
    ASSERT_OK(Table::Open(std::move(raw), bytes_->size(), 1, nullptr, "", &table));
    first_entry_ = new std::vector<size_t>;
    std::unique_ptr<Iterator> it = table->NewIterator({});
    uint64_t seen = 0;
    size_t i = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next(), ++i) {
      if (table->data_block_reads() != seen) {
        seen = table->data_block_reads();
        first_entry_->push_back(i);
      }
    }
    ASSERT_OK(it->status());
    ASSERT_EQ(i, entries_->size());
    ASSERT_EQ(first_entry_->size(), handles_->size());
  }

  static void TearDownTestSuite() {
    delete entries_;
    delete bytes_;
    delete handles_;
    delete first_entry_;
  }

  void SetUp() override { counting_ = std::make_unique<CountingEnv>(mem_.get()); }

  std::shared_ptr<Table> Open(const std::string& bytes, BlockCache* cache = nullptr) {
    EXPECT_OK(WriteStringToFile(counting_.get(), bytes, "/open.sst", false));
    std::unique_ptr<RandomAccessFile> file;
    EXPECT_OK(counting_->NewRandomAccessFile("/open.sst", &file));
    std::shared_ptr<Table> table;
    EXPECT_OK(Table::Open(std::move(file), bytes.size(), 1, cache, "", &table));
    counting_->ResetCounts();
    return table;
  }

  std::shared_ptr<Table> OpenScripted(const std::string& bytes, ScriptedFile** file,
                                      BlockCache* cache = nullptr) {
    auto scripted = std::make_unique<ScriptedFile>(bytes);
    *file = scripted.get();
    std::shared_ptr<Table> table;
    EXPECT_OK(Table::Open(std::move(scripted), bytes.size(), 1, cache, "", &table));
    return table;
  }

  static uint64_t Stored(size_t block) { return (*handles_)[block].size + kBlockTrailerSize; }
  static uint64_t DataEnd() {
    return handles_->back().offset + handles_->back().size + kBlockTrailerSize;
  }
  static uint64_t MaxStored() {
    uint64_t max = 0;
    for (size_t i = 0; i < handles_->size(); ++i) {
      max = std::max(max, Stored(i));
    }
    return max;
  }
  static const std::string& FirstKey(size_t block) {
    return (*entries_)[(*first_entry_)[block]].first;
  }
  static std::string Damaged(size_t block, uint64_t at) {
    std::string damaged = *bytes_;
    const size_t position = static_cast<size_t>((*handles_)[block].offset + at);
    damaged[position] = static_cast<char>(damaged[position] ^ 0x01);
    return damaged;
  }
  // The entries of blocks `from` up to, not including, `to`.
  static Entries EntriesOfBlocks(size_t from, size_t to) {
    const size_t end = to < first_entry_->size() ? (*first_entry_)[to] : entries_->size();
    return Entries(entries_->begin() + static_cast<std::ptrdiff_t>((*first_entry_)[from]),
                   entries_->begin() + static_cast<std::ptrdiff_t>(end));
  }

  static Entries* entries_;
  static std::string* bytes_;
  static std::vector<BlockHandle>* handles_;
  static std::vector<size_t>* first_entry_;

  std::unique_ptr<Env> mem_ = NewMemEnv();
  std::unique_ptr<CountingEnv> counting_;
};

Entries* TableReadaheadTest::entries_ = nullptr;
std::string* TableReadaheadTest::bytes_ = nullptr;
std::vector<BlockHandle>* TableReadaheadTest::handles_ = nullptr;
std::vector<size_t>* TableReadaheadTest::first_entry_ = nullptr;

TEST_F(TableReadaheadTest, SequentialScanOfAThousandBlocksMakesFewReads) {
  const std::shared_ptr<Table> table = Open(*bytes_);
  CountingEnv& env = *counting_;
  const uint64_t data_end = DataEnd();
  const uint64_t max_stored_block = MaxStored();
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  const uint64_t ramp = 6;  // fills at 8, 16, 32, 64, 128 and 256 KiB past the block
  EXPECT_EQ(drained.entries, *entries_);
  EXPECT_OK(drained.status);
  EXPECT_EQ(table->data_block_reads(), handles_->size());
  EXPECT_LE(env.random_reads(),
            Readahead::kSequentialBlocks + ramp + data_end / Readahead::kMaxBytes + 1);
  EXPECT_LE(env.max_read_end(), data_end);  // nothing past the data blocks
  EXPECT_LE(env.random_read_bytes(), data_end + env.random_reads() * max_stored_block);
}

TEST_F(TableReadaheadTest, RandomSeeksReadOnlyTheBlocksTheyLandIn) {
  const std::shared_ptr<Table> table = Open(*bytes_);
  CountingEnv& env = *counting_;
  const uint64_t max_stored_block = MaxStored();
  std::unique_ptr<Iterator> it = table->NewIterator({});
  Random rnd(301);
  for (int i = 0; i < 300; ++i) {
    const size_t target = rnd.Uniform(static_cast<uint32_t>(entries_->size()));
    it->Seek((*entries_)[target].first);
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), (*entries_)[target].first);
    EXPECT_EQ(it->value(), (*entries_)[target].second);
    for (int step = 0; step < 2 && it->Valid(); ++step) {
      it->Next();
    }
    ASSERT_OK(it->status());
  }
  EXPECT_LE(env.random_reads(), 2u * 300);
  EXPECT_LE(env.random_read_bytes(), env.random_reads() * max_stored_block);
  EXPECT_LE(env.random_read_bytes(), 2u * 300 * max_stored_block);
}

TEST_F(TableReadaheadTest, GetNeverReadsAhead) {
  const std::shared_ptr<Table> table = Open(*bytes_);
  CountingEnv& env = *counting_;
  uint64_t expected_bytes = 0;
  for (size_t block = 0; block < 100; ++block) {
    TableGetResult result;
    const std::string_view user_key = ExtractUserKey(FirstKey(block));
    ASSERT_OK(table->Get({}, IKey(user_key, kMaxSequenceNumber), &result));
    EXPECT_EQ(result.kind, TableGetResult::Kind::kFound);
    expected_bytes += Stored(block);
  }
  EXPECT_EQ(env.random_reads(), 100u);
  EXPECT_EQ(env.random_read_bytes(), expected_bytes);
}

TEST_F(TableReadaheadTest, FirstBlockAfterASeekIsReadAlone) {
  const std::shared_ptr<Table> table = Open(*bytes_);
  CountingEnv& env = *counting_;
  std::unique_ptr<Iterator> it = table->NewIterator({});
  it->SeekToFirst();
  for (; it->Valid() && it->key() != (*entries_)[(*first_entry_)[20] - 1].first; it->Next()) {
  }
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(table->data_block_reads(), 20u);  // blocks 0 through 19

  uint64_t reads = env.random_reads();
  uint64_t bytes = env.random_read_bytes();
  it->Seek(FirstKey(600));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(env.random_reads(), reads + 1);
  EXPECT_EQ(env.random_read_bytes(), bytes + Stored(600));

  reads = env.random_reads();
  bytes = env.random_read_bytes();
  while (it->Valid() && it->key() != (*entries_)[(*first_entry_)[601] - 1].first) {
    it->Next();
  }
  it->Next();  // into block 601
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), FirstKey(601));
  EXPECT_EQ(env.random_reads(), reads + 1);
  EXPECT_EQ(env.random_read_bytes(), bytes + Stored(601));

  reads = env.random_reads();
  bytes = env.random_read_bytes();
  while (it->Valid() && it->key() != (*entries_)[(*first_entry_)[602] - 1].first) {
    it->Next();
  }
  it->Next();  // into block 602
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), FirstKey(602));
  EXPECT_EQ(env.random_reads(), reads + 1);
  EXPECT_GT(env.random_read_bytes(), bytes + Stored(602));
}

TEST_F(TableReadaheadTest, EveryBlockReadAheadIsCheckedOnItsOwn) {
  const std::shared_ptr<Table> table = Open(Damaged(10, 50));
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  EXPECT_EQ(drained.entries, EntriesOfBlocks(0, 10));
  EXPECT_EQ(drained.status.ToString(), "Corruption: block checksum mismatch");
}

TEST_F(TableReadaheadTest, ChecksumFailureInABlockReadAheadIsReadAgainAlone) {
  // A read that returns a block wrong once is not a block that is wrong: the copy in the buffer
  // fails its check, and the block is read again by itself, which decides.
  ScriptedFile* file = nullptr;
  const std::shared_ptr<Table> table = OpenScripted(*bytes_, &file);
  file->FlipOnce((*handles_)[3].offset + 10);
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  EXPECT_OK(drained.status);
  EXPECT_EQ(drained.entries, *entries_);
  EXPECT_EQ(file->flips(), 1);
  EXPECT_EQ(table->data_block_reads(), handles_->size() + 1);
}

TEST_F(TableReadaheadTest, FillThatFailsFallsBackToReadingTheBlockAlone) {
  // A read error in the bytes ahead of a block must not fail a block that a read of its own serves.
  {
    ScriptedFile* file = nullptr;
    const std::shared_ptr<Table> table = OpenScripted(*bytes_, &file);
    file->FailReadsLongerThan(static_cast<size_t>(MaxStored()));
    std::unique_ptr<Iterator> it = table->NewIterator({});
    const Drained drained = Drain(it.get());
    EXPECT_OK(drained.status);
    EXPECT_EQ(drained.entries, *entries_);
    // Every block was read, and each fill that failed counted a second read of its block.
    EXPECT_GT(table->data_block_reads(), handles_->size());
  }
  // A lone read that fails too decides: the scan ends with its error.
  {
    ScriptedFile* file = nullptr;
    const std::shared_ptr<Table> table = OpenScripted(*bytes_, &file);
    file->FailReadsOverlapping((*handles_)[10].offset, (*handles_)[10].offset + Stored(10));
    std::unique_ptr<Iterator> it = table->NewIterator({});
    const Drained drained = Drain(it.get());
    EXPECT_TRUE(drained.status.IsIOError()) << drained.status.ToString();
    EXPECT_EQ(drained.entries, EntriesOfBlocks(0, 10));
  }
}

TEST_F(TableReadaheadTest, FileCutShortWhileReadingAheadIsTruncatedBlockRead) {
  ScriptedFile* file = nullptr;
  const std::shared_ptr<Table> table = OpenScripted(*bytes_, &file);
  std::unique_ptr<Iterator> it = table->NewIterator({});
  Entries seen;
  it->SeekToFirst();
  for (; it->Valid() && seen.size() < (*first_entry_)[5]; it->Next()) {
    seen.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  ASSERT_OK(it->status());
  file->CutAt((*handles_)[40].offset + 100);
  const Drained rest = Drain(it.get(), /*from_first=*/false);
  seen.insert(seen.end(), rest.entries.begin(), rest.entries.end());
  EXPECT_EQ(seen, Prefix(*entries_, seen.size()));
  EXPECT_LT(seen.size(), (*first_entry_)[40] + 1);
  EXPECT_GE(seen.size(), (*first_entry_)[39]);
  EXPECT_EQ(rest.status.ToString(), "Corruption: truncated block read");
}

TEST_F(TableReadaheadTest, BlocksThatAreNotBackToBackAreFoundByTheirOffsets) {
  Entries all;
  RawTable raw;
  std::vector<std::pair<std::string, BlockHandle>> index;
  Random rnd(5);
  int next = 0;
  for (int block = 0; block < 40; ++block) {
    Entries entries;
    for (int i = 0; i < 10; ++i, ++next) {
      entries.emplace_back(IKey(UserKey(next), static_cast<SequenceNumber>(next) + 1),
                           RandomString(&rnd, 100));
    }
    index.emplace_back(entries.back().first, raw.AddEntries(entries, 16));
    raw.AddPadding(37);
    all.insert(all.end(), entries.begin(), entries.end());
  }
  const BlockHandle filter = raw.AddBlock("");
  const BlockHandle index_handle = raw.AddIndex(index);
  const std::shared_ptr<Table> table = Open(raw.Finish(filter, index_handle));
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  EXPECT_OK(drained.status);
  EXPECT_EQ(drained.entries, all);
  EXPECT_LT(counting_->random_reads(), 40u);
}

TEST_F(TableReadaheadTest, BlocksLargerThanTheCapAreReadAlone) {
  Entries entries;
  Random rnd(9);
  for (int i = 0; i < 600; ++i) {
    entries.emplace_back(IKey(UserKey(i), static_cast<SequenceNumber>(i) + 1),
                         RandomString(&rnd, 10 << 10));
  }
  std::unique_ptr<WritableFile> file;
  ASSERT_OK(counting_->NewWritableFile("/big.sst", &file));
  TableBuilderOptions options;
  options.block_size = size_t{512} << 10;
  TableBuilder builder(options, file.get());
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  ASSERT_OK(builder.Finish());
  ASSERT_OK(file->Close());
  const std::vector<BlockHandle> handles = DataBlockHandles(counting_.get(), "/big.sst");
  ASSERT_GE(handles.size(), 6u);
  std::string bytes;
  ASSERT_OK(ReadFileToString(counting_.get(), "/big.sst", &bytes));
  const std::shared_ptr<Table> table = Open(bytes);
  std::unique_ptr<Iterator> it = table->NewIterator({});
  const Drained drained = Drain(it.get());
  EXPECT_OK(drained.status);
  EXPECT_EQ(drained.entries, entries);
  EXPECT_EQ(counting_->random_reads(), handles.size());
}

TEST_F(TableReadaheadTest, UnverifiedScanThatReadsAheadLeavesNothingInTheCache) {
  BlockCache cache(8 << 20);
  const std::shared_ptr<Table> table = Open(Damaged(4, 50), &cache);

  TableReadOptions unverified;
  unverified.verify_checksums = false;
  std::unique_ptr<Iterator> it = table->NewIterator(unverified);
  const Drained loose = Drain(it.get());
  EXPECT_OK(loose.status);
  ASSERT_EQ(loose.entries.size(), entries_->size());
  for (size_t i = 0; i < entries_->size(); ++i) {
    ASSERT_EQ(loose.entries[i].first, (*entries_)[i].first);
  }
  EXPECT_EQ(cache.TotalCharge(), 0u);

  TableReadOptions no_fill;
  no_fill.fill_cache = false;
  it = table->NewIterator(no_fill);
  const Drained uncached = Drain(it.get());
  EXPECT_EQ(uncached.entries, EntriesOfBlocks(0, 4));
  EXPECT_EQ(uncached.status.ToString(), "Corruption: block checksum mismatch");
  EXPECT_EQ(cache.TotalCharge(), 0u);

  it = table->NewIterator({});
  const Drained strict = Drain(it.get());
  EXPECT_EQ(strict.entries, EntriesOfBlocks(0, 4));
  EXPECT_EQ(strict.status.ToString(), "Corruption: block checksum mismatch");
}

TEST_F(TableReadaheadTest, ScanThatReadsAheadCachesExactlyTheBlocksItVisited) {
  BlockCache cache(size_t{64} << 20);
  const std::shared_ptr<Table> table = Open(*bytes_, &cache);
  std::unique_ptr<Iterator> it = table->NewIterator({});
  it->SeekToFirst();
  while (it->Valid() && it->key() != (*entries_)[(*first_entry_)[10] - 1].first) {
    it->Next();
  }
  ASSERT_TRUE(it->Valid());

  uint64_t reads = table->data_block_reads();
  TableGetResult result;
  for (const std::string& key : {FirstKey(9), (*entries_)[(*first_entry_)[10] - 1].first}) {
    ASSERT_OK(table->Get({}, IKey(ExtractUserKey(key), kMaxSequenceNumber), &result));
    EXPECT_EQ(result.kind, TableGetResult::Kind::kFound);
  }
  EXPECT_EQ(table->data_block_reads(), reads) << "block 9 was visited, so it is cached";

  reads = table->data_block_reads();
  ASSERT_OK(table->Get({}, IKey(ExtractUserKey(FirstKey(12)), kMaxSequenceNumber), &result));
  EXPECT_EQ(result.kind, TableGetResult::Kind::kFound);
  EXPECT_EQ(table->data_block_reads(), reads + 1) << "block 12 was buffered and never visited";
}

}  // namespace
}  // namespace shale
