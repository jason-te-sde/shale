#include "io/table.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "io/block.h"
#include "io/bloom.h"
#include "io/iterator_util.h"
#include "io/table_format.h"
#include "io/two_level_iterator.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/dbformat.h"

namespace shale {

namespace {

// True if the block `handle` names, with its trailer, lies inside [0, limit). Written so that no
// sum of values from the file can overflow.
bool HandleFits(const BlockHandle& handle, uint64_t limit) {
  return handle.offset <= limit && handle.size <= limit - handle.offset &&
         limit - handle.offset - handle.size >= kBlockTrailerSize;
}

// Decodes an index entry's value: a handle, and nothing else, to a block that lies before
// `data_end`.
Status DecodeDataHandle(std::string_view value, uint64_t data_end, BlockHandle* handle) {
  const Status s = handle->DecodeFrom(&value);
  if (!s.ok() || !value.empty()) {
    return Status::Corruption("bad block handle in index");
  }
  if (!HandleFits(*handle, data_end)) {
    return Status::Corruption("block handle in index is outside the data blocks");
  }
  return Status::OK();
}

// Checks every entry of the index: the keys are internal keys in strictly increasing order, and
// the values are handles of data blocks that come one after another, in order, before `data_end`.
// An index like that is what the builder writes, and one that is not could send a read into the
// filter or the index itself, or return the entries of one block twice.
Status ValidateIndex(std::shared_ptr<const Block> index, uint64_t data_end) {
  std::unique_ptr<Iterator> it =
      NewBlockIterator(std::move(index), CompareInternalKey, kInternalKeyTagSize);
  std::string previous_key;
  uint64_t next_block_offset = 0;
  bool first = true;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (!first && CompareInternalKey(previous_key, it->key()) >= 0) {
      return Status::Corruption("index keys are not in increasing order");
    }
    BlockHandle handle;
    Status s = DecodeDataHandle(it->value(), data_end, &handle);
    if (!s.ok()) {
      return s;
    }
    if (handle.offset < next_block_offset) {
      return Status::Corruption("data blocks in the index overlap or are out of order");
    }
    // This cannot overflow: the handle fits below `data_end`.
    next_block_offset = handle.offset + handle.size + kBlockTrailerSize;
    previous_key.assign(it->key());
    first = false;
  }
  return it->status();
}

// The index iterator with the key of each entry put in front of its value, which is what the
// two-level iterator hands the function that opens a block. The block's iterator needs the key,
// the last key the block may hold, to check the block against, and the two-level iterator passes
// that function the value alone.
class KeyedIndexIterator final : public Iterator {
 public:
  explicit KeyedIndexIterator(std::unique_ptr<Iterator> index) : index_(std::move(index)) {}

  bool Valid() const override { return index_->Valid(); }
  void SeekToFirst() override { index_->SeekToFirst(); }
  void Seek(std::string_view target) override { index_->Seek(target); }
  void Next() override { index_->Next(); }
  std::string_view key() const override { return index_->key(); }
  Status status() const override { return index_->status(); }

  std::string_view value() const override {
    keyed_value_.clear();
    PutLengthPrefixed(&keyed_value_, index_->key());
    keyed_value_.append(index_->value());
    return keyed_value_;
  }

 private:
  const std::unique_ptr<Iterator> index_;
  mutable std::string keyed_value_;
};

// Splits what KeyedIndexIterator returns into the key and the value it put it in front of.
void SplitKeyedValue(std::string_view keyed_value, std::string_view* key,
                     std::string_view* index_value) {
  *index_value = keyed_value;
  SHALE_CHECK(GetLengthPrefixed(index_value, key), "a keyed index value has no key");
}

// Iterates a data block, and ends the iteration with Corruption at a key that is after the last key
// the index says the block holds. A table that is searched by its index cannot have one: a lookup
// for it would be sent to the next block. Like the iterators of blocks it is sticky: once it has
// failed it stays invalid.
class BoundedBlockIterator final : public Iterator {
 public:
  BoundedBlockIterator(std::unique_ptr<Iterator> block, std::string last_key)
      : block_(std::move(block)), last_key_(std::move(last_key)) {}

  bool Valid() const override { return status_.ok() && block_->Valid(); }

  void SeekToFirst() override {
    if (status_.ok()) {
      block_->SeekToFirst();
      Check();
    }
  }

  void Seek(std::string_view target) override {
    if (status_.ok()) {
      block_->Seek(target);
      Check();
    }
  }

  void Next() override {
    SHALE_CHECK(Valid(), "table block iterator: Next requires Valid");
    block_->Next();
    Check();
  }

  std::string_view key() const override {
    SHALE_CHECK(Valid(), "table block iterator: key requires Valid");
    return block_->key();
  }

  std::string_view value() const override {
    SHALE_CHECK(Valid(), "table block iterator: value requires Valid");
    return block_->value();
  }

  Status status() const override { return status_.ok() ? block_->status() : status_; }

 private:
  void Check() {
    if (block_->Valid() && CompareInternalKey(block_->key(), last_key_) > 0) {
      status_ =
          Status::Corruption("data block holds a key after the last key its index entry names");
    }
  }

