#include "db/db_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "io/log_reader.h"
#include "io/log_writer.h"
#include "io/table.h"
#include "io/write_batch_internal.h"
#include "shale/db.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/filename.h"

namespace shale {

DBTestBase::DBTestBase() : env_(std::make_unique<FaultInjectionEnv>(1)), dbname_("/db") {
  // A fatal assertion cannot return from a constructor; a failure here shows up as the test's own
  // failure, and every use of db_ after it would fail too.
  EXPECT_OK(TryReopen(CurrentOptions()));
}

DBTestBase::~DBTestBase() { Close(); }

Options DBTestBase::CurrentOptions() {
  Options options;
  options.env = env_.get();
  options.scheduler = &scheduler_;
  options.create_if_missing = true;
  return options;
}

Status DBTestBase::TryReopen(const Options& options) {
  Close();
  return DB::Open(options, dbname_, &db_);
}

void DBTestBase::Reopen() { Reopen(CurrentOptions()); }

void DBTestBase::Reopen(const Options& options) { ASSERT_OK(TryReopen(options)); }

void DBTestBase::Close() { db_.reset(); }

Status DBTestBase::Put(std::string_view k, std::string_view v, bool sync) {
  WriteOptions options;
  options.sync = sync;
  return db_->Put(options, k, v);
}

Status DBTestBase::Delete(std::string_view k) { return db_->Delete(WriteOptions(), k); }

std::string DBTestBase::Get(std::string_view k, const Snapshot* snapshot) {
  ReadOptions options;
  options.snapshot = snapshot;
  std::string value;
  const Status s = db_->Get(options, k, &value);
  if (s.ok()) {
    return value;
  }
  if (s.IsNotFound()) {
    return "NOT_FOUND";
  }
  return s.ToString();
}

std::string DBTestBase::Contents(const Snapshot* snapshot) {
  ReadOptions options;
  options.snapshot = snapshot;
  const std::unique_ptr<Iterator> it = db_->NewIterator(options);
  std::string result;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (!result.empty()) {
      result.push_back(',');
    }
    result.append(it->key());
    result.push_back('=');
    result.append(it->value());
  }
  if (!it->status().ok()) {
    return it->status().ToString();
  }
  return result;
}

int DBTestBase::NumTableFilesAtLevel(int level) {
  const VersionState state = dbfull()->TEST_VersionState();
  return static_cast<int>(state.files[static_cast<size_t>(level)].size());
}

std::vector<std::string> DBTestBase::DbFiles() {
  std::vector<std::string> names;
  EXPECT_OK(env_->GetChildren(dbname_, &names));
  std::sort(names.begin(), names.end());
  return names;
}

DBImpl* DBTestBase::dbfull() { return static_cast<DBImpl*>(db_.get()); }

Options DBTestBase::SmallBuffer() {
  Options options = CurrentOptions();
  options.write_buffer_size = 16384;
  return options;
}

std::string DBTestBase::KiB(int i) { return std::string(1024, 'v') + ValueOf(i); }

std::vector<std::string> DBTestBase::FilesOfType(FileType type) {
  std::vector<std::string> names;
  for (const std::string& name : DbFiles()) {
    uint64_t number = 0;
    FileType found = FileType::kCurrentFile;
    if (ParseFileName(name, &number, &found) && found == type) {
      names.push_back(name);
    }
  }
  return names;
}

int DBTestBase::CountFiles(FileType type) { return static_cast<int>(FilesOfType(type).size()); }

int DBTestBase::PutUntilSwitched() {
  int written = 0;
  while (CountFiles(FileType::kLogFile) < 2) {
    EXPECT_LT(written, 100) << "the memtable never switched";
    if (written >= 100) {
      break;
    }
    EXPECT_OK(Put(UserKey(written), KiB(written), /*sync=*/false));
    written++;
  }
  return written;
}

HookEnv* DBTestBase::NewHookEnv() {
  // The database may be running on the one that is replaced.
  Close();
  hook_env_ = std::make_unique<HookEnv>(env_.get());
  return hook_env_.get();
}

bool PollUntil(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return true;
}

Status ForgeEmptyDatabase(Env* env, const std::string& dbname, SequenceNumber last_sequence,
                          uint64_t next_file_number) {
  Status s = env->CreateDir(dbname);
  if (!s.ok()) {
    return s;
  }
  BlockCache block_cache(size_t{1} << 20);
  TableCache table_cache(dbname, env, &block_cache, 20);
  VersionSet versions(dbname, env, &table_cache);
  s = versions.CreateNew();
  if (!s.ok()) {
    return s;
  }
  versions.SetLastSequence(last_sequence);
  if (next_file_number > 0) {
    versions.MarkFileNumberUsed(next_file_number - 1);
  }
  // Every record of the MANIFEST carries the counters, so an edit that changes nothing else is how
  // they get there. The sequence number is the tables' newest write, which the edit says, as the
  // flush that wrote them would have.
  VersionEdit edit;
  edit.last_sequence = last_sequence;
  return versions.LogAndApply(edit);
}

std::string EncodedBatch(SequenceNumber sequence, const Entries& puts) {
  WriteBatch batch;
  for (const auto& [key, value] : puts) {
    batch.Put(key, value);
  }
  WriteBatchInternal::SetSequence(&batch, sequence);
  return std::string(WriteBatchInternal::Contents(&batch));
}

Status WriteLogFile(Env* env, const std::string& fname, const std::vector<std::string>& records) {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  log::Writer writer(file.get());
  for (const std::string& record : records) {
    s = writer.AddRecord(record);
    if (!s.ok()) {
      break;
    }
  }
  if (s.ok()) {
    s = file->Sync();
  }
  const Status closed = file->Close();
  return s.ok() ? closed : s;
}

std::vector<uint64_t> LogRecordOffsets(Env* env, const std::string& fname) {
  std::vector<uint64_t> offsets;
  std::unique_ptr<SequentialFile> file;
  if (!env->NewSequentialFile(fname, &file).ok()) {
    ADD_FAILURE() << "cannot read " << fname;
    return offsets;
  }
  log::Reader reader(file.get(), nullptr, /*checksum=*/true);
  std::string_view record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    offsets.push_back(reader.LastRecordOffset());
  }
  return offsets;
}

std::string NewestLogFile(Env* env, const std::string& dbname) {
  std::vector<std::string> names;
  if (!env->GetChildren(dbname, &names).ok()) {
    return "";
  }
  uint64_t newest = 0;
  bool found = false;
  for (const std::string& name : names) {
    uint64_t number = 0;
    FileType type = FileType::kCurrentFile;
    if (ParseFileName(name, &number, &type) && type == FileType::kLogFile &&
        (!found || number > newest)) {
      newest = number;
      found = true;
    }
  }
  return found ? LogFileName(dbname, newest) : "";
}

}  // namespace shale
