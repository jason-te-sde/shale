#include "io/table.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "io/block.h"
#include "io/bloom.h"
#include "io/readahead.h"
#include "io/table_format.h"
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
             BlockContents filter, std::string name)
    : file_(std::move(file)),
      name_(std::move(name)),
      cache_id_(cache_id),
      cache_(cache),
      data_end_(data_end),
      index_block_(std::move(index_block)),
      filter_(std::move(filter)) {}

Status Table::Open(std::unique_ptr<RandomAccessFile> file, uint64_t file_size, uint64_t cache_id,
                   BlockCache* cache, std::string name, std::shared_ptr<Table>* table) {
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

  *table =
      std::make_shared<Table>(PrivateTag{}, std::move(file), cache_id, cache, footer.filter.offset,
                              std::move(index_block), std::move(filter), std::move(name));
  return Status::OK();
}

Status Table::ReadDataBlock(const TableReadOptions& options, std::string_view index_value,
                            Readahead* readahead, std::shared_ptr<const Block>* block) const {
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
  std::shared_ptr<const Block> read;
  const size_t stored_size = static_cast<size_t>(handle.size) + kBlockTrailerSize;
  if (stored_size <= kReadChunkSize) {
    // The block and its trailer are one allocation with the block object, whether they come out of
    // the readahead buffer or off the disk. A handle larger than one chunk is not trusted to name
    // a buffer before the file has supplied any of it, and is read as ReadBlock() reads.
    bool have = false;
    if (readahead != nullptr) {
      std::string_view stored;
      bool found = readahead->Find(handle, &stored);
      if (!found && readahead->ShouldFill(handle)) {
        // The limit is where the filter block begins, so that a fill never reads past the data.
        s = readahead->Fill(*file_, handle, data_end_);
        if (!s.ok()) {
          return s;
        }
        found = readahead->Find(handle, &stored);
      }
      if (found) {
        // What came out of the buffer was read for the sake of other blocks and has been checked
        // by no one. It is copied into a buffer of its own, which is the block's, and checked as a
        // block read alone is.
        s = Block::MakeShared(
            static_cast<size_t>(handle.size), kBlockTrailerSize,
            [&](char* bytes) {
              std::memcpy(bytes, stored.data(), stored.size());
              return CheckBlockTrailer(std::string_view(bytes, stored.size()),
                                       options.verify_checksums);
            },
            &read);
        have = s.ok();
        if (!have) {
          // A failure here may be the read that filled the buffer and not the block: a byte that
          // came back wrong once is not a block that is wrong. The buffer is dropped and the block
          // is read again, by itself, and that read decides.
          readahead->Discard();
          data_block_reads_.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    if (!have) {
      s = Block::MakeShared(
          static_cast<size_t>(handle.size), kBlockTrailerSize,
          [&](char* bytes) {
            return ReadBlockInto(file_.get(), handle, options.verify_checksums, bytes);
          },
          &read);
      if (!s.ok()) {
        return s;
      }
    }
  } else {
    BlockContents contents;
    s = ReadBlock(file_.get(), handle, options.verify_checksums, &contents);
    if (!s.ok()) {
      return s;
    }
    read = std::make_shared<const Block>(std::move(contents));
  }
  // Only a block that has been checked goes in. The cache is shared with reads that check, and a
  // block that nothing has checked, handed to one of them, would be read as if something had.
  if (cache_ != nullptr && options.fill_cache && options.verify_checksums) {
    cache_->Insert(key, read, read->size());
  }
  *block = std::move(read);
  return Status::OK();
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
  BlockIterator index_iter(index_block_, CompareInternalKey, kInternalKeyTagSize);
  index_iter.Seek(internal_key);
  if (!index_iter.Valid()) {
    return index_iter.status();  // OK means the target is after every entry of the table
  }
  std::shared_ptr<const Block> block;
  Status s = ReadDataBlock(options, index_iter.value(), nullptr, &block);
  if (!s.ok()) {
    return s;
  }

  BlockIterator block_iter(std::move(block), CompareInternalKey, kInternalKeyTagSize);
  block_iter.Seek(internal_key);
  if (!block_iter.Valid()) {
    s = block_iter.status();
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
  if (CompareInternalKey(block_iter.key(), index_iter.key()) > 0) {
    return Status::Corruption("data block holds a key after the last key its index entry names");
  }

  ParsedInternalKey found;
  if (!ParseInternalKey(block_iter.key(), &found)) {
    return Status::Corruption("bad internal key in data block");
  }
  if (BytewiseCompare(found.user_key, user_key) != 0) {
    return Status::OK();
  }
  switch (found.type) {
    case ValueType::kValue:
      result->kind = TableGetResult::Kind::kFound;
      result->value.assign(block_iter.value());
      return Status::OK();
    case ValueType::kDeletion:
      result->kind = TableGetResult::Kind::kDeleted;
      return Status::OK();
  }
  return Status::Corruption("bad internal key in data block");  // ParseInternalKey allows no more
}

}  // namespace shale