  const std::unique_ptr<Iterator> block_;
  const std::string last_key_;
  Status status_;
};

// Iterates a table, and ends the iteration with Corruption at a key that is not after the one
// before it, or at a seek that lands before its target. Keys that go backwards, or repeat, are no
// sequence a TableBuilder can be given, and code that merges tables into a new one would be stopped
// by its check for them; a scan that begins before the key it was asked to begin at returns what it
// was not asked for. Like the others it is sticky.
class OrderedIterator final : public Iterator {
 public:
  explicit OrderedIterator(std::unique_ptr<Iterator> table) : table_(std::move(table)) {}

  bool Valid() const override { return status_.ok() && table_->Valid(); }

  void SeekToFirst() override {
    if (status_.ok()) {
      table_->SeekToFirst();
      Check(/*after_seek=*/true);
    }
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) {
      return;
    }
    table_->Seek(target);
    if (table_->Valid() && CompareInternalKey(table_->key(), target) < 0) {
      status_ = Status::Corruption("seek in the table landed before its target");
      return;
    }
    Check(/*after_seek=*/true);
  }

  void Next() override {
    SHALE_CHECK(Valid(), "table iterator: Next requires Valid");
    table_->Next();
    Check(/*after_seek=*/false);
  }

  std::string_view key() const override {
    SHALE_CHECK(Valid(), "table iterator: key requires Valid");
    return table_->key();
  }

  std::string_view value() const override {
    SHALE_CHECK(Valid(), "table iterator: value requires Valid");
    return table_->value();
  }

  Status status() const override { return status_.ok() ? table_->status() : status_; }

 private:
  // After a seek there is no key before, and after a step there is the one that was returned.
  void Check(bool after_seek) {
    if (!table_->Valid()) {
      return;
    }
    const std::string_view key = table_->key();
    if (!after_seek && CompareInternalKey(previous_key_, key) >= 0) {
      status_ = Status::Corruption("keys in the table are not in increasing order");
      return;
    }
    previous_key_.assign(key);
  }

  const std::unique_ptr<Iterator> table_;
  std::string previous_key_;  // the key returned last, while the iterator is valid
  Status status_;
};

}  // namespace

size_t BlockCacheKeyHash::operator()(const BlockCacheKey& key) const {
  // The finalizer of MurmurHash3's 64-bit hash, over the two fields combined.
  uint64_t h = key.cache_id * 0x9e3779b97f4a7c15ULL + key.offset;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return static_cast<size_t>(h);
}

Table::Table(PrivateTag /*tag*/, std::unique_ptr<RandomAccessFile> file, uint64_t cache_id,
             BlockCache* cache, uint64_t data_end, std::shared_ptr<const Block> index_block,
             BlockContents filter)
    : file_(std::move(file)),
      cache_id_(cache_id),
      cache_(cache),
      data_end_(data_end),
      index_block_(std::move(index_block)),
      filter_(std::move(filter)) {}

Status Table::Open(std::unique_ptr<RandomAccessFile> file, uint64_t file_size, uint64_t cache_id,
                   BlockCache* cache, std::shared_ptr<Table>* table) {
  SHALE_CHECK(file != nullptr, "Table::Open needs a file");
  SHALE_CHECK(table != nullptr, "Table::Open needs somewhere to put the table");
  table->reset();

  if (file_size < kFooterSize) {
    return Status::Corruption("file is too short to be an sstable");
  }
  char footer_scratch[kFooterSize];
  std::string_view footer_bytes;
  Status s = file->Read(file_size - kFooterSize, kFooterSize, &footer_bytes, footer_scratch);
  if (!s.ok()) {
    return s;
  }
  if (footer_bytes.size() != kFooterSize) {
    return Status::Corruption("truncated footer read");
  }
  Footer footer;
  s = footer.DecodeFrom(footer_bytes);
  if (!s.ok()) {
    return s;
  }

  // A table is its data blocks, the filter block, the index block and the footer, in that order,
  // so both handles lie before the footer and the filter's block ends where the index's may begin.
  const uint64_t blocks_end = file_size - kFooterSize;
  if (!HandleFits(footer.filter, blocks_end) || !HandleFits(footer.index, blocks_end) ||
      footer.filter.offset + footer.filter.size + kBlockTrailerSize > footer.index.offset) {
    return Status::Corruption("bad block handle in footer");
  }

  // The index and the filter are read once and kept, so they are always checked, whatever a later
  // read does about the checksums of data blocks.
  BlockContents index_contents;
  s = ReadBlock(file.get(), footer.index, true, &index_contents);
  if (!s.ok()) {
    return s;
  }
  BlockContents filter;
  s = ReadBlock(file.get(), footer.filter, true, &filter);
  if (!s.ok()) {
    return s;
  }
  auto index_block = std::make_shared<const Block>(std::move(index_contents));
  s = ValidateIndex(index_block, footer.filter.offset);
  if (!s.ok()) {
    return s;
  }

  *table = std::make_shared<Table>(PrivateTag{}, std::move(file), cache_id, cache,
                                   footer.filter.offset, std::move(index_block), std::move(filter));
  return Status::OK();
}

