// The work that happens beside the writes: switching to a new memtable when the one in use is full,
// writing the one that is waiting out as a level 0 table, compacting the levels, deleting the files
// that nothing needs any more, and what happens when any of it fails.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/compaction_picker.h"
#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/builder.h"
#include "db/compaction_job.h"
#include "db/db_impl.h"
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
  // The switch is the leader's: a group is being appended to the log and added to `mem_` with the
  // mutex given up, and a switch under it would replace both. So this takes its place in the queue
  // as a request with no batch, which no group takes in, and switches when it has the front.
  Writer w(&mutex_);
  writers_.push_back(&w);
  while (&w != writers_.front()) {
    w.cv.Wait();
  }
  Status s;
  if (!IsEmpty(*mem_)) {
    s = MakeRoomForWrite(/*force=*/true);
  }
  // The memtable that is waiting now is the one this call has to see written: the one it just
  // switched, or, when there was nothing to switch, the one that an earlier switch left. A flush
  // that ends and another switch after it are not this call's to wait for, so the wait is for this
  // memtable to be gone and not for `imm_` to be empty.
  const std::shared_ptr<MemTable> target = imm_;
  // The place in the queue is given up before the wait: the writes behind it need not wait for a
  // flush that they do not depend on, and the memtable they fill is a new one.
  FinishGroup(&w, &w, Status::OK());
  if (!s.ok()) {
    return s;
  }
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
  // A request to switch is not a write, and is not slowed.
  bool allow_delay = !force;
  while (true) {
    if (!bg_error_.ok()) {
      return bg_error_;
    }
    const int level0_files = versions_.current()->NumFiles(0);
    if (allow_delay && level0_files >= options_.l0_slowdown_trigger) {
      // Level 0 fills faster than compaction empties it. The write gives it a millisecond, once,
      // with the mutex given up: the writers that queue meanwhile are in the group that follows.
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      mutex_.Lock();
      counters_.write_slowdowns++;
      allow_delay = false;
      continue;  // what was read above may have changed while the mutex was not held
    }
    if (!force && mem_->ApproximateMemoryUsage() < options_.write_buffer_size) {
      return Status::OK();
    }
    if (imm_ != nullptr) {
      // The memtable that was switched before is not yet a table, and there is no place to put
      // this one.
      counters_.write_stops++;
      WaitForBackgroundWork();
      continue;
    }
    if (level0_files >= options_.l0_stop_trigger) {
      // A switch is what adds a file to level 0, once the flush of the memtable it makes is done,
      // and it waits here for a compaction to take some away. This is what bounds level 0.
      counters_.write_stops++;
      WaitForBackgroundWork();
      continue;
    }
    return SwitchMemTable();
  }
}

