#include "db/table_cache.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/env_util.h"
#include "io/lru_cache.h"
#include "io/table.h"
#include "io/table_builder.h"
#include "io/table_format.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/fault_env.h"
#include "testkit/raw_table.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"

namespace shale {
namespace {

const char kName[] = "/db/000007.sst";

class TableCacheTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_OK(env_.CreateDir("/db")); }

  // Writes table 7 from `entries`, in the order a table holds them, and returns its size.
  uint64_t WriteTable(const Entries& entries) {
    std::unique_ptr<WritableFile> file;
    EXPECT_OK(env_.NewWritableFile(kName, &file));
    TableBuilderOptions options;
    options.block_size = 512;
    TableBuilder builder(options, file.get());
    for (const auto& [key, value] : entries) {
      builder.Add(key, value);
    }
    EXPECT_OK(builder.Finish());
    EXPECT_OK(file->Close());
    return SizeOfTable();
  }

  // Writes table 7 from the bytes of a file.
  uint64_t WriteBytes(const std::string& bytes) {
    EXPECT_OK(WriteStringToFile(&env_, bytes, kName, false));
    return SizeOfTable();
  }

  // Table 7 is a different file each time it is written, so what the caches hold of the last one
  // is dropped: the table cache serves a table of that number for as long as it has it, and the
  // block cache a block of it.
  void ForgetTable() {
    cache_ = nullptr;
    block_cache_ = std::make_unique<BlockCache>(1 << 20);
    cache_ = std::make_unique<TableCache>("/db", &env_, block_cache_.get(), 10);
  }

  uint64_t SizeOfTable() {
    ForgetTable();
    uint64_t size = 0;
    EXPECT_OK(env_.GetFileSize(kName, &size));
    return size;
  }

  // The table's file with the byte at `position` flipped; a negative one counts from the end.
  std::string FlippedByte(uint64_t size, int64_t position) {
    std::string bytes;
    EXPECT_OK(ReadFileToString(&env_, kName, &bytes));
    EXPECT_EQ(bytes.size(), size);
    const size_t at = position < 0 ? bytes.size() - static_cast<size_t>(-position)
                                   : static_cast<size_t>(position);
    bytes[at] = static_cast<char>(bytes[at] ^ 0x01);
    return bytes;
  }

  // What a full read of table 7 by an iterator from the cache gives, from its first entry.
  struct Read {
    std::unique_ptr<Iterator> it;
    Entries entries;
  };
  Read Scan(uint64_t size) {
    Read read;
    read.it = cache_->NewIterator({}, 7, size);
    for (read.it->SeekToFirst(); read.it->Valid(); read.it->Next()) {
      read.entries.emplace_back(std::string(read.it->key()), std::string(read.it->value()));
    }
    return read;
  }

  static std::string Named(const std::string& status) { return status + ": " + kName; }

  FaultInjectionEnv env_{1};
  std::unique_ptr<BlockCache> block_cache_ = std::make_unique<BlockCache>(1 << 20);
  std::unique_ptr<TableCache> cache_ =
      std::make_unique<TableCache>("/db", &env_, block_cache_.get(), 10);
};

