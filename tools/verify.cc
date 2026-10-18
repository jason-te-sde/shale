#include "verify.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/named_status.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "io/table.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/dbformat.h"
#include "util/filename.h"

namespace shale {

namespace {

// How many tables the check keeps open at once. It reads each table once, front to back, so a
// handful is plenty.
constexpr size_t kOpenTables = 16;

// Checks one table of the recovered state, and appends what is wrong with it, if anything, to
// `*problems`: the first defect only, since what comes after the first is mostly what it caused.
void CheckTable(Env* env, TableCache* table_cache, const std::string& dbname,
                const FileMetaData& file, std::vector<std::string>* problems) {
  const std::string path = TableFileName(dbname, file.number);
  uint64_t size = 0;
  Status s = env->GetFileSize(path, &size);
  if (!s.ok()) {
    problems->push_back(s.ToString());
    return;
  }
  if (size != file.file_size) {
    // Reading the table as the MANIFEST sizes it would find nothing wrong with a file that has
    // bytes after its footer, so the sizes are compared first.
    problems->push_back(Status::Corruption("the table's size is " + std::to_string(size) +
                                               " bytes and the MANIFEST records " +
                                               std::to_string(file.file_size),
                                           path)
                            .ToString());
    return;
  }
  TableReadOptions options;
  options.verify_checksums = true;
  options.fill_cache = false;
  const std::unique_ptr<Iterator> iter = table_cache->NewIterator(options, file.number, size);
  s = CheckTableEntries(iter.get(), file, path);
  if (!s.ok()) {
    problems->push_back(s.ToString());
  }
}

// Recovers the state that CURRENT names and checks every table in it.
void CheckDatabase(Env* env, const std::string& dbname, std::vector<std::string>* problems) {
  TableCache table_cache(dbname, env, /*block_cache=*/nullptr, kOpenTables);
  VersionSet versions(dbname, env, &table_cache);
  const Status recovered = versions.Recover();
  if (!recovered.ok()) {
    problems->push_back(recovered.ToString());
    return;
  }
  const std::shared_ptr<const Version> version = versions.current();
  for (const std::vector<FileMetaData>& level : version->state().files) {
    for (const FileMetaData& file : level) {
      CheckTable(env, &table_cache, dbname, file, problems);
    }
  }
}

}  // namespace

Status VerifyDatabase(Env* env, const std::string& dbname, std::vector<std::string>* problems) {
  problems->clear();
  // Before the lock: taking it creates the file, and a directory with no database is left as it is.
  if (!env->FileExists(CurrentFileName(dbname))) {
    return Status::InvalidArgument(dbname + " does not hold a database (it has no CURRENT file)");
  }
  std::unique_ptr<FileLock> lock;
  Status locked = env->LockFile(LockFileName(dbname), &lock);
  if (!locked.ok()) {
    return locked;
  }
  CheckDatabase(env, dbname, problems);
  return Status::OK();
}

Status CheckTableEntries(Iterator* iter, const FileMetaData& file, const std::string& path) {
  ParsedInternalKey bound;
  if (!ParseInternalKey(file.smallest, &bound) || !ParseInternalKey(file.largest, &bound)) {
    return Status::Corruption("the MANIFEST records a range that is not made of internal keys",
                              path);
  }
  std::string previous;
  uint64_t entry = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    entry++;
    const std::string_view key = iter->key();
    const std::string at = "entry " + std::to_string(entry);
    ParsedInternalKey parsed;
    if (!ParseInternalKey(key, &parsed)) {
      return Status::Corruption(at + " has a key that is not an internal key", path);
    }
    if (entry > 1 && CompareInternalKey(previous, key) >= 0) {
      return Status::Corruption(at + " is not after the entry before it", path);
    }
    if (CompareInternalKey(key, file.smallest) < 0) {
      return Status::Corruption(
          at + " is below the smallest key the MANIFEST records for the table", path);
    }
    if (CompareInternalKey(key, file.largest) > 0) {
      return Status::Corruption(at + " is above the largest key the MANIFEST records for the table",
                                path);
    }
    previous.assign(key);
  }
  // The iterator the table cache hands out has named the file in its errors already. One taken from
  // a table directly has not: a block that fails its checksum knows where in the file it is and not
  // which file. Either way the file is named once.
  Status status = iter->status();
  if (status.ok() || status.ToString().find(path) != std::string::npos) {
    return status;
  }
  return WithFileName(status, path);
}

}  // namespace shale
