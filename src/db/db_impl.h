#ifndef SHALE_DB_DB_IMPL_H_
#define SHALE_DB_DB_IMPL_H_

// The database: opening it and recovering what a previous run left, the write path, and point
// reads. The other files of src/db that belong to DBImpl are db_write.cc, the write path,
// db_background.cc, the memtable switch and the background work, and, as they come, the iterators.

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/compaction_picker.h"
#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/info_log.h"
#include "db/snapshot_list.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "io/log_writer.h"
#include "io/memtable.h"
#include "io/table.h"
#include "io/table_builder.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "util/dbformat.h"
#include "util/mutex.h"
#include "util/thread_annotations.h"

namespace shale {

/// What the database has done, counted, for tests to look at and for the properties that report it.
struct DBCounters {
  uint64_t flushes = 0;                   ///< Memtables written out as level 0 tables.
  uint64_t compactions = 0;               ///< Compactions that merged files.
  uint64_t trivial_moves = 0;             ///< Files moved to the next level as they were.
  uint64_t tombstones_dropped = 0;        ///< Deletion entries that a compaction dropped.
  uint64_t shadowed_dropped = 0;          ///< Entries a compaction dropped for a newer one.
  uint64_t user_bytes_written = 0;        ///< Key and value bytes the callers wrote.
  uint64_t wal_bytes_written = 0;         ///< Bytes appended to the write-ahead log.
  uint64_t flush_bytes_written = 0;       ///< Bytes of the tables that flushes wrote.
  uint64_t compaction_bytes_read = 0;     ///< Bytes of the tables that compactions read.
  uint64_t compaction_bytes_written = 0;  ///< Bytes of the tables that compactions wrote.
  uint64_t write_groups = 0;              ///< Groups of writes that shared one log record.
  uint64_t wal_syncs = 0;                 ///< Syncs of the write-ahead log.
  uint64_t write_slowdowns = 0;           ///< Writes that were slowed for level 0 being full.
  uint64_t write_stops = 0;               ///< Waits of a write for level 0 or the flush.
  uint64_t bg_errors = 0;                 ///< Errors that stopped writes.
};

/// The implementation of DB.
///
/// Everything the database changes is guarded by `mutex_`: the memtables and the log the newer one
/// is written to, the version set, the snapshots and the first error of the log. A write holds it
/// from the sequence number to the memtable, which is what makes one writer at a time, and a read
/// holds it only to take the memtables, the version and the sequence number it will read.
///
/// Background work is one task at a time on the Scheduler: a full memtable becomes the immutable
/// one, `imm_`, and a task writes it out as a level 0 table while writes go on into a new `mem_`.
/// What a task does when it runs is the most urgent of what there is: the flush of `imm_`, which
/// stalls writes, then the compaction that a CompactRange() is waiting for, then the compaction
/// that the size of a level asks for. A compaction is the task for as long as it takes, which is
/// why it writes out `imm_` itself between two of its output files, and why only one is ever
/// running: the picker cannot see a compaction that is in flight, and would choose the same files
/// again. `bg_scheduled_` says a task is queued or running, and it takes the mutex, which it gives
/// up only to write files. Whoever must wait for background work, a write that finds the memtable
/// full and `imm_` still there among them, does it in WaitForBackgroundWork(): on a scheduler that
/// runs tasks on the caller's thread, it runs them itself.
class DBImpl final : public DB {
 public:
  /// Creates a database that is not open: DB::Open() calls OpenImpl() on it. `options` has been
  /// through ValidateOptions().
  /// @param options how to run the database.
  /// @param dbname the database directory.
  DBImpl(const Options& options, std::string dbname);

  /// Closes the database; see DB::~DB(). Aborts if a snapshot is still alive.
  ~DBImpl() override;

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value) override;
  Status Delete(const WriteOptions& options, std::string_view key) override;
  Status Write(const WriteOptions& options, WriteBatch* updates) override;
  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override;

  /// Switches to a new memtable, if the one in use is not empty, and returns when the memtable
  /// that was in use, and any that was waiting before it, is a table. Returns the error that stops
  /// writes, if there is one, and then the memtable may not have been written.
  Status Flush() override;

  /// Compacts the range as DB::CompactRange() says: writes out the memtable, then, level by level
  /// from 0 down to the level just above the deepest one that holds a file overlapping the range,
  /// compacts the files of the level that overlap it, with the ones they overlap in the level
  /// below, into that level below. Level 0 is always compacted, so that what the flush just wrote
  /// reaches level 1. Every such compaction rewrites its files, a lone file that could move down
  /// whole included, so that what they hold that is dead is given back. Each is a request that the
  /// background task takes before any compaction it would have chosen itself, and this waits for
  /// each in turn. Returns the error that stops writes, if there is one.
  Status CompactRange(std::optional<std::string_view> begin,
                      std::optional<std::string_view> end) override;

