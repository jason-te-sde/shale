#ifndef SHALE_DB_TABLE_CACHE_H_
#define SHALE_DB_TABLE_CACHE_H_

// The open tables, kept so that a read does not open, and read the footer, index and filter of, a
// file for every key it looks up.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "io/lru_cache.h"
#include "io/table.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"

namespace shale {

/// A cache of open Tables, by file number, charged 1 for each, so that its capacity is a number of
/// tables, which is what the limit on open files is. Safe to use from any number of threads.
///
/// A table is opened on the first read of it and found here after that. The least recently used is
/// dropped when the cache is over capacity, and that only gives up the cache's share of it: a
/// table that an iterator is reading from stays open until the iterator ends, however it was
/// evicted. What the cache cannot know is that a file has been deleted, so it serves a table whose
/// file is gone for as long as it holds it; whoever deletes a table file calls Evict() with its
/// number.
///
/// The data blocks that the tables read go through one shared BlockCache, under the file number as
/// the table's identity. A file number is never used for two files, which is what makes that
/// name safe, and why a block left in the block cache by a table that was evicted, or deleted, is
/// harmless: nothing will ask for it again, and it ages out.
///
/// It does not own the Env or the block cache, which must outlive it and every iterator it hands
/// out.
class TableCache {
 public:
  /// Creates an empty cache of tables for the database in `dbname`.
  /// @param dbname the database directory, where the table files are.
  /// @param env the Env to open files through.
  /// @param block_cache the cache of data blocks every table reads through; may be null for none.
  /// @param capacity how many tables to keep open; 0 keeps none, and every read opens its table.
  ///   It is divided among up to 16 shards, each of which orders its tables by use, but none gets
  ///   fewer than 16: a cache for 20 tables is not made of 16 shards of 1 that an uneven hash
  ///   would thrash.
  TableCache(std::string dbname, Env* env, BlockCache* block_cache, size_t capacity);

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  /// Finds table `number`, opening it if it is not in the cache. Returns what Table::Open() does
  /// for a file that cannot be opened or read, and an IOError that names the file for one that is
  /// not there. A table that failed to open is not remembered. On failure `*table` is null.
  ///
  /// The cache is keyed by `number` alone and trusts that a number is one file: the size is used
  /// only when the table is opened.
  /// @param number the file number of the table.
  /// @param file_size the size of the file in bytes, as the version set records it.
  /// @param table receives the table, which the caller holds as long as it likes.
  Status FindTable(uint64_t number, uint64_t file_size, std::shared_ptr<Table>* table);

  /// Returns an iterator over every entry of table `number`. It holds the table, so the cache may
  /// evict it, and the file may be deleted, while the iterator is in use. If the table cannot be
  /// opened the iterator is NewErrorIterator() with the reason, never null.
  /// @param options how to read.
  /// @param number the file number of the table.
  /// @param file_size the size of the file in bytes.
  std::unique_ptr<Iterator> NewIterator(const TableReadOptions& options, uint64_t number,
                                        uint64_t file_size);

  /// Looks `internal_key` up in table `number` as Table::Get() does. Returns an error if the table
  /// cannot be opened or read; the answer is in `*result` only when the status is OK.
  /// @param options how to read.
  /// @param number the file number of the table.
  /// @param file_size the size of the file in bytes.
  /// @param internal_key the user key and tag to look up, as a LookupKey makes them.
  /// @param result receives the answer.
  Status Get(const TableReadOptions& options, uint64_t number, uint64_t file_size,
             std::string_view internal_key, TableGetResult* result);

  /// Drops table `number` from the cache, if it is there. A reader that holds the table keeps it.
  /// Call it when the file is deleted, so that the cache does not go on serving a table that has
  /// no file.
  /// @param number the file number of the table.
  void Evict(uint64_t number);

 private:
  const std::string dbname_;
  Env* const env_;
  BlockCache* const block_cache_;
  ShardedLRUCache<uint64_t, Table> cache_;
};

}  // namespace shale

#endif  // SHALE_DB_TABLE_CACHE_H_
