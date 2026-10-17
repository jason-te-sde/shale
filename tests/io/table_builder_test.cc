// The table builder: what it writes, checked by taking the file apart with the format's own
// pieces, and what it refuses.
//
// The reader has its own tests, which build tables with this builder, so a mistake in the builder
// that the reader forgives would pass them. These tests do not use the reader: they parse the file
// the way the format describes it, the footer, then the index, then the blocks the index names.

#include "io/table_builder.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/block.h"
#include "io/block_builder.h"
#include "io/bloom.h"
#include "io/env_util.h"
#include "io/table_format.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/coding.h"
#include "util/dbformat.h"
#include "util/random.h"

namespace shale {
namespace {

// A table written to an in-memory file, and what is known about it.
struct BuiltTable {
  std::string bytes;
  uint64_t reported_size = 0;
  uint64_t reported_entries = 0;
};

BuiltTable Build(const Entries& entries, const TableBuilderOptions& options = {}) {
  std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  EXPECT_OK(env->NewWritableFile("/t.sst", &file));
  BuiltTable built;
  {
    TableBuilder builder(options, file.get());
    for (const auto& [key, value] : entries) {
      builder.Add(key, value);
    }
    EXPECT_OK(builder.Finish());
    built.reported_size = builder.FileSize();
    built.reported_entries = builder.NumEntries();
  }
  EXPECT_OK(file->Close());
  EXPECT_OK(ReadFileToString(env.get(), "/t.sst", &built.bytes));
  return built;
}

// A table taken apart by the rules of the format and nothing else.
struct ParsedTable {
  Footer footer;
  std::string filter;
  std::string index;
  // The data blocks in the order the index lists them.
  struct DataBlock {
    BlockHandle handle;
    std::string index_key;
    Entries entries;
  };
  std::vector<DataBlock> blocks;
};

Entries ReadAllEntries(std::string contents) {
  Entries entries;
  std::unique_ptr<Iterator> it =
      NewBlockIterator(std::make_shared<const Block>(std::move(contents)), CompareInternalKey, 8);
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    entries.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  EXPECT_OK(it->status());
  return entries;
}

void Parse(const std::string& bytes, ParsedTable* parsed) {
  ASSERT_GE(bytes.size(), kFooterSize);
  ASSERT_OK(parsed->footer.DecodeFrom(std::string_view(bytes).substr(bytes.size() - kFooterSize)));

  std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(WriteStringToFile(env.get(), bytes, "/t.sst", false));
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_OK(env->NewRandomAccessFile("/t.sst", &file));

  ASSERT_OK(ReadBlock(file.get(), parsed->footer.filter, true, &parsed->filter));
  ASSERT_OK(ReadBlock(file.get(), parsed->footer.index, true, &parsed->index));

  std::unique_ptr<Iterator> index =
      NewBlockIterator(std::make_shared<const Block>(parsed->index), CompareInternalKey, 8);
  for (index->SeekToFirst(); index->Valid(); index->Next()) {
    ParsedTable::DataBlock block;
    block.index_key = std::string(index->key());
    std::string_view value = index->value();
    ASSERT_OK(block.handle.DecodeFrom(&value));
    ASSERT_TRUE(value.empty()) << "an index value is a handle and nothing else";
    std::string contents;
    ASSERT_OK(ReadBlock(file.get(), block.handle, true, &contents));
    block.entries = ReadAllEntries(std::move(contents));
    parsed->blocks.push_back(std::move(block));
  }
  ASSERT_OK(index->status());
}

TEST(TableBuilderTest, FileSizeMatchesBytesWritten) {
  const Entries entries = NumberedEntries(10000);
  std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  ASSERT_OK(env->NewWritableFile("/t.sst", &file));
  TableBuilder builder(TableBuilderOptions{}, file.get());
  EXPECT_EQ(builder.FileSize(), uint64_t{0});
  EXPECT_EQ(builder.NumEntries(), uint64_t{0});
  uint64_t last_size = 0;
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
    EXPECT_GE(builder.FileSize(), last_size) << "FileSize never goes down";
    last_size = builder.FileSize();
  }
  EXPECT_EQ(builder.NumEntries(), uint64_t{10000});
  ASSERT_OK(builder.Finish());
  ASSERT_OK(file->Close());
  uint64_t on_disk = 0;
  ASSERT_OK(env->GetFileSize("/t.sst", &on_disk));
  EXPECT_EQ(builder.FileSize(), on_disk);
  EXPECT_GT(on_disk, last_size)
      << "Finish writes the last block, the filter, the index and the footer";
  EXPECT_EQ(builder.NumEntries(), uint64_t{10000});
  EXPECT_OK(builder.status());
}

TEST(TableBuilderTest, EmptyTableHasFooter) {
  const BuiltTable built = Build({});
  ASSERT_GE(built.bytes.size(), kFooterSize);
  EXPECT_EQ(built.bytes.size(), built.reported_size);
  EXPECT_EQ(DecodeFixed64(built.bytes.data() + built.bytes.size() - 8), kTableMagicNumber);
  EXPECT_EQ(built.bytes[built.bytes.size() - 8], '\x01') << "the magic is little-endian";

  ParsedTable parsed;
  Parse(built.bytes, &parsed);
  EXPECT_TRUE(parsed.blocks.empty());
  EXPECT_EQ(built.reported_entries, uint64_t{0});
}

TEST(TableBuilderTest, FileHoldsEveryEntryInOrderAcrossBlocks) {
  const Entries entries = NumberedEntries(3000);
  const BuiltTable built = Build(entries);
  EXPECT_EQ(built.reported_size, built.bytes.size());
  EXPECT_EQ(built.reported_entries, uint64_t{3000});

  ParsedTable parsed;
  Parse(built.bytes, &parsed);
  ASSERT_GT(parsed.blocks.size(), size_t{10}) << "3000 entries cannot fit in a handful of blocks";

  Entries joined;
  uint64_t expected_offset = 0;
  for (const ParsedTable::DataBlock& block : parsed.blocks) {
    ASSERT_FALSE(block.entries.empty());
    EXPECT_EQ(block.handle.offset, expected_offset) << "blocks follow one another with no gap";
    expected_offset = block.handle.offset + block.handle.size + kBlockTrailerSize;
    EXPECT_EQ(block.index_key, block.entries.back().first)
        << "an index entry's key is the last key of its block";
    joined.insert(joined.end(), block.entries.begin(), block.entries.end());
  }
  EXPECT_EQ(joined, entries);
  EXPECT_EQ(parsed.footer.filter.offset, expected_offset)
      << "the filter block follows the last data block";
  EXPECT_EQ(parsed.footer.index.offset,
            parsed.footer.filter.offset + parsed.footer.filter.size + kBlockTrailerSize)
      << "and the index block follows the filter";
  EXPECT_EQ(parsed.footer.index.offset + parsed.footer.index.size + kBlockTrailerSize + kFooterSize,
            built.bytes.size())
      << "and the footer follows the index and ends the file";
}

TEST(TableBuilderTest, BlockIsCutWhenItsEstimateReachesTheBlockSize) {
  TableBuilderOptions options;
  options.block_size = 1000;
  options.block_restart_interval = 4;
  const Entries entries = NumberedEntries(2000);
  const BuiltTable built = Build(entries, options);
  ParsedTable parsed;
  Parse(built.bytes, &parsed);
  ASSERT_GT(parsed.blocks.size(), size_t{20});

  for (size_t b = 0; b < parsed.blocks.size(); ++b) {
    const Entries& block = parsed.blocks[b].entries;
    // Rebuild the block with the builder's own restart interval and look at its size at each step.
    BlockBuilder rebuilt(options.block_restart_interval);
    for (size_t i = 0; i < block.size(); ++i) {
      if (i + 1 < block.size()) {
        EXPECT_LT(rebuilt.CurrentSizeEstimate(), options.block_size)
            << "block " << b << " is cut late: it had reached the block size before entry " << i;
      }
      rebuilt.Add(block[i].first, block[i].second);
    }
    if (b + 1 < parsed.blocks.size()) {
      EXPECT_GE(rebuilt.CurrentSizeEstimate(), options.block_size)
          << "block " << b << " is cut early, before it reached the block size";
    }
  }
}

TEST(TableBuilderTest, IndexBlockStoresEveryKeyWhole) {
  TableBuilderOptions options;
  options.block_size = 512;
  options.block_restart_interval = 16;
  const BuiltTable built = Build(NumberedEntries(1000), options);
  ParsedTable parsed;
  Parse(built.bytes, &parsed);
  ASSERT_GT(parsed.blocks.size(), size_t{5});
  // The last four bytes of a block are its number of restart points. The index is read by binary
  // search over its restart points alone, so a restart point per entry is what lets it be searched
  // as finely as it is long.
  ASSERT_GE(parsed.index.size(), size_t{4});
  EXPECT_EQ(DecodeFixed32(parsed.index.data() + parsed.index.size() - 4), parsed.blocks.size());
}

TEST(TableBuilderTest, FilterCoversUserKeysNotInternalKeys) {
  Entries entries;
  for (int i = 0; i < 500; ++i) {
    // Three versions of each user key, newest first, as they sort.
    entries.emplace_back(IKey(UserKey(i), 30, ValueType::kValue), "c");
    entries.emplace_back(IKey(UserKey(i), 20, ValueType::kDeletion), "");
    entries.emplace_back(IKey(UserKey(i), 10, ValueType::kValue), "a");
  }
  const BuiltTable built = Build(entries);
  ParsedTable parsed;
  Parse(built.bytes, &parsed);
  ASSERT_FALSE(parsed.filter.empty());
  for (int i = 0; i < 500; ++i) {
    EXPECT_TRUE(BloomFilterMayMatch(parsed.filter, UserKey(i))) << UserKey(i);
  }
  // A filter of internal keys would let the user keys through only by luck, and would let few of
  // them through. Absent keys are mostly refused; that is what the filter is for.
  int passed = 0;
  for (int i = 0; i < 1000; ++i) {
    passed += BloomFilterMayMatch(parsed.filter, "absent" + UserKey(i)) ? 1 : 0;
  }
  EXPECT_LT(passed, 100);
}

TEST(TableBuilderTest, FilterHoldsEachUserKeyOnce) {
  // A thousand versions of ten keys: a filter sized for a thousand keys would be mostly empty
  // space, and no more exact for it.
  Entries entries;
  for (int k = 0; k < 10; ++k) {
    for (int v = 1000; v >= 1; --v) {
      entries.emplace_back(IKey(UserKey(k), static_cast<SequenceNumber>(v)), "v");
    }
  }
  const BuiltTable built = Build(entries);
  ParsedTable parsed;
  Parse(built.bytes, &parsed);
  BloomFilterBuilder expected(10);
  for (int k = 0; k < 10; ++k) {
    expected.AddKey(UserKey(k));
  }
  EXPECT_EQ(parsed.filter, expected.Finish());
}

TEST(TableBuilderTest, NoBloomBitsMeansAnEmptyFilterBlock) {
  TableBuilderOptions options;
  options.bloom_bits_per_key = 0;
  const BuiltTable built = Build(NumberedEntries(100), options);
  ParsedTable parsed;
  Parse(built.bytes, &parsed);
  EXPECT_TRUE(parsed.filter.empty());
  EXPECT_EQ(parsed.footer.filter.size, uint64_t{0})
      << "the block is still there, so that every table has the same shape";
}

TEST(TableBuilderTest, ValueLargerThanABlockGetsABlockOfItsOwn) {
  TableBuilderOptions options;
  options.block_size = 256;
  Entries entries;
  entries.emplace_back(IKey("a", 1), "small");
  entries.emplace_back(IKey("b", 2), std::string(100000, 'v'));
  entries.emplace_back(IKey("c", 3), "small");
  const BuiltTable built = Build(entries, options);
  ParsedTable parsed;
  Parse(built.bytes, &parsed);
  Entries joined;
  for (const ParsedTable::DataBlock& block : parsed.blocks) {
    joined.insert(joined.end(), block.entries.begin(), block.entries.end());
  }
  EXPECT_EQ(joined, entries);
}

TEST(TableBuilderTest, SeveralVersionsOfOneKeyAreAccepted) {
  // The newest comes first: a higher sequence number sorts earlier.
  Entries entries;
  entries.emplace_back(IKey("k", 9), "new");
  entries.emplace_back(IKey("k", 5, ValueType::kDeletion), "");
  entries.emplace_back(IKey("k", 2), "old");
  const BuiltTable built = Build(entries);
  ParsedTable parsed;
  Parse(built.bytes, &parsed);
  ASSERT_EQ(parsed.blocks.size(), size_t{1});
  EXPECT_EQ(parsed.blocks[0].entries, entries);
}

// A file that takes `budget` bytes and then fails every call, to see what the builder does when a
// write fails: it must say so, and write nothing more.
class FailingFile final : public WritableFile {
 public:
  explicit FailingFile(size_t budget) : budget_(budget) {}