  // These return NotSupported, or nothing, until they are written.
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;
  bool GetProperty(std::string_view property, std::string* value) override;

  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* snapshot) override;

  /// Opens the database: the work of DB::Open() after the options are checked. The sequence is
  /// the directory, the lock, CURRENT and the MANIFEST, the replay of the write-ahead logs that
  /// hold writes the tables do not, a new log, the recovered writes as a level 0 table, a new
  /// MANIFEST that names both, and last the deletion of the files that these replace, which also
  /// takes the tables that an earlier open or a flush left that no version names. A failure
  /// anywhere leaves what was in the directory as it was, except for a new log, a new table or a
  /// new MANIFEST that may or may not have come to be named, none of which is deleted.
  Status OpenImpl() SHALE_EXCLUDES(mutex_);

  /// Returns the files of the current version, level by level, and its counters.
  VersionState TEST_VersionState() SHALE_EXCLUDES(mutex_);

  /// Returns what the database has counted so far.
  DBCounters TEST_Counters() SHALE_EXCLUDES(mutex_);

  /// Returns the sequence number of the newest write.
  SequenceNumber TEST_LastSequence() SHALE_EXCLUDES(mutex_);

  /// Returns the numbers of the files of every version that is still alive.
  std::set<uint64_t> TEST_LiveFiles() SHALE_EXCLUDES(mutex_);

  /// Returns the current version, which the caller then holds: a table that it names is not
  /// deleted for as long as the caller does, as for a read that began now.
  std::shared_ptr<const Version> TEST_CurrentVersion() SHALE_EXCLUDES(mutex_);

  /// Returns the cache of open tables, for a test to ask whether a table is in it.
  TableCache* TEST_TableCache() { return &table_cache_; }

 private:
  // The work of OpenImpl(), with the mutex held.
  Status OpenLocked() SHALE_REQUIRES(mutex_);

  // Finds the files that recovery cares about: marks the numbers of the logs and tables in the
  // directory as used, and returns the numbers of the logs to replay, in ascending order.
  Status FindLogsToReplay(std::vector<uint64_t>* logs) SHALE_REQUIRES(mutex_);

  // Replays log `number` into `mem_`. `*max_sequence` is the last sequence number applied so far
  // and moves on with each batch; `*entries` counts the operations applied. Returns the IOError of
  // a read that failed, whether or not paranoid_checks is on: the log may be whole. Sets
  // `*stopped_for` to what was wrong when the log has a defect that is not an error because
  // paranoid_checks is off: replay ends there, with nothing after it in this log or the later ones
  // applied, and the caller sets those logs aside.
  Status ReplayLog(uint64_t number, SequenceNumber* max_sequence, uint64_t* entries,
                   std::string* stopped_for) SHALE_REQUIRES(mutex_);

  // Applies the batch that is `record` to `mem_`, whole or not at all: returns Corruption, with
  // nothing applied, for a batch that does not decode, whose sequence number does not come after
  // `*max_sequence`, or whose operations would be numbered above kMaxSequenceNumber.
  Status ApplyRecoveredBatch(std::string_view record, SequenceNumber* max_sequence,
                             uint64_t* entries) SHALE_REQUIRES(mutex_);

  // Renames logs[first] and every log after it to "<name>.dropped", the last first, so that a
  // failure part of the way never leaves a later log replayable behind one that is not, and says
  // for each in the info log why. `why` is what was wrong with logs[first]. Syncs the directory
  // after the later logs are renamed and before logs[first] is, so that the order holds across a
  // power loss on a filesystem that does not persist directory changes in order, and again at the
  // end. The renamed files have a name that nothing reads or deletes.
  Status SetAsideLogs(const std::vector<uint64_t>& logs, size_t first, const std::string& why)
      SHALE_REQUIRES(mutex_);

  // Takes a number for a log or a table. Returns Corruption instead when none is left below
  // kMaxFileNumber: a file numbered at it is one that no MANIFEST holds, and a log numbered so high
  // would be taken for a stray file by the next open and never replayed.
  Status AllocateFileNumber(uint64_t* number) SHALE_REQUIRES(mutex_);

  // Writes `mem_`, the recovered writes, as a level 0 table and adds it to `*edit`.
  Status WriteRecoveredMemTable(VersionEdit* edit) SHALE_REQUIRES(mutex_);

  // Remembers `s` as the first error of the write path, if there is none yet: every later write
  // returns it, no more background work is scheduled, and whoever waits for background work is
  // woken to see it.
  void RecordBackgroundError(const Status& s) SHALE_REQUIRES(mutex_);

  // Makes room in `mem_` for a write, and returns OK when there is, or the error that stops
  // writes. There is room when `mem_` is below write_buffer_size, and when it is not, or when
  // `force` is set, `mem_` becomes `imm_` and a new one takes its place, which waits first for an
  // `imm_` that is still there. May give up the mutex while it waits, so a caller that read
  // anything under the mutex before it must read it again.
  Status MakeRoomForWrite(bool force) SHALE_REQUIRES(mutex_);

  // Starts a new log and a new `mem_`, and makes the old `mem_` the `imm_`, which must be null.
  // The old log is synced first and the new one's directory entry before any write goes into it, so
  // that a write that is durable in the new log never has an earlier one that is not in the old.
  // Any failure is a background error: the state of the log is not known after it.
  Status SwitchMemTable() SHALE_REQUIRES(mutex_);

  // Queues a background task if there is work and none is queued or running: the work is a memtable
  // to flush, a request of CompactRange() or a level that is due for compaction, and there is none
  // once the database is closing or an error has stopped writes.
  void MaybeScheduleBackgroundWork() SHALE_REQUIRES(mutex_);

  // The task that the scheduler runs. Takes the mutex.
  void BackgroundCall() SHALE_EXCLUDES(mutex_);

  // The size limits the picker chooses by, from the options.
  CompactionOptions PickerOptions() const;

  // Returns the compaction that the sizes of the levels ask for, or nothing. Aborts if a compaction
  // is running: the picker reads the version, which does not show the files a running compaction
  // is reading, and would choose them again.
  std::optional<CompactionPlan> PickAutomaticCompaction() SHALE_REQUIRES(mutex_);

  // What a background task does when there is no memtable to flush: the compaction that a request
  // of CompactRange() is waiting for if there is one, else the one that a level is due for, if
  // any. Returns OK if there was nothing to do. On an error, whatever the compaction left is left:
  // see CompactFiles().
  Status BackgroundCompaction() SHALE_REQUIRES(mutex_);

  // Runs `plan`, which was picked from `base`: moves its file to the next level if it is a lone
  // one that can go as it is, and else merges the files into new ones. Gives up the mutex while it
  // merges.
  Status RunCompaction(const CompactionPlan& plan, std::shared_ptr<const Version> base)
      SHALE_REQUIRES(mutex_);

  // Installs the move of the one file of `plan` to the next level, which reads nothing, so that
  // none of it goes through the filter: it is the one edit that deletes the file at its level and
  // adds it at the next.
  Status MoveFileDown(const CompactionPlan& plan) SHALE_REQUIRES(mutex_);

  // Merges the files of `plan` with a CompactionJob and installs the result, with the mutex given
  // up while the job runs. A failed job has removed its outputs. A failure to install the edit
  // leaves them, since the MANIFEST may hold the edit all the same and name them: they are left for
  // the next open to find, with the inputs that the edit may or may not have deleted. After a
  // background error the outputs of a job that succeeded are removed without being installed, for
  // the work of the database is over.
  Status CompactFiles(const CompactionPlan& plan, std::shared_ptr<const Version> base)
      SHALE_REQUIRES(mutex_);

  // The hook of a compaction between two of its output files: writes `imm_` out, if there is one,
  // so that a write that waits for it does not wait for the end of the compaction. Takes the mutex.
  // A failure is a background error, which the compaction finds when it is done.
  void FlushBetweenOutputs() SHALE_EXCLUDES(mutex_);

  // Compacts the files of `level` that overlap the range, as a request that the background task
  // takes, and waits for it. Returns the error that stops writes, if there is one.
  Status CompactLevel(int level, std::optional<std::string_view> begin,
                      std::optional<std::string_view> end) SHALE_REQUIRES(mutex_);

  // Writes `imm_` as a level 0 table and installs it, with the mutex given up while the table is
  // built. Returns the error of whichever step failed; then `imm_` is still there, a table that the
  // MANIFEST may or may not name may be left in the directory, and nothing has been deleted.
  Status FlushImmutable() SHALE_REQUIRES(mutex_);

  // Deletes the files that nothing needs: the tables that no version, in use by a reader or not,
  // names and no flush or compaction is producing, the logs older than `LogNumber()`, the MANIFESTs
  // but the current one, and temporary files. A file whose name is not one of the database's, a
  // log that a lenient open set aside among them, is not touched, nor is a numbered file at or
  // above kMaxFileNumber, which the database never makes. A table that is deleted is evicted from
  // the table cache. Does nothing after an error has stopped writes, since whether the version
  // that a failed edit would have installed is the one that a restart finds is not known. Gives up
  // the mutex while it lists and deletes: what it decides is obsolete never becomes needed, since a
  // file number is not used twice, and the one task at a time that makes files is the caller, a
  // compaction that flushes between its outputs included. A table being made while the files are
  // listed would be taken for one that nothing needs, so the sweep aborts if `pending_outputs_`
  // changed while it was away.
  void DeleteObsoleteFiles() SHALE_REQUIRES(mutex_);

  // Waits for the background work to make progress, and returns so that the caller can look at
  // what it is waiting for again. On a scheduler that runs tasks on the caller's thread it runs
  // them: the mutex is given up while they do, and it is a bug to be here with nothing queued,
  // since no one else will ever run a task. On any other it waits for the end of a task.
  void WaitForBackgroundWork() SHALE_REQUIRES(mutex_);

  // What a caller of CompactRange() asks the background task for, for one level: a request that
  // lives on the caller's stack from the time it is made until the caller has seen it done.
  struct ManualCompaction {
    int level = 0;                     // the level whose files are compacted
    std::optional<std::string> begin;  // the range of user keys, inclusive, or unbounded
    std::optional<std::string> end;
    bool running = false;  // the task has taken it, and the caller must not leave until it is done
    bool done = false;     // the task is finished with it, however it ended
  };

  std::shared_ptr<MemTable> NewMemTable() SHALE_REQUIRES(mutex_);

  TableBuilderOptions TableOptions() const;

  // Adds a line to the info log, if there is one yet.
  void Log(const std::string& line);

  // The lock on the database directory comes first so that it is released last, after every file
  // the database holds open has been closed.
  std::unique_ptr<FileLock> db_lock_;

  const Options options_;  // with `env` filled in
  Env* const env_;
  const std::string dbname_;
  std::unique_ptr<InfoLog> info_log_;  // null until the lock is held: see OpenLocked()
  BlockCache block_cache_;
  TableCache table_cache_;

  Mutex mutex_;
  // Signalled, with the mutex held, when a background task ends: whoever waits for the work to
  // make progress looks at what it waits for again.
  CondVar background_cv_{&mutex_};
  VersionSet versions_ SHALE_GUARDED_BY(mutex_);
  std::shared_ptr<MemTable> mem_ SHALE_GUARDED_BY(mutex_);  // the one that writes go into
  // The memtable that is waiting to be written out as a table, or null. Reads look at it after
  // `mem_`, and it is only ever set when `mem_` is switched and cleared by the flush.
  std::shared_ptr<MemTable> imm_ SHALE_GUARDED_BY(mutex_);
  // Whether `imm_` is set, for a compaction to check between its output files without the mutex.
  std::atomic<bool> has_imm_{false};
  // The write-ahead log of `mem_`. The writer is declared after the file it writes to, so that it
  // is destroyed first.
  std::unique_ptr<WritableFile> logfile_ SHALE_GUARDED_BY(mutex_);
  std::unique_ptr<log::Writer> log_ SHALE_GUARDED_BY(mutex_);
  uint64_t logfile_number_ SHALE_GUARDED_BY(mutex_) = 0;
  // Whether a record was appended to the log since it was last synced: the switch to a new
  // memtable syncs the old log, and does not when there is nothing to sync, which with every write
  // synced, as they are by default, is every time.
  bool log_unsynced_ SHALE_GUARDED_BY(mutex_) = false;
  SnapshotList snapshots_ SHALE_GUARDED_BY(mutex_);
  Status bg_error_ SHALE_GUARDED_BY(mutex_);  // the first error of the write path, or OK
  DBCounters counters_ SHALE_GUARDED_BY(mutex_);
  uint64_t memtable_seed_ SHALE_GUARDED_BY(mutex_) = 0;  // the seed of the next memtable
  // Whether a background task is queued or running: at most one is, and it is the one that makes
  // tables, so a table that is being made is also here, in `pending_outputs_`.
  bool bg_scheduled_ SHALE_GUARDED_BY(mutex_) = false;
  // The numbers of the tables that are being written and are in no version yet, which the deletion
  // of obsolete files must not take: the table of a flush, and the outputs of a compaction from the
  // moment each is numbered until the edit that names them has been tried.
  std::set<uint64_t> pending_outputs_ SHALE_GUARDED_BY(mutex_);
  // The request of a CompactRange() that the background task has not yet finished with, or null.
  // One at a time: a second caller waits for the first.
  ManualCompaction* manual_ SHALE_GUARDED_BY(mutex_) = nullptr;
  // Whether a compaction is running, which is for as long as the background task is merging files.
  bool compaction_running_ SHALE_GUARDED_BY(mutex_) = false;
  // Set when the database is closing: a task that has not started returns at once, and no more are
  // scheduled.
  std::atomic<bool> shutting_down_{false};

  // Where background work runs: the scheduler of the options, or the one the database made for
  // itself when there was none. Never null once the constructor has run.
  Scheduler* scheduler_ = nullptr;
  // Declared last, so that it is destroyed first, which joins its thread before anything that a
  // task could use is gone. Nothing is queued on it by then: the destructor waits for the task.
  std::unique_ptr<Scheduler> owned_scheduler_;
};

}  // namespace shale

#endif  // SHALE_DB_DB_IMPL_H_
