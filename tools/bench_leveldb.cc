#include "bench_leveldb.h"

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/env.h>
#include <leveldb/filter_policy.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "bench_engine.h"
#include "shale/status.h"

namespace shale {
namespace {

// The settings that put LevelDB level with shale's defaults. Where the two engines have the same
// knob, it has shale's default value here; what LevelDB has no knob for is not set. The settings
// that stay LevelDB's own, and that differ from shale's, are in docs/benchmarks.md.
constexpr size_t kWriteBufferSize = size_t{4} * 1024 * 1024;
constexpr size_t kBlockSize = 4096;
constexpr int kBloomBitsPerKey = 10;
constexpr size_t kBlockCacheCapacity = size_t{8} * 1024 * 1024;
constexpr int kMaxOpenFiles = 1000;

leveldb::Slice ToSlice(std::string_view text) { return leveldb::Slice(text.data(), text.size()); }

// LevelDB's status as one of ours, keeping its text. A status that is none of the kinds a caller
// tells apart is an IOError, which is what a failure of the engine is.
Status ToStatus(const leveldb::Status& status) {
  if (status.ok()) {
    return Status::OK();
  }
  const std::string text = status.ToString();
  if (status.IsNotFound()) {
    return Status::NotFound("leveldb", text);
  }
  if (status.IsCorruption()) {
    return Status::Corruption("leveldb", text);
  }
  if (status.IsInvalidArgument()) {
    return Status::InvalidArgument("leveldb", text);
  }
  if (status.IsNotSupportedError()) {
    return Status::NotSupported("leveldb", text);
  }
  return Status::IOError("leveldb", text);
}

class LevelDBBenchEngine final : public BenchEngine {
 public:
  explicit LevelDBBenchEngine(leveldb::Env* env)
      : filter_(leveldb::NewBloomFilterPolicy(kBloomBitsPerKey)) {
    options_.create_if_missing = true;
    options_.write_buffer_size = kWriteBufferSize;
    options_.block_size = kBlockSize;
    options_.filter_policy = filter_.get();
    options_.max_open_files = kMaxOpenFiles;
    options_.compression = leveldb::kNoCompression;
    if (env != nullptr) {
      options_.env = env;
    }
  }

  std::string Name() const override { return "leveldb"; }

  Status Open(const std::string& path, bool fresh) override {
    Close();
    // The cache is new at every open, as the cache that is part of a shale database is: a database
    // that was opened again starts with nothing cached, and not with what the one before it read.
    cache_.reset(leveldb::NewLRUCache(kBlockCacheCapacity));
    options_.block_cache = cache_.get();
    if (fresh) {
      if (const leveldb::Status destroyed = leveldb::DestroyDB(path, options_); !destroyed.ok()) {
        return ToStatus(destroyed);
      }
    }
    leveldb::DB* db = nullptr;
    const leveldb::Status opened = leveldb::DB::Open(options_, path, &db);
    db_.reset(db);
    return ToStatus(opened);
  }

  Status Put(std::string_view key, std::string_view value, bool sync) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    leveldb::WriteOptions write_options;
    write_options.sync = sync;
    return ToStatus(db_->Put(write_options, ToSlice(key), ToSlice(value)));
  }

  Status Get(std::string_view key, std::string* value) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    return ToStatus(db_->Get(VerifiedReadOptions(), ToSlice(key), value));
  }

  Status Scan(std::string_view from, size_t limit, size_t* seen) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    const std::unique_ptr<leveldb::Iterator> iterator(db_->NewIterator(VerifiedReadOptions()));
    if (from.empty()) {
      iterator->SeekToFirst();
    } else {
      iterator->Seek(ToSlice(from));
    }
    size_t visited = 0;
    for (; iterator->Valid() && (limit == 0 || visited < limit); iterator->Next()) {
      // As for shale: the key and the value are produced, and their bytes are not read.
      static_cast<void>(iterator->key());
      static_cast<void>(iterator->value());
      visited++;
    }
    if (const Status status = ToStatus(iterator->status()); !status.ok()) {
      return status;
    }
    *seen = visited;
    return Status::OK();
  }

  Status CompactAll() override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    db_->CompactRange(nullptr, nullptr);
    return Status::OK();
  }

  // LevelDB's statistics give the bytes that each level read and wrote, not a ratio of what reached
  // the disk to what the callers gave it that the adapter could report without working it out
  // itself, and a figure of the adapter's own making would not be LevelDB's.
  std::string WriteAmplification() override { return "n/a"; }

  void Close() override { db_.reset(); }

 private:
  static Status NotOpen() { return Status::InvalidArgument("the benchmark database is not open"); }

  // What shale reads with, which is checksummed: LevelDB's default is not to verify, so it is
  // asked to. A read fills the cache, as it does by default in both.
  static leveldb::ReadOptions VerifiedReadOptions() {
    leveldb::ReadOptions read_options;
    read_options.verify_checksums = true;
    return read_options;
  }

  const std::unique_ptr<const leveldb::FilterPolicy> filter_;
  leveldb::Options options_;
  // Declared after the cache and the filter policy that the database uses, so that it is destroyed
  // before them.
  std::unique_ptr<leveldb::Cache> cache_;
  std::unique_ptr<leveldb::DB> db_;
};

}  // namespace

std::unique_ptr<BenchEngine> NewLevelDBBenchEngine(leveldb::Env* env) {
  return std::make_unique<LevelDBBenchEngine>(env);
}

}  // namespace shale