  Status Append(std::string_view data) override {
    if (failed_) {
      ++calls_after_failure_;
      return Status::IOError("disk full");
    }
    if (data.size() > budget_) {
      failed_ = true;
      return Status::IOError("disk full");
    }
    budget_ -= data.size();
    written_ += data.size();
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

  size_t written() const { return written_; }
  int calls_after_failure() const { return calls_after_failure_; }

 private:
  size_t budget_;
  size_t written_ = 0;
  bool failed_ = false;
  int calls_after_failure_ = 0;
};

TEST(TableBuilderTest, WriteFailureIsReportedAndStopsTheWriting) {
  const Entries entries = NumberedEntries(2000);
  FailingFile file(5000);
  TableBuilder builder(TableBuilderOptions{}, &file);
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  EXPECT_TRUE(builder.status().IsIOError()) << builder.status().ToString();
  const Status finished = builder.Finish();
  EXPECT_TRUE(finished.IsIOError()) << "Finish reports the failure: " << finished.ToString();
  EXPECT_EQ(file.calls_after_failure(), 0) << "nothing is written to a file that has failed";
  EXPECT_LE(file.written(), size_t{5000});
  EXPECT_EQ(builder.NumEntries(), uint64_t{2000}) << "entries are still counted as they are added";
}

TEST(TableBuilderTest, FailureWhileFinishingIsReturnedFromFinish) {
  // Everything but the footer goes in; the footer is the last write, and its failure is Finish's.
  const Entries entries = NumberedEntries(10);
  const BuiltTable built = Build(entries);
  FailingFile file(built.bytes.size() - 1);
  TableBuilder builder(TableBuilderOptions{}, &file);
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  EXPECT_OK(builder.status()) << "nothing has been written to a file yet";
  EXPECT_TRUE(builder.Finish().IsIOError());
  EXPECT_TRUE(builder.status().IsIOError());
}

TEST(TableBuilderTest, AbandonWritesNothingMore) {
  std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  ASSERT_OK(env->NewWritableFile("/t.sst", &file));
  uint64_t size_at_abandon = 0;
  {
    TableBuilder builder(TableBuilderOptions{}, file.get());
    for (const auto& [key, value] : NumberedEntries(1000)) {
      builder.Add(key, value);
    }
    size_at_abandon = builder.FileSize();
    // Not an ASSERT: a builder that is not ended aborts when it is destroyed, and an ASSERT would
    // leave the scope with it still live.
    builder.Abandon();
    EXPECT_GT(size_at_abandon, uint64_t{0}) << "the test needs some blocks written";
    EXPECT_EQ(builder.FileSize(), size_at_abandon);
  }
  ASSERT_OK(file->Close());
  uint64_t on_disk = 0;
  ASSERT_OK(env->GetFileSize("/t.sst", &on_disk));
  EXPECT_EQ(on_disk, size_at_abandon) << "an abandoned table gets no filter, index or footer";
}

// The checks of a builder that is misused. Each one builds everything inside the statement, so that
// the process that is meant to die is the one that does it.
TEST(TableBuilderDeathTest, OutOfOrderKeyIsRefused) {
  EXPECT_DEATH(
      {
        std::unique_ptr<Env> env = NewMemEnv();
        std::unique_ptr<WritableFile> file;
        EXPECT_OK(env->NewWritableFile("/t.sst", &file));
        TableBuilder builder(TableBuilderOptions{}, file.get());
        builder.Add(IKey("b", 1), "v");
        builder.Add(IKey("a", 2), "v");
      },
      "keys must be added in increasing order");
}

TEST(TableBuilderDeathTest, RepeatedKeyIsRefused) {
  EXPECT_DEATH(
      {
        std::unique_ptr<Env> env = NewMemEnv();
        std::unique_ptr<WritableFile> file;
        EXPECT_OK(env->NewWritableFile("/t.sst", &file));
        TableBuilder builder(TableBuilderOptions{}, file.get());
        builder.Add(IKey("a", 1), "v");
        builder.Add(IKey("a", 1), "v");
      },
      "keys must be added in increasing order");
}

TEST(TableBuilderDeathTest, OlderVersionBeforeNewerIsRefused) {
  // For one user key the newest entry sorts first, so seq 5 before seq 6 is backwards.
  EXPECT_DEATH(
      {
        std::unique_ptr<Env> env = NewMemEnv();
        std::unique_ptr<WritableFile> file;
        EXPECT_OK(env->NewWritableFile("/t.sst", &file));
        TableBuilder builder(TableBuilderOptions{}, file.get());
        builder.Add(IKey("a", 5), "v");
        builder.Add(IKey("a", 6), "v");
      },
      "keys must be added in increasing order");
}

TEST(TableBuilderDeathTest, KeyThatIsNotAnInternalKeyIsRefused) {
  EXPECT_DEATH(
      {
        std::unique_ptr<Env> env = NewMemEnv();
        std::unique_ptr<WritableFile> file;
        EXPECT_OK(env->NewWritableFile("/t.sst", &file));
        TableBuilder builder(TableBuilderOptions{}, file.get());
        builder.Add("short", "v");
      },
      "internal key");
}

TEST(TableBuilderDeathTest, AddAfterFinishIsRefused) {
  EXPECT_DEATH(
      {
        std::unique_ptr<Env> env = NewMemEnv();
        std::unique_ptr<WritableFile> file;
        EXPECT_OK(env->NewWritableFile("/t.sst", &file));
        TableBuilder builder(TableBuilderOptions{}, file.get());
        EXPECT_OK(builder.Finish());
        builder.Add(IKey("a", 1), "v");
      },
      "after Finish or Abandon");
}

TEST(TableBuilderDeathTest, FinishTwiceIsRefused) {
  EXPECT_DEATH(
      {
        std::unique_ptr<Env> env = NewMemEnv();
        std::unique_ptr<WritableFile> file;
        EXPECT_OK(env->NewWritableFile("/t.sst", &file));
        TableBuilder builder(TableBuilderOptions{}, file.get());
        EXPECT_OK(builder.Finish());
        EXPECT_OK(builder.Finish());
      },
      "after Finish or Abandon");
}

TEST(TableBuilderDeathTest, DestroyedWithoutFinishOrAbandonIsRefused) {
  // A builder dropped half-way leaves a file that looks like a table until someone reads its end.
  EXPECT_DEATH(
      {
        std::unique_ptr<Env> env = NewMemEnv();
        std::unique_ptr<WritableFile> file;
        EXPECT_OK(env->NewWritableFile("/t.sst", &file));
        TableBuilder builder(TableBuilderOptions{}, file.get());
        builder.Add(IKey("a", 1), "v");
      },
      "Finish or Abandon");
}

}  // namespace
}  // namespace shale
