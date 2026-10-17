#include "db/table_cache.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "db/named_status.h"
#include "io/iterator_util.h"
#include "io/table.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/filename.h"

namespace shale {

namespace {

// The most shards a cache is divided into, as a power of two, and the fewest tables a shard may
// have room for.
constexpr int kMaxShardBits = 4;
constexpr size_t kMinTablesPerShard = 16;

}  // namespace

int TableCache::ShardBitsFor(size_t capacity) {
  int bits = 0;
  while (bits < kMaxShardBits && (capacity >> (bits + 1)) >= kMinTablesPerShard) {
    bits++;
  }
  return bits;
}

TableCache::TableCache(std::string dbname, Env* env, BlockCache* block_cache, size_t capacity)
    : dbname_(std::move(dbname)),
      env_(env),
      block_cache_(block_cache),
      cache_(capacity, ShardBitsFor(capacity)) {}

Status TableCache::FindTable(uint64_t number, uint64_t file_size, std::shared_ptr<Table>* table) {
  table->reset();
  std::shared_ptr<Table> found = cache_.Lookup(number);
  if (found != nullptr) {
    *table = std::move(found);
    return Status::OK();
  }

  // Two threads that miss together both open the file, and the later insert replaces the earlier.
  // The earlier table stays alive for whoever got it, and both read the same bytes, so the only
  // cost is the opening, once.
  const std::string fname = TableFileName(dbname_, number);
  std::unique_ptr<RandomAccessFile> file;
  Status s = env_->NewRandomAccessFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  std::shared_ptr<Table> opened;
  s = Table::Open(std::move(file), file_size, number, block_cache_, &opened);
  if (!s.ok()) {
    return WithFileName(s, fname);
  }
  cache_.Insert(number, opened, 1);
  *table = std::move(opened);
  return Status::OK();
}

std::unique_ptr<Iterator> TableCache::NewIterator(const TableReadOptions& options, uint64_t number,
                                                  uint64_t file_size) {
  std::shared_ptr<Table> table;
  const Status s = FindTable(number, file_size, &table);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }
  // The iterator holds the table, so it outlives the cache's share of it.
  return table->NewIterator(options);
}

Status TableCache::Get(const TableReadOptions& options, uint64_t number, uint64_t file_size,
                       std::string_view internal_key, TableGetResult* result) {
  std::shared_ptr<Table> table;
  Status s = FindTable(number, file_size, &table);
  if (!s.ok()) {
    return s;
  }
  s = table->Get(options, internal_key, result);
  return WithFileName(s, TableFileName(dbname_, number));
}

void TableCache::Evict(uint64_t number) { cache_.Erase(number); }

}  // namespace shale