Status DBImpl::SwitchMemTable() {
  SHALE_CHECK(imm_ == nullptr, "the memtable is switched while another is waiting to be flushed");
  // Nothing is synced here, and the writers do not wait for a sync of the old log or the directory
  // at a switch. What that would buy, and how it is had without it:
  //
  // 1. A write acknowledged as synced survives a power loss with every write acknowledged before
  //    it (D1, D2). The writes before the switch that were not synced are in the old log, and the
  //    new log's directory entry is not durable: CommitGroup() syncs the old log, which this keeps
  //    open in `unsynced_log_`, and the directory, before it syncs the new log and acknowledges a
  //    synced group. The flush of `imm_`, which holds the old log's writes, makes both syncs
  //    unnecessary: its table is durable before its edit is, and it syncs the directory after this
  //    made the new log.
  // 2. A power loss leaves a prefix of the writes, never a hole (D2). Without the sync, it can
  //    take the end of the old log, writes that were not synced, and keep writes of the new log
  //    that were not synced either. Every write is numbered one after the last, so recovery sees
  //    the hole at the new log's first write and replays nothing from there on (ReplayLog()); the
  //    tables' newest write, which the flush records, is what the first log replayed is held to.
  //
  // The simulator's I4 checks both after every crash it makes: a reopened database must equal the
  // writes up to some point at or after the last one acknowledged as synced.
  SHALE_CHECK(unsynced_log_ == nullptr,
              "a log from before the last switch is still waiting for a sync at the next switch");
  uint64_t number = 0;
  Status s = AllocateFileNumber(&number);
  if (!s.ok()) {
    RecordBackgroundError(s);
    return s;
  }
  std::unique_ptr<WritableFile> file;
  s = env_->NewWritableFile(LogFileName(dbname_, number), &file);
  if (!s.ok()) {
    // Like any failure of the write path, this stops writes (D5). The old log is whole and still
    // the one in use, so a retry might go through, but a database that cannot make its next log is
    // not one to trust with the next write: it would be acknowledged into a file that has no room
    // to go on.
    RecordBackgroundError(s);
    return s;
  }

  log_.reset();
  if (log_unsynced_) {
    unsynced_log_ = std::move(logfile_);
  } else {
    // Every byte of it is durable, so a failure to close it has nothing to lose.
    static_cast<void>(logfile_->Close());
  }
  logfile_ = std::move(file);
  log_ = std::make_unique<log::Writer>(logfile_.get());
  logfile_number_ = number;
  log_unsynced_ = false;  // the new log is empty
  log_dir_unsynced_ = true;

  // The leader switches before it numbers its group, so every write numbered up to now is in the
  // memtable that becomes `imm_`, and none after it is.
  imm_last_sequence_ = versions_.LastSequence();
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
  // Only now, with no compaction running, is the version one that the picker may be asked about.
  if (imm_ == nullptr && manual_ == nullptr && !PickAutomaticCompaction().has_value()) {
    return;
  }
  bg_scheduled_ = true;
  scheduler_->Schedule([this] { BackgroundCall(); });
}

void DBImpl::BackgroundCall() {
  MutexLock lock(&mutex_);
  SHALE_CHECK(bg_scheduled_, "a background task ran that was not scheduled");
  if (!shutting_down_.load(std::memory_order_acquire) && bg_error_.ok()) {
    // The most urgent first: the memtable that a write may be stalled for, and then a compaction,
    // which a caller's request comes before.
    const Status s = imm_ != nullptr ? FlushImmutable() : BackgroundCompaction();
    if (!s.ok()) {
      RecordBackgroundError(s);
    }
  }
  bg_scheduled_ = false;
  // What this task did, or what happened while it ran, may have made more work.
  MaybeScheduleBackgroundWork();
  background_cv_.SignalAll();
}

CompactionOptions DBImpl::PickerOptions() const {
  CompactionOptions picker_options;
  // Never above the number of files at which writes stop: a write that waits for level 0 to shrink
  // must have a compaction coming, or it would wait for ever.
  picker_options.l0_compaction_trigger =
      std::min(options_.l0_compaction_trigger, options_.l0_stop_trigger);
  picker_options.max_bytes_for_level_base = options_.max_bytes_for_level_base;
  picker_options.level_size_multiplier = options_.level_size_multiplier;
  picker_options.target_file_size = options_.target_file_size;
  return picker_options;
}

std::optional<CompactionPlan> DBImpl::PickAutomaticCompaction() {
  SHALE_CHECK(!compaction_running_,
              "the picker is asked for a compaction while one is running: it would choose the "
              "files that the running one is reading");
  return PickCompaction(versions_.current()->state(), PickerOptions());
}

Status DBImpl::BackgroundCompaction() {
  ManualCompaction* const manual = manual_;
  std::shared_ptr<const Version> base = versions_.current();
  std::optional<CompactionPlan> plan;
  if (manual != nullptr) {
    manual->running = true;
    const auto view = [](const std::optional<std::string>& key) -> std::optional<std::string_view> {
      return key.has_value() ? std::optional<std::string_view>(*key) : std::nullopt;
    };
    plan = PickRangeCompaction(base->state(), manual->level, view(manual->begin), view(manual->end),
                               PickerOptions());
  } else {
    plan = PickAutomaticCompaction();
  }
  Status s;
  if (plan.has_value()) {
    s = RunCompaction(*plan, std::move(base));
  }
  if (manual != nullptr) {
    // The caller may leave from here on: nothing below uses the request.
    manual->running = false;
    manual->done = true;
    manual_ = nullptr;
  }
  return s;
}

