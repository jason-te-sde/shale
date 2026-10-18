#include "bench_engine.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "shale/db.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"

namespace shale {

BenchEngine::~BenchEngine() = default;

namespace {

class ShaleBenchEngine final : public BenchEngine {
 public:
  explicit ShaleBenchEngine(const Options& base) : options_(base) {
    options_.create_if_missing = true;
  }

  std::string Name() const override { return "shale"; }

  Status Open(const std::string& path, bool fresh) override {
    Close();
    if (fresh) {
      if (const Status destroyed = DestroyDB(path, options_); !destroyed.ok()) {
        return destroyed;
      }
    }
    return DB::Open(options_, path, &db_);
  }

  Status Put(std::string_view key, std::string_view value, bool sync) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    WriteOptions write_options;
    write_options.sync = sync;
    return db_->Put(write_options, key, value);
  }

  Status Get(std::string_view key, std::string* value) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    return db_->Get(ReadOptions(), key, value);
  }

  Status Scan(std::string_view from, size_t limit, size_t* seen) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    const std::unique_ptr<Iterator> iterator = db_->NewIterator(ReadOptions());
    if (from.empty()) {
      iterator->SeekToFirst();
    } else {
      iterator->Seek(from);
    }
    size_t visited = 0;
    for (; iterator->Valid() && (limit == 0 || visited < limit); iterator->Next()) {
      // A scan hands the caller the entry, so the key and the value are both produced. Their bytes
      // are not read: the engine's work is what is measured, as db_bench does.
      static_cast<void>(iterator->key());
      static_cast<void>(iterator->value());
      visited++;
    }
    if (const Status status = iterator->status(); !status.ok()) {
      return status;
    }
    *seen = visited;
    return Status::OK();
  }

  Status CompactAll() override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    return db_->CompactRange(std::nullopt, std::nullopt);
  }

  std::string WriteAmplification() override {
    std::string figure;
    if (db_ == nullptr || !db_->GetProperty("shale.write-amplification", &figure)) {
      return "n/a";
    }
    return figure;
  }

  void Close() override { db_.reset(); }

 private:
  static Status NotOpen() { return Status::InvalidArgument("the benchmark database is not open"); }

  Options options_;
  std::unique_ptr<DB> db_;
};

}  // namespace

std::unique_ptr<BenchEngine> NewShaleBenchEngine(const Options& base) {
  return std::make_unique<ShaleBenchEngine>(base);
}

}  // namespace shale