TEST_F(TableCacheTest, IteratorErrorsNameTheFile) {
  // Each is a table that is read to the point of its fault, and the status it ends with.
  {
    const uint64_t size = WriteTable(NumberedEntries(2000));
    WriteBytes(FlippedByte(size, 3));
    Read read = Scan(size);
    EXPECT_TRUE(read.entries.empty());
    EXPECT_EQ(read.it->status().ToString(), Named("Corruption: block checksum mismatch"));
    // And it is the end, with the name still on it.
    read.it->Seek(IKey("k001000", kMaxSequenceNumber));
    EXPECT_FALSE(read.it->Valid());
    EXPECT_EQ(read.it->status().ToString(), Named("Corruption: block checksum mismatch"));
  }

  const auto entry = [](std::string_view user_key) {
    return Entries::value_type{IKey(user_key, 1), "v"};
  };
  const auto table_of = [&](const std::vector<std::pair<std::string, Entries>>& blocks) {
    RawTable raw;
    std::vector<std::pair<std::string, BlockHandle>> index_entries;
    for (const auto& [index_key, entries] : blocks) {
      index_entries.emplace_back(index_key, raw.AddEntries(entries, 16));
    }
    const BlockHandle filter = raw.AddBlock("");
    const BlockHandle index = raw.AddIndex(index_entries);
    return WriteBytes(raw.Finish(filter, index));
  };

  {
    const uint64_t size = table_of({{IKey("b", 1), {entry("b"), entry("a")}}});
    Read read = Scan(size);
    EXPECT_EQ(read.entries, (Entries{entry("b")}));
    EXPECT_EQ(read.it->status().ToString(),
              Named("Corruption: keys in the table are not in increasing order"));
  }
  {
    const uint64_t size = table_of({{IKey("m", 1), {entry("a"), entry("z")}}});
    Read read = Scan(size);
    EXPECT_EQ(read.entries, (Entries{entry("a")}));
    EXPECT_EQ(read.it->status().ToString(),
              Named("Corruption: data block holds a key after the last key its index entry names"));
  }
  {
    const uint64_t size = table_of(
        {{IKey("e", 1), {entry("a"), entry("c")}}, {IKey("m", 1), {entry("b"), entry("m")}}});
    std::unique_ptr<Iterator> it = cache_->NewIterator({}, 7, size);
    it->Seek(IKey("d", kMaxSequenceNumber));
    EXPECT_FALSE(it->Valid());
    EXPECT_EQ(it->status().ToString(),
              Named("Corruption: seek in the table landed before its target"));
  }
}

TEST_F(TableCacheTest, OpenAndGetErrorsNameTheFile) {
  // A footer with a byte changed does not open.
  const uint64_t size = WriteTable(NumberedEntries(2000));
  WriteBytes(FlippedByte(size, -47));
  std::shared_ptr<Table> table;
  const Status opened = cache_->FindTable(7, size, &table);
  EXPECT_TRUE(opened.IsCorruption()) << opened.ToString();
  EXPECT_EQ(table, nullptr);
  const std::string text = opened.ToString();
  const std::string suffix = std::string(": ") + kName;
  ASSERT_GE(text.size(), suffix.size());
  EXPECT_EQ(text.substr(text.size() - suffix.size()), suffix) << text;

  // A data block with a byte changed opens, and the lookup that reaches it fails by name.
  WriteTable(NumberedEntries(2000));
  WriteBytes(FlippedByte(size, 3));
  TableGetResult result;
  const Status got =
      cache_->Get({}, 7, size, IKey(UserKey(0), kMaxSequenceNumber, kValueTypeForSeek), &result);
  EXPECT_EQ(got.ToString(), Named("Corruption: block checksum mismatch"));
}

TEST_F(TableCacheTest, IOErrorsAreNotNamedTwice) {
  const uint64_t size = WriteTable(NumberedEntries(2000));
  std::shared_ptr<Table> table;
  ASSERT_OK(cache_->FindTable(7, size, &table));
  env_.FailOpAfter(FaultInjectionEnv::Op::kRead, 0);
  Read read = Scan(size);
  const Status status = read.it->status();
  EXPECT_TRUE(status.IsIOError()) << status.ToString();
  const std::string text = status.ToString();
  size_t count = 0;
  for (size_t at = text.find(kName); at != std::string::npos; at = text.find(kName, at + 1)) {
    count++;
  }
  EXPECT_EQ(count, size_t{1}) << text;
}

TEST_F(TableCacheTest, TableThatDoesNotOpenGivesAnIteratorWhoseErrorNamesIt) {
  const uint64_t size = WriteTable(NumberedEntries(2000));
  WriteBytes(FlippedByte(size, -47));
  std::unique_ptr<Iterator> it = cache_->NewIterator({}, 7, size);
  ASSERT_NE(it, nullptr);
  EXPECT_FALSE(it->Valid());
  EXPECT_EQ(it->status().ToString(), Named("Corruption: footer checksum mismatch"));
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_EQ(it->status().ToString(), Named("Corruption: footer checksum mismatch"));
}

}  // namespace
}  // namespace shale