Status Table::ReadDataBlock(const TableReadOptions& options, std::string_view index_value,
                            std::shared_ptr<const Block>* block) const {
  BlockHandle handle;
  Status s = DecodeDataHandle(index_value, data_end_, &handle);
  if (!s.ok()) {
    return s;
  }
  const BlockCacheKey key{cache_id_, handle.offset};
  if (cache_ != nullptr) {
    *block = cache_->Lookup(key);
    if (*block != nullptr) {
      return Status::OK();
    }
  }

  data_block_reads_.fetch_add(1, std::memory_order_relaxed);
  BlockContents contents;
  s = ReadBlock(file_.get(), handle, options.verify_checksums, &contents);
  if (!s.ok()) {
    return s;
  }
  std::shared_ptr<const Block> read = std::make_shared<const Block>(std::move(contents));
  // Only a block that has been checked goes in. The cache is shared with reads that check, and a
  // block that nothing has checked, handed to one of them, would be read as if something had.
  if (cache_ != nullptr && options.fill_cache && options.verify_checksums) {
    cache_->Insert(key, read, read->size());
  }
  *block = std::move(read);
  return Status::OK();
}

std::unique_ptr<Iterator> Table::NewDataBlockIterator(const TableReadOptions& options,
                                                      std::string_view keyed_value) const {
  std::string_view last_key;
  std::string_view index_value;
  SplitKeyedValue(keyed_value, &last_key, &index_value);
  std::shared_ptr<const Block> block;
  Status s = ReadDataBlock(options, index_value, &block);
  if (!s.ok()) {
    return NewErrorIterator(std::move(s));
  }
  return std::make_unique<BoundedBlockIterator>(
      NewBlockIterator(std::move(block), CompareInternalKey, kInternalKeyTagSize),
      std::string(last_key));
}

std::unique_ptr<Iterator> Table::NewIterator(const TableReadOptions& options) const {
  // The function holds the table, and the two-level iterator holds the function.
  std::shared_ptr<const Table> self = shared_from_this();
  return std::make_unique<OrderedIterator>(
      NewTwoLevelIterator(std::make_unique<KeyedIndexIterator>(NewBlockIterator(
                              index_block_, CompareInternalKey, kInternalKeyTagSize)),
                          [self = std::move(self), options](std::string_view keyed_value) {
                            return self->NewDataBlockIterator(options, keyed_value);
                          }));
}

Status Table::Get(const TableReadOptions& options, std::string_view internal_key,
                  TableGetResult* result) const {
  SHALE_CHECK(internal_key.size() >= kInternalKeyTagSize, "Table::Get needs an internal key");
  SHALE_CHECK(result != nullptr, "Table::Get needs somewhere to put the result");
  result->kind = TableGetResult::Kind::kNotFound;
  result->value.clear();

  const std::string_view user_key = ExtractUserKey(internal_key);
  if (!BloomFilterMayMatch(filter_.view(), user_key)) {
    return Status::OK();
  }

  // The first block whose last key is at or after the target is the only one that can hold the
  // first entry at or after it: every block before it ends before the target.
  std::unique_ptr<Iterator> index_iter =
      NewBlockIterator(index_block_, CompareInternalKey, kInternalKeyTagSize);
  index_iter->Seek(internal_key);
  if (!index_iter->Valid()) {
    return index_iter->status();  // OK means the target is after every entry of the table
  }
  std::shared_ptr<const Block> block;
  Status s = ReadDataBlock(options, index_iter->value(), &block);
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<Iterator> block_iter =
      NewBlockIterator(std::move(block), CompareInternalKey, kInternalKeyTagSize);
  block_iter->Seek(internal_key);
  if (!block_iter->Valid()) {
    s = block_iter->status();
    if (!s.ok()) {
      return s;
    }
    // The index says the block ends at a key that is not before the target, and the block holds
    // no such key. That is a table that contradicts itself, not a key that is not in it.
    return Status::Corruption("data block does not hold the last key its index entry names");
  }

  // The entry found is not after the last key the index names for the block. If it were, a lookup
  // for the key it holds would have been sent to the next block, and this one is lying about
  // what it ends with.
  if (CompareInternalKey(block_iter->key(), index_iter->key()) > 0) {
    return Status::Corruption("data block holds a key after the last key its index entry names");
  }

  ParsedInternalKey found;
  if (!ParseInternalKey(block_iter->key(), &found)) {
    return Status::Corruption("bad internal key in data block");
  }
  if (BytewiseCompare(found.user_key, user_key) != 0) {
    return Status::OK();
  }
  switch (found.type) {
    case ValueType::kValue:
      result->kind = TableGetResult::Kind::kFound;
      result->value.assign(block_iter->value());
      return Status::OK();
    case ValueType::kDeletion:
      result->kind = TableGetResult::Kind::kDeleted;
      return Status::OK();
  }
  return Status::Corruption("bad internal key in data block");  // ParseInternalKey allows no more
}

}  // namespace shale
