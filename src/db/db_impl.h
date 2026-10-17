#ifndef SHALE_DB_DB_IMPL_H_
#define SHALE_DB_DB_IMPL_H_

// The database: opening it and recovering what a previous run left, the write path, and point
// reads. The other files of src/db that belong to DBImpl are db_write.cc, the write path, and, as
// they come, the background work and the iterators.

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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
/// Everything the database changes is guarded by `mutex_`: the memtable and the log it is written
/// to, the version set, the snapshots and the first error of the log. A write holds it from the
/// sequence number to the memtable, which is what makes one writer at a time, and a read holds it
/// only to take the memtable, the version and the sequence number it will read.
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

  // These return NotSupported, or nothing, until they are written.
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;
  Status Flush() override;
  Status CompactRange(std::optional<std::string_view> begin,
                      std::optional<std::string_view> end) override;
  bool GetProperty(std::string_view property, std::string* value) override;

  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* snapshot) override;

  /// Opens the database: the work of DB::Open() after the options are checked. The sequence is
  /// the directory, the lock, CURRENT and the MANIFEST, the replay of the write-ahead logs that
  /// hold writes the tables do not, a new log, the recovered writes as a level 0 table, a new
  /// MANIFEST that names both, and last the deletion of the files that these replace. A failure
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

 private:
  // The work of OpenImpl(), with the mutex held.
  Status OpenLocked() SHALE_REQUIRES(mutex_);

  // Finds the files that recovery cares about: marks the numbers of the logs and tables in the
  // directory as used, and returns the numbers of the logs to replay, in ascending order.
  Status FindLogsToReplay(std::vector<uint64_t>* logs) SHALE_REQUIRES(mutex_);

  // Replays log `number` into `mem_`. `*max_sequence` is the last sequence number applied so far
  // and moves on with each batch; `*entries` counts the operations applied. Sets `*stop` when the
  // log has a defect that is not an error because paranoid_checks is off: replay ends there, with
  // nothing after it in this log or the later ones applied.
  Status ReplayLog(uint64_t number, SequenceNumber* max_sequence, uint64_t* entries, bool* stop)
      SHALE_REQUIRES(mutex_);

  // Applies the batch that is `record` to `mem_`, whole or not at all: returns Corruption, with
  // nothing applied, for a batch that does not decode, whose sequence number does not come after
  // `*max_sequence`, or whose operations would be numbered above kMaxSequenceNumber.
  Status ApplyRecoveredBatch(std::string_view record, SequenceNumber* max_sequence,
                             uint64_t* entries) SHALE_REQUIRES(mutex_);

  // Writes `mem_`, the recovered writes, as a level 0 table and adds it to `*edit`.
  Status WriteRecoveredMemTable(VersionEdit* edit) SHALE_REQUIRES(mutex_);

  // Deletes the logs and MANIFESTs that the database has no more use for, after a successful open:
  // every log but the current one, and every MANIFEST but the current one. A file it does not
  // know, or a log numbered beyond kMaxFileNumber, which is not the database's own, is not touched.
  void RemoveObsoleteFilesAfterOpen() SHALE_REQUIRES(mutex_);

  // Remembers `s` as the first error of the write path, if there is none yet: every later write
  // returns it.
  void RecordBackgroundError(const Status& s) SHALE_REQUIRES(mutex_);

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
  VersionSet versions_ SHALE_GUARDED_BY(mutex_);
  std::shared_ptr<MemTable> mem_ SHALE_GUARDED_BY(mutex_);
  // The write-ahead log of `mem_`. The writer is declared after the file it writes to, so that it
  // is destroyed first.
  std::unique_ptr<WritableFile> logfile_ SHALE_GUARDED_BY(mutex_);
  std::unique_ptr<log::Writer> log_ SHALE_GUARDED_BY(mutex_);
  uint64_t logfile_number_ SHALE_GUARDED_BY(mutex_) = 0;
  SnapshotList snapshots_ SHALE_GUARDED_BY(mutex_);
  Status bg_error_ SHALE_GUARDED_BY(mutex_);  // the first error of the write path, or OK
  DBCounters counters_ SHALE_GUARDED_BY(mutex_);
  uint64_t memtable_seed_ SHALE_GUARDED_BY(mutex_) = 0;  // the seed of the next memtable
};

}  // namespace shale

#endif  // SHALE_DB_DB_IMPL_H_
