// The work that happens beside the writes: switching to a new memtable when the one in use is full,
// writing the one that is waiting out as a level 0 table, deleting the files that nothing needs
// any more, and what happens when any of it fails.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "db/builder.h"
#include "db/db_impl.h"
#include "db/filename_io.h"
#include "db/version_set.h"
#include "io/log_writer.h"
#include "io/memtable.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/filename.h"
#include "util/mutex.h"

namespace shale {

namespace {

// Whether `mem` holds no entry.
bool IsEmpty(const MemTable& mem) {
  const std::unique_ptr<Iterator> iter = mem.NewIterator();
  iter->SeekToFirst();
  return !iter->Valid();
}

}  // namespace

void DBImpl::RecordBackgroundError(const Status& s) {
  if (bg_error_.ok()) {
    bg_error_ = s;
    counters_.bg_errors++;
    Log("write path stopped: " + s.ToString());
  }
  // Whoever waits for a flush that will never come is woken to see the error.
  background_cv_.SignalAll();
}

Status DBImpl::Flush() {
  MutexLock lock(&mutex_);
  if (!bg_error_.ok()) {
    return bg_error_;
  }
  if (!IsEmpty(*mem_)) {
    const Status s = MakeRoomForWrite(/*force=*/true);
    if (!s.ok()) {
      return s;
    }
  }
  // The memtable that is waiting now is the one this call has to see written: the one it just
  // switched, or, when there was nothing to switch, the one that an earlier switch left. A flush
  // that ends and another switch after it are not this call's to wait for, so the wait is for this
  // memtable to be gone and not for `imm_` to be empty.
  const std::shared_ptr<MemTable> target = imm_;
  while (target != nullptr && imm_ == target && bg_error_.ok()) {
    WaitForBackgroundWork();
  }
  return bg_error_;
}

void DBImpl::WaitForBackgroundWork() {
  if (scheduler_->RunsOnCallerThread()) {
    // Nobody else will run the tasks, so the waiter does. The mutex is given up for it: a task
    // takes it.
    mutex_.Unlock();
    const size_t ran = scheduler_->RunPending();
    mutex_.Lock();
    SHALE_CHECK(ran > 0, "waiting for background work but none is scheduled");
  } else {
    background_cv_.Wait();
  }
}

Status DBImpl::MakeRoomForWrite(bool force) {
  while (true) {
    if (!bg_error_.ok()) {
      return bg_error_;
    }
    if (!force && mem_->ApproximateMemoryUsage() < options_.write_buffer_size) {
      return Status::OK();
    }
    if (imm_ != nullptr) {
      // The memtable that was switched before is not yet a table, and there is no place to put
      // this one.
      WaitForBackgroundWork();
      continue;
    }
    return SwitchMemTable();
  }
}

Status DBImpl::SwitchMemTable() {
  SHALE_CHECK(imm_ == nullptr, "the memtable is switched while another is waiting to be flushed");
  // The old log is made durable before any write can go into the new one. Otherwise a later write
  // that is synced, in the new log, could survive a power loss while earlier ones that were not, in
  // the old, were lost: a hole in the sequence of writes, which only a suffix of it may be.
  Status s = logfile_->Sync();
  if (!s.ok()) {
    RecordBackgroundError(s);
    return s;
  }
  uint64_t number = 0;
  s = AllocateFileNumber(&number);
  if (!s.ok()) {
    RecordBackgroundError(s);
    return s;
  }
  const std::string fname = LogFileName(dbname_, number);
  std::unique_ptr<WritableFile> file;
  s = env_->NewWritableFile(fname, &file);
  if (s.ok()) {
    // The directory entry is durable before a write is acknowledged in the file.
    s = SyncDirectory(env_, dbname_);
    if (!s.ok()) {
      // The file is empty and has no use: it is closed and removed. A failure to remove it leaves
      // an empty log for the next open to replay and delete.
      static_cast<void>(file->Close());
      static_cast<void>(env_->RemoveFile(fname));
    }
  }
  if (!s.ok()) {
    // Like any failure of the write path, this stops writes (D5). The old log is whole and still
    // the one in use, so a retry might go through, but a database that cannot make its next log is
    // not one to trust with the next write: it would be acknowledged into a file that has no room
    // to go on.
    RecordBackgroundError(s);
    return s;
  }

  log_.reset();
  // Every byte of it is durable, so a failure to close it has nothing to lose.
  static_cast<void>(logfile_->Close());
  logfile_ = std::move(file);
  log_ = std::make_unique<log::Writer>(logfile_.get());
  logfile_number_ = number;

  imm_ = std::move(mem_);
  has_imm_.store(true, std::memory_order_release);
  mem_ = NewMemTable();
  MaybeScheduleBackgroundWork();
  return Status::OK();
}

void DBImpl::MaybeScheduleBackgroundWork() {
  if (bg_scheduled_ || shutting_down_.load(std::memory_order_acquire) || !bg_error_.ok()) {
    return;
  }
  if (imm_ == nullptr) {
    return;
  }
  bg_scheduled_ = true;
  scheduler_->Schedule([this] { BackgroundCall(); });
}

void DBImpl::BackgroundCall() {
  MutexLock lock(&mutex_);
  SHALE_CHECK(bg_scheduled_, "a background task ran that was not scheduled");
  if (!shutting_down_.load(std::memory_order_acquire) && bg_error_.ok() && imm_ != nullptr) {
    const Status s = FlushImmutable();
    if (!s.ok()) {
      RecordBackgroundError(s);
    }
  }
  bg_scheduled_ = false;
  // What this task did, or what happened while it ran, may have made more work.
  MaybeScheduleBackgroundWork();
  background_cv_.SignalAll();
}

Status DBImpl::FlushImmutable() {
  // Only this task clears `imm_`, and a switch waits for it to be clear, so it is the same
  // memtable from here to the end, and no write goes into it.
  const std::shared_ptr<MemTable> imm = imm_;
  FileMetaData meta;
  Status s = AllocateFileNumber(&meta.number);
  if (!s.ok()) {
    return s;
  }
  pending_outputs_.insert(meta.number);

  mutex_.Unlock();
  const std::unique_ptr<Iterator> iter = imm->NewIterator();
  s = BuildTable(dbname_, env_, TableOptions(), &table_cache_, iter.get(), &meta);
  mutex_.Lock();

  if (s.ok()) {
    VersionEdit edit;
    // The logs before the one in use are for the memtable that this table holds, and for the ones
    // before it, which are tables already: from the MANIFEST that names this edit on, they are not
    // replayed.
    edit.log_number = logfile_number_;
    if (meta.file_size > 0) {
      edit.new_files.emplace_back(0, meta);
    }
    // If this fails, the MANIFEST may hold the edit all the same, and then the table is one that a
    // restart finds in the version. Nothing is deleted after an error for that reason.
    s = versions_.LogAndApply(std::move(edit));
  }
  pending_outputs_.erase(meta.number);
  if (!s.ok()) {
    return s;
  }

  imm_.reset();
  has_imm_.store(false, std::memory_order_release);
  if (meta.file_size > 0) {
    counters_.flushes++;
    counters_.flush_bytes_written += meta.file_size;
    Log("flushed a memtable to table " + std::to_string(meta.number) + ", " +
        std::to_string(meta.file_size) + " byte(s)");
  }
  DeleteObsoleteFiles();
  return Status::OK();
}

void DBImpl::DeleteObsoleteFiles() {
  if (!bg_error_.ok()) {
    return;
  }
  // What decides which files are obsolete is read here, with the mutex held.
  std::set<uint64_t> needed = versions_.LiveFiles();
  needed.insert(pending_outputs_.begin(), pending_outputs_.end());
  const uint64_t log_number = versions_.LogNumber();
  const uint64_t current_log = logfile_number_;
  const uint64_t manifest_number = versions_.ManifestFileNumber();

  mutex_.Unlock();
  std::vector<std::string> names;
  if (env_->GetChildren(dbname_, &names).ok()) {
    for (const std::string& name : names) {
      uint64_t number = 0;
      FileType type = FileType::kCurrentFile;
      if (!ParseFileName(name, &number, &type)) {
        continue;  // not a file of the database's
      }
      if (number >= kMaxFileNumber) {
        continue;  // the database never numbers a file so high: not its own
      }
      bool obsolete = false;
      switch (type) {
        case FileType::kLogFile:
          obsolete = number < log_number && number != current_log;
          break;
        case FileType::kTableFile:
          obsolete = needed.count(number) == 0;
          break;
        case FileType::kDescriptorFile:
          obsolete = number != manifest_number;
          break;
        case FileType::kTempFile:
          obsolete = true;
          break;
        case FileType::kCurrentFile:
        case FileType::kLockFile:
        case FileType::kInfoLogFile:
          break;
      }
      if (!obsolete) {
        continue;
      }
      if (type == FileType::kTableFile) {
        // The cache does not know that a file is gone, and would go on serving the table.
        table_cache_.Evict(number);
      }
      const Status s = env_->RemoveFile(dbname_ + "/" + name);
      if (s.ok()) {
        Log("deleted " + name);
      } else {
        Log("could not delete " + name + ": " + s.ToString());  // it is tried again later
      }
    }
  }
  mutex_.Lock();
}

}  // namespace shale