Status DBImpl::RunCompaction(const CompactionPlan& plan, std::shared_ptr<const Version> base) {
  if (plan.IsTrivialMove(PickerOptions())) {
    base.reset();
    return MoveFileDown(plan);
  }
  return CompactFiles(plan, std::move(base));
}

Status DBImpl::MoveFileDown(const CompactionPlan& plan) {
  const FileMetaData& file = plan.inputs[0].front();
  VersionEdit edit;
  edit.deleted_files.emplace(plan.level, file.number);
  edit.new_files.emplace_back(plan.level + 1, file);
  edit.compact_pointers.emplace_back(plan.level, plan.next_compact_pointer);
  // If this fails, the MANIFEST may hold the edit all the same: the file is the same one either
  // way, and nothing is deleted. The mutex is given up while the edit is written.
  const Status s = versions_.LogAndApply(std::move(edit), &mutex_);
  if (s.ok()) {
    counters_.trivial_moves++;
    Log("moved table " + std::to_string(file.number) + " from level " + std::to_string(plan.level) +
        " to level " + std::to_string(plan.level + 1));
  }
  return s;
}

Status DBImpl::CompactFiles(const CompactionPlan& plan, std::shared_ptr<const Version> base) {
  // The numbers the job takes, which are the files it makes and must be told apart from the tables
  // of every version until an edit names them. The job asks on this thread, with the mutex given
  // up, so the vector is this thread's.
  std::vector<uint64_t> allocated;
  CompactionJobContext ctx;
  ctx.dbname = dbname_;
  ctx.env = env_;
  ctx.table_options = TableOptions();
  ctx.table_cache = &table_cache_;
  ctx.target_file_size = options_.target_file_size;
  ctx.allocate_file_number = [this, &allocated] {
    MutexLock lock(&mutex_);
    uint64_t number = 0;
    // The job refuses a number at the ceiling, so the status is the job's to find out.
    static_cast<void>(AllocateTableNumber(&number));
    pending_outputs_.insert(number);
    allocated.push_back(number);
    return number;
  };
  ctx.between_outputs = [this] { FlushBetweenOutputs(); };
  // Asked before each output: closing does not wait for the rest of a compaction.
  ctx.should_stop = [this] { return shutting_down_.load(std::memory_order_acquire); };

  // The job is the one thing that holds the version it reads, so that the files it reads are not
  // deleted under it, and it is gone before the files it replaces are swept.
  auto job = std::make_unique<CompactionJob>(std::move(ctx), plan, std::move(base),
                                             snapshots_.Sequences());
  compaction_running_ = true;
  mutex_.Unlock();
  Status s = job->Run();
  mutex_.Lock();
  compaction_running_ = false;

  const bool stopped = s.ok() && job->stopped();
  if (s.ok() && !stopped && !bg_error_.ok()) {
    // A flush between two outputs failed, or a write did: the database has stopped. Nothing names
    // the outputs, so they are no use to anyone, and nothing more is installed.
    job->RemoveOutputs();
    s = bg_error_;
  }
  if (s.ok() && !stopped) {
    // If this fails, the MANIFEST may hold the edit all the same, and the outputs are then tables
    // that the next open finds in the version: they stay, and nothing is deleted. The mutex is
    // given up while the edit is written; the outputs are in `pending_outputs_` until after, and
    // the inputs in the current version, which only this task replaces.
    s = versions_.LogAndApply(job->MakeEdit(), &mutex_);
    if (s.ok()) {
      // A write that waits for level 0 to shrink can go on now, and not when the sweep below is
      // done.
      background_cv_.SignalAll();
    }
  }
  for (const uint64_t number : allocated) {
    pending_outputs_.erase(number);
  }
  if (!s.ok()) {
    return s;
  }
  if (stopped) {
    // The database is closing. The job removed what it made, no edit names it, and the inputs are
    // where they were: the next open finds the same version, and compacts again if it is due.
    Log("stopped a compaction of level " + std::to_string(plan.level) +
        ": the database is closing");
    return Status::OK();
  }

  const CompactionJobStats& stats = job->stats();
  counters_.compactions++;
  counters_.compaction_bytes_read += stats.bytes_read;
  counters_.compaction_bytes_written += stats.bytes_written;
  counters_.tombstones_dropped += stats.tombstones_dropped;
  counters_.shadowed_dropped += stats.shadowed_dropped;
  Log("compacted " + std::to_string(plan.inputs[0].size()) + " table(s) of level " +
      std::to_string(plan.level) + " and " + std::to_string(plan.inputs[1].size()) + " of level " +
      std::to_string(plan.level + 1) + " into " + std::to_string(job->outputs().size()) +
      " table(s) of level " + std::to_string(plan.level + 1) + ": " +
      std::to_string(stats.bytes_read) + " byte(s) read, " + std::to_string(stats.bytes_written) +
      " written, " + std::to_string(stats.shadowed_dropped) + " shadowed and " +
      std::to_string(stats.tombstones_dropped) + " deletion(s) dropped");
  // The inputs are obsolete now, and are deleted unless a reader still holds a version that names
  // them, which the job did until now.
  job.reset();
  DeleteObsoleteFiles();
  return Status::OK();
}

