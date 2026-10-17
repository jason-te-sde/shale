#ifndef SHALE_IO_TABLE_H_
#define SHALE_IO_TABLE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "io/block.h"
#include "io/lru_cache.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"

namespace shale {

/// What a cached data block is called: the table it is in and where in the file it starts. A
/// table's `cache_id` is chosen by whoever opens it and must differ from that of every other table
/// that shares the cache, a file number being the natural choice.
struct BlockCacheKey {
  uint64_t cache_id;
  uint64_t offset;

  bool operator==(const BlockCacheKey&) const = default;
};

/// Hashes a BlockCacheKey. The offsets of blocks are close to multiples of the block size, so the
/// hash mixes both fields into every bit of its result.
struct BlockCacheKeyHash {
  size_t operator()(const BlockCacheKey& key) const;
};

/// The cache of data blocks that tables share. It is charged by block size in bytes. A reader that
/// gets a block from it holds the block, so evicting one that an iterator is reading from is safe.
using BlockCache = ShardedLRUCache<BlockCacheKey, const Block, BlockCacheKeyHash>;

/// How one read of a table is done.
struct TableReadOptions {
  /// Whether to check the checksum of each data block read from the file. A block that was
  /// cached by an earlier read is not checked again, so a read that does not verify can leave an
  /// unchecked block in the cache for one that does.
  bool verify_checksums = true;
  /// Whether a data block read from the file goes into the block cache. A bulk scan sets it to
  /// false so that it does not push out the blocks that point lookups are using. Blocks already
  /// in the cache are used either way.
  bool fill_cache = true;
};

/// What Table::Get() found.
struct TableGetResult {
  /// What the table holds for the key.
  enum class Kind : uint8_t {
    kNotFound,  ///< No entry for the user key at or below the lookup's sequence number.
    kFound,     ///< The newest such entry sets the key; `value` is its value.
    kDeleted,   ///< The newest such entry deletes the key.
  };

  Kind kind = Kind::kNotFound;
  std::string value;  ///< The value for kFound, and empty otherwise.
};

/// A table opened for reading: the sorted entries in one file, found by key through an index and
/// skipped by a bloom filter, with the blocks they are in read through a shared cache.
///
/// Everything in a table comes off a disk, and nothing in it is trusted. Open() reads the footer,
/// the index and the filter and checks what it can without reading the data: that the footer,
/// index block and filter block are intact; that the index is a sorted list of keys with a handle
/// each, every handle naming a different block in the part of the file before the filter, in
/// order. After that, whatever the bytes of the data blocks are, a read returns the entry that
/// was written or a Corruption status, never a crash, an out-of-bounds read or a value that
/// was not stored. What a checksum cannot show, a file written by something that computes a
/// checksum for whatever it writes, is handled by the decoders staying inside their buffers, and
/// that is all that can be promised for it.
///
/// A Table is immutable once opened and every method may be called from any number of threads at
/// once. It is owned by shared pointers, and an iterator holds one, so a table is closed when the
/// last reader of it is done, however the caller's own pointer to it is dropped.
class Table : public std::enable_shared_from_this<Table> {
 public:
  /// Opens a table of `file_size` bytes in `file`, reading its footer, index block and filter
  /// block. Returns Corruption for a file that is not a table or has a damaged footer, index or
  /// filter, and an error from the file as it is. On failure `*table` is null.
  ///
  /// The size is taken from the caller, which has just listed or written the file; a size that is
  /// not the file's makes the footer be read from the wrong place and fails as Corruption.
  /// @param file the file the table is in. The table owns it.
  /// @param file_size the size of the file in bytes.
  /// @param cache_id the table's identity in `cache`; see BlockCacheKey.
  /// @param cache the cache of data blocks to read through, or null for none. It is not owned and
  ///   must outlive the table and every iterator of it.
  /// @param table receives the table.
  static Status Open(std::unique_ptr<RandomAccessFile> file, uint64_t file_size, uint64_t cache_id,
                     BlockCache* cache, std::shared_ptr<Table>* table);

  /// Returns an iterator over every entry of the table, internal key and value, in order. It holds
  /// the table alive. A data block that cannot be read ends the iteration with its error.
  /// @param options how to read.
  std::unique_ptr<Iterator> NewIterator(const TableReadOptions& options) const;

  /// Looks up the first entry at or after `internal_key`, and reports it only if its user key is
  /// that of `internal_key`. Since the entries for a user key are ordered newest first, a lookup
  /// key built with a sequence number finds the newest entry the sequence number can see. When
  /// the bloom filter shows that the table holds no entry for the user key, the lookup ends there
  /// and reads no data block.
  ///
  /// Returns OK, with `*result` saying what was found, or Corruption if the data block that has to
  /// hold the entry is damaged, whether or not it is the entry that was asked for: a block that
  /// cannot be trusted cannot say that a key is absent either. Aborts if `internal_key` is shorter
  /// than the 8-byte tag.
  /// @param options how to read.
  /// @param internal_key a user key followed by a tag, as LookupKey makes it.
  /// @param result receives the result; its previous contents are discarded.
  Status Get(const TableReadOptions& options, std::string_view internal_key,
             TableGetResult* result) const;

  /// Returns the number of data blocks read from the file so far, which does not count one that
  /// was served by the block cache. A block that was read and failed its checks counts.
  uint64_t data_block_reads() const { return data_block_reads_.load(std::memory_order_relaxed); }

  // The constructor is public for std::make_shared and unusable otherwise: it takes a type that
  // only Open() can make.
  struct PrivateTag {
    explicit PrivateTag() = default;
  };
  Table(PrivateTag, std::unique_ptr<RandomAccessFile> file, uint64_t cache_id, BlockCache* cache,
        uint64_t data_end, std::shared_ptr<const Block> index_block, std::string filter);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

 private:
  // Reads the block the index value `index_value` names, from the cache if it is there.
  Status ReadDataBlock(const TableReadOptions& options, std::string_view index_value,
                       std::shared_ptr<const Block>* block) const;

  // An iterator over that block, or one that reports why there is none.
  std::unique_ptr<Iterator> NewDataBlockIterator(const TableReadOptions& options,
                                                 std::string_view index_value) const;

  const std::unique_ptr<RandomAccessFile> file_;
  const uint64_t cache_id_;
  BlockCache* const cache_;
  // Where the data blocks end and the filter block begins. An index entry's handle must lie before.
  const uint64_t data_end_;
  const std::shared_ptr<const Block> index_block_;
  const std::string filter_;  // empty if the table was written without one
  mutable std::atomic<uint64_t> data_block_reads_{0};
};

}  // namespace shale

#endif  // SHALE_IO_TABLE_H_
