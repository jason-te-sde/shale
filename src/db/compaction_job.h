#ifndef SHALE_DB_COMPACTION_JOB_H_
#define SHALE_DB_COMPACTION_JOB_H_

// Running one compaction: merging the input tables into new ones, keeping what a reader can still
// need, and describing the result as an edit.
//
// The job is the part of a compaction that does the work and needs no lock: it reads tables and
// writes tables, and holds nothing of the database but what its context gives it. Choosing the
// inputs is the picker's (core/compaction_picker.h), deciding which entries survive is the
// filter's (core/compaction_filter.h), and installing the edit, deleting the files it makes
// obsolete and answering for a failure belong to the database.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/compaction_picker.h"
#include "core/version_edit.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "io/table_builder.h"
#include "shale/env.h"
#include "shale/status.h"
#include "util/dbformat.h"

namespace shale {

/// What a CompactionJob needs of the database that runs it.
struct CompactionJobContext {
  /// The database directory, where the tables are read from and written to.
  std::string dbname;
  /// The Env to write through; not owned.
  Env* env = nullptr;
  /// The shape of the tables the job writes.
  TableBuilderOptions table_options;
  /// The cache that the inputs are read through and the outputs are opened in to be checked; not
  /// owned.
  TableCache* table_cache = nullptr;
  /// The size an output is closed at, in bytes: a file that has reached it is closed before the
  /// next user key, and a file is also closed when the level below holds more than ten times it
  /// of the files that it overlaps.
  uint64_t target_file_size = 0;
  /// Returns a file number for an output. It runs on the job's thread, without the database's
  /// mutex held by the job, and the database's takes it, allocates, and records the number among
  /// the tables that are being made, so that the deletion of obsolete files leaves the output
  /// alone until an edit names it. The job fails, without creating the file, with a number at or
  /// above kMaxFileNumber.
  std::function<uint64_t()> allocate_file_number;
  /// Called on the job's thread after each output file is finished, durable and named, and before
  /// the next is begun: the database writes out the memtable that is waiting here, so that a write
  /// that is stalled for it does not wait for the whole compaction. May be empty.
  std::function<void()> between_outputs;
};

/// What a job did, for the counters and the log.
struct CompactionJobStats {
  uint64_t bytes_read = 0;     ///< The sizes of the input files.
  uint64_t bytes_written = 0;  ///< The sizes of the output files.
  uint64_t tombstones_dropped =
      0;                          ///< Deletions left out because nothing was left for them to hide.
  uint64_t shadowed_dropped = 0;  ///< Entries left out because a newer one hides them.
  uint64_t entries_kept = 0;      ///< Entries written to the outputs.
};

/// One compaction: reads the files of a plan as a single sorted stream, leaves out the entries
/// that no reader can see, and writes the rest into new tables at the level below the plan's.
///
/// Every entry goes through a CompactionFilter, with the snapshots that were open when the job was
/// made, and a deletion is left out only when no level below the output can hold its key. An
/// output is closed before the entry that would make it too big, but never between two entries of
/// one user key, so that the outputs of a level hold disjoint ranges, which is what the version
/// state demands of every level below 0.
///
/// A job runs once and is not thread-safe, but Run() touches nothing the database guards, so it
/// runs without the database's mutex. The outputs are not part of any version until the caller
/// installs MakeEdit(); until then they are files that no version names.
class CompactionJob {
 public:
  /// Creates a job for `plan`, which reads the files that `input_version` holds and which the
  /// version must go on holding, and so keep from being deleted, for as long as the job lives.
  /// @param ctx the database's side of the job.
  /// @param plan what to merge; a trivial move is not a job, and is not what this runs.
  /// @param input_version the version the plan was picked from.
  /// @param snapshots the sequence numbers of the snapshots that are open, in any order.
  CompactionJob(CompactionJobContext ctx, CompactionPlan plan,
                std::shared_ptr<const Version> input_version,
                std::vector<SequenceNumber> snapshots);

  CompactionJob(const CompactionJob&) = delete;
  CompactionJob& operator=(const CompactionJob&) = delete;

  /// Writes the outputs. Returns OK when every entry has been read and the outputs are durable,
  /// or the first error: the I/O error of a file, or the Corruption of an input that cannot be
  /// read, which names the table. An error removes the outputs that were finished and the one that
  /// was being written, and outputs() is then empty. Aborts if called twice.
  Status Run();

  /// Returns the edit that installs the result: it deletes every input file from its level, adds
  /// the outputs at `plan.level + 1`, and moves the compact pointer of `plan.level` to
  /// `plan.next_compact_pointer`. Meant for after a Run() that returned OK.
  VersionEdit MakeEdit() const;

  /// Removes the files of the outputs that are finished, and forgets them. Run() does it when it
  /// fails. A caller that decides not to install the result of a Run() that succeeded does it, and
  /// only then: once an edit that adds the outputs has been tried, the MANIFEST may hold it, and
  /// the files must stay.
  void RemoveOutputs();

  /// Returns the description of each output file, in key order: the files that Run() finished.
  const std::vector<FileMetaData>& outputs() const { return outputs_; }

  /// Returns what the job has counted. Complete after a Run() that returned OK.
  const CompactionJobStats& stats() const { return stats_; }

 private:
  // The work of Run(), which cleans up after it if it fails.
  Status RunImpl();

  const CompactionJobContext ctx_;
  const CompactionPlan plan_;
  const std::shared_ptr<const Version> input_version_;
  const std::vector<SequenceNumber> snapshots_;
  std::vector<FileMetaData> outputs_;
  CompactionJobStats stats_;
  bool started_ = false;
};

}  // namespace shale

#endif  // SHALE_DB_COMPACTION_JOB_H_
