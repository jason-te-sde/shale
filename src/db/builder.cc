#include "db/builder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "core/version_edit.h"
#include "db/filename_io.h"
#include "db/table_cache.h"
#include "io/table.h"
#include "io/table_builder.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/filename.h"

namespace shale {

namespace {

// Abandons a TableBuilder that goes out of scope without having been finished. A builder that is
// destroyed neither finished nor abandoned ends the process, so every way out of the scope that
// holds one, an early return included, has to end it.
class AbandonUnlessDisarmed {
 public:
  explicit AbandonUnlessDisarmed(TableBuilder* builder) : builder_(builder) {}
  ~AbandonUnlessDisarmed() {
    if (builder_ != nullptr) {
      builder_->Abandon();
    }
  }

  // Called once the builder has been ended another way.
  void Disarm() { builder_ = nullptr; }

  AbandonUnlessDisarmed(const AbandonUnlessDisarmed&) = delete;
  AbandonUnlessDisarmed& operator=(const AbandonUnlessDisarmed&) = delete;

 private:
  TableBuilder* builder_;
};

}  // namespace

Status BuildTable(const std::string& dbname, Env* env, const TableBuilderOptions& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta) {
  meta->file_size = 0;
  meta->smallest.clear();
  meta->largest.clear();

  iter->SeekToFirst();
  if (!iter->Valid()) {
    // No entry is either an empty input or a failed one, and the status says which.
    return iter->status();
  }

  const std::string fname = TableFileName(dbname, meta->number);
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(fname, &file);
  if (!s.ok()) {
    return s;  // Nothing was created.
  }

  std::string smallest(iter->key());
  std::string largest;
  uint64_t file_size = 0;
  {
    TableBuilder builder(options, file.get());
    AbandonUnlessDisarmed abandon(&builder);
    for (; iter->Valid() && builder.status().ok(); iter->Next()) {
      builder.Add(iter->key(), iter->value());
      largest.assign(iter->key());
    }
    // An iterator that stopped on an error has not run out of entries, and a table of the entries
    // it did return would lose the rest without a word.
    s = iter->status();
    if (s.ok()) {
      // Finish() ends the builder, with an error too: there is nothing left to abandon.
      abandon.Disarm();
      s = builder.Finish();
      file_size = builder.FileSize();
    }
  }

  if (s.ok()) {
    s = file->Sync();
  }
  // Closed in every case: a handle is only released by Close(), and it is no use after an error.
  const Status close_status = file->Close();
  if (s.ok()) {
    s = close_status;
  }
  file.reset();

  if (s.ok()) {
    // Open what was written, and so read its footer, index and filter, before anything refers to
    // it. A table that cannot be opened is found out here, with the data it was made from still at
    // hand, and not by a read that has nothing to fall back on.
    std::shared_ptr<Table> table;
    s = table_cache->FindTable(meta->number, file_size, &table);
  }
  if (s.ok()) {
    // The file is durable, but the directory entry that names it is not until now.
    s = SyncDirectory(env, dbname);
  }

  if (!s.ok()) {
    // The first failure is the one to report; one while cleaning up after it must not replace it.
    table_cache->Evict(meta->number);
    static_cast<void>(env->RemoveFile(fname));
    return s;
  }
  meta->file_size = file_size;
  meta->smallest = std::move(smallest);
  meta->largest = std::move(largest);
  return Status::OK();
}

}  // namespace shale