void DBImpl::FlushBetweenOutputs() {
  if (!has_imm_.load(std::memory_order_acquire)) {
    return;
  }
  MutexLock lock(&mutex_);
  if (imm_ == nullptr || !bg_error_.ok() || shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  const Status s = FlushImmutable();
  if (!s.ok()) {
    RecordBackgroundError(s);
  }
}

Status DBImpl::CompactRange(std::optional<std::string_view> begin,
                            std::optional<std::string_view> end) {
  Status s = Flush();
  if (!s.ok()) {
    return s;
  }
  MutexLock lock(&mutex_);
  // Level 0 is always compacted: its files are what the flush just made, and they are to reach
  // level 1 whether or not anything is below. Then each level down to the one before the deepest
  // that holds part of the range, so that all of it ends in that deepest level.
  int deepest = 1;
  {
    const std::shared_ptr<const Version> base = versions_.current();
    for (int level = 1; level < kNumLevels; level++) {
      if (!OverlappingFiles(base->state(), level, begin, end).empty()) {
        deepest = level;
      }
    }
  }
  for (int level = 0; level < deepest; level++) {
    s = CompactLevel(level, begin, end);
    if (!s.ok()) {
      return s;
    }
  }
  return bg_error_;
}

Status DBImpl::CompactLevel(int level, std::optional<std::string_view> begin,
                            std::optional<std::string_view> end) {
  ManualCompaction request;
  request.level = level;
  if (begin.has_value()) {
    request.begin = std::string(*begin);
  }
  if (end.has_value()) {
    request.end = std::string(*end);
  }
  // One request at a time. A caller that comes while another's is there waits for it to be done.
  while (manual_ != nullptr && bg_error_.ok()) {
    WaitForBackgroundWork();
  }
  if (!bg_error_.ok()) {
    return bg_error_;
  }
  manual_ = &request;
  MaybeScheduleBackgroundWork();
  // The task that has taken the request is using it, so a caller leaves only when it is done, even
  // when an error has already stopped the database: the error is returned all the same.
  while (!request.done && (bg_error_.ok() || request.running)) {
    WaitForBackgroundWork();
  }
  if (manual_ == &request) {
    manual_ = nullptr;  // an error stopped the database before the task took it
  }
  return bg_error_;
}

Status DBImpl::FlushImmutable() {
  // Only this task clears `imm_`, and a switch waits for it to be clear, so it is the same
  // memtable from here to the end, and no write goes into it.
  const std::shared_ptr<MemTable> imm = imm_;
  FileMetaData meta;
  Status s = AllocateTableNumber(&meta.number);
  if (!s.ok()) {
    return s;
  }
  pending_outputs_.insert(meta.number);

  mutex_.Unlock();
  const std::unique_ptr<Iterator> iter = imm->NewIterator();
  s = BuildTable(dbname_, env_, TableOptions(), &table_cache_, iter.get(), &meta);
  mutex_.Lock();
  SHALE_CHECK(imm_ == imm,
              "the memtable that was being flushed was replaced while its table was built");

  if (s.ok()) {
    VersionEdit edit;
    // The logs before the one in use are for the memtable that this table holds, and for the ones
    // before it, which are tables already: from the MANIFEST that names this edit on, they are not
    // replayed. The writes up to the newest in the memtable are in tables from then on, and the
    // log in use goes on from the next.
    edit.log_number = logfile_number_;
    edit.last_sequence = imm_last_sequence_;
    if (meta.file_size > 0) {
      edit.new_files.emplace_back(0, meta);
    }
    // If this fails, the MANIFEST may hold the edit all the same, and then the table is one that a
    // restart finds in the version. Nothing is deleted after an error for that reason. The mutex
    // is given up while the edit is written, with the table in `pending_outputs_` until after; a
    // switch cannot replace the log the edit names, since `imm_` is still set.
    s = versions_.LogAndApply(std::move(edit), &mutex_);
  }
  pending_outputs_.erase(meta.number);
  if (!s.ok()) {
    return s;
  }

  // The old log's writes are in a durable table now, so the sync that a synced write would have
  // made of it is no longer needed. And the directory was synced after the table was written,
  // which is after the switch that made the log in use: no switch happens while `imm_` is set.
  if (unsynced_log_ != nullptr) {
    static_cast<void>(unsynced_log_->Close());  // what it holds is in the table
    unsynced_log_.reset();
  }
  if (meta.file_size > 0) {
    log_dir_unsynced_ = false;  // an empty memtable makes no table, and syncs no directory
  }
  imm_.reset();
  has_imm_.store(false, std::memory_order_release);
  // A write that waits for `imm_` to go can go on now, and not when the sweep below is done: it
  // lists and deletes files, with the mutex given up and the disk to wait for.
  background_cv_.SignalAll();
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
  // The tables an edit adds are in no version until it is installed, and a sweep while it is being
  // written could take them. Only the background task writes edits, and only it sweeps, so the two
  // never meet; this is where they would.
  SHALE_CHECK(!versions_.ManifestWriteInProgress(),
              "the obsolete files are swept while an edit is being written to the MANIFEST");
  if (!bg_error_.ok()) {
    return;
  }
  // What decides which files are obsolete is read here, with the mutex held.
#if defined(SHALE_FLAW_DELETE_LIVE_FILES)
  // Flaw delete_live_files: only the current version's files are kept, not those that readers hold.
  std::set<uint64_t> needed;
  for (const std::vector<FileMetaData>& level : versions_.current()->state().files) {
    for (const FileMetaData& file : level) {
      needed.insert(file.number);
    }
  }
#else
  std::set<uint64_t> needed = versions_.LiveFiles();
#endif
  needed.insert(pending_outputs_.begin(), pending_outputs_.end());
  const uint64_t makers = table_makers_;
  const uint64_t log_number = versions_.LogNumber();
  const uint64_t current_log = logfile_number_;
  const uint64_t manifest_number = versions_.ManifestFileNumber();

  mutex_.Unlock();
  std::vector<std::string> names;
  const Status listed = env_->GetChildren(dbname_, &names);
  if (!listed.ok()) {
    Log("could not list " + dbname_ + " to delete obsolete files: " + listed.ToString());
  } else {
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
  // A table begun while the files were being listed is one that `needed` does not have, and that
  // the loop above would have taken, and it is no use that it is done by now: the count shows it
  // where the set of numbers in progress would not. Only the task that runs this makes tables, and
  // it was here.
  SHALE_CHECK(table_makers_ == makers, "a table was begun while the obsolete files were swept");
}

}  // namespace shale
