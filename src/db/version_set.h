#ifndef SHALE_DB_VERSION_SET_H_
#define SHALE_DB_VERSION_SET_H_

// The table set as the database keeps it: immutable versions that reads run against, and the
// version set that moves from one to the next through the MANIFEST.
//
// Two orders carry the crash safety of everything above this file.
//
//  1. An edit is made durable in the MANIFEST before the version it makes is installed. A reader
//     that has seen a version never sees it taken back by a crash, and a table that a version
//     names is one the MANIFEST names too.
//  2. CURRENT is switched to a new MANIFEST by renaming a synced file over it and syncing the
//     directory, so a crash leaves it naming the old MANIFEST or the new one, whole.
//
// The pure rules of what an edit may do live in core/version_state.h. This file adds the files.

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/table_cache.h"
#include "io/log_writer.h"
#include "io/table.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/dbformat.h"

namespace shale {

/// The table set at one moment, as a value that never changes: which files are at which level.
/// A read takes a shared pointer to the current Version and runs against it, whatever flush or
/// compaction installs next, and the files it names are not deleted while any pointer to it lives.
///
/// It does not own the table cache, which must outlive it. Safe to use from any number of threads.
class Version {
 public:
  /// Creates the version for `state`, which must satisfy CheckInvariants(). Tables are read
  /// through `table_cache`, which is not owned.
  Version(VersionState state, TableCache* table_cache);

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  /// Returns the files, level by level, and the counters as of this version.
  const VersionState& state() const { return state_; }

  /// Looks `key` up in the tables. Returns OK with `*value` set to the newest value the lookup's
  /// sequence number can see, NotFound if there is none or the newest entry it can see is a
  /// deletion, or the error of a table that could not be opened or read: a table that cannot be
  /// read cannot say that a key is absent either, so it is never reported as a miss. `*value` is
  /// left alone unless the status is OK.
  ///
  /// Level 0 is searched newest file first, and only the files whose range covers the user key,
  /// since its files overlap and a newer one holds the newer entries. Every other level has files
  /// that are disjoint, so one file at most can hold the key, found by binary search, and levels
  /// are searched from the shallowest to the deepest, since a shallower level holds the newer
  /// entries. The search ends at the first entry for the key, a deletion included.
  /// @param options how to read.
  /// @param key the user key and the sequence number to read as of.
  /// @param value receives the value.
  /// @param files_probed receives how many tables were consulted, or null.
  Status Get(const TableReadOptions& options, const LookupKey& key, std::string* value,
             int* files_probed) const;

  /// Appends to `*iters` the iterators that together cover every entry of this version, as a read
  /// of the whole database merges them: one for each level 0 file, newest first, then one for each
  /// level below it that has files, which reads that level's files one after another. All are over
  /// internal keys, in CompareInternalKey() order, and the order is the priority: listed in this
  /// order to a merging iterator, an entry of a newer file comes before an equal one of an older.
  ///
  /// The iterators hold the tables they have opened but not this Version, and a level's iterator
  /// refers to its description of the files and opens them as the iteration reaches them. The
  /// caller keeps a pointer to this Version for as long as it uses them, which is also what keeps
  /// the files from being deleted.
  /// @param options how to read.
  /// @param iters receives the iterators, appended to what it holds.
  void AddIterators(const TableReadOptions& options,
                    std::vector<std::unique_ptr<Iterator>>* iters) const;

  /// Returns the number of files at `level`, which must be in [0, kNumLevels).
  int NumFiles(int level) const;

 private:
  const VersionState state_;
  TableCache* const table_cache_;
};

/// The largest file number a database accepts, and the largest the next file number may be: a
/// MANIFEST is refused if its number is at or above this, or if its counter is above it, and the
/// counter is never written above it. It plays the part for file numbers that kMaxSequenceNumber
/// does for sequence numbers: no real database approaches it (a file a microsecond for 2000
/// years), and content beyond it is a forged or damaged file whose numbers would otherwise leave
/// the counter within a few files of the end of the 64 bits, where the database would fail
/// instead of opening, or abort as it numbered its first files.
inline constexpr uint64_t kMaxFileNumber = uint64_t{1} << 56;

/// The versions of a database, and the MANIFEST that records how it got from each to the next.
///
/// The MANIFEST is a log of VersionEdits, written with log::Writer. Its first record is a snapshot
/// of the whole state and each later one an edit; replaying them in order rebuilds the current
/// version. CURRENT names the live MANIFEST. A database is opened by Recover(), which reads it, and
/// then by WriteNewManifest(), which starts a new MANIFEST: the one that was read may end in a torn
/// record, which cannot be appended to, and a new one is a snapshot, which also keeps the log from
/// growing for ever. After that, every change is LogAndApply().
///
/// A new database is CreateNew() and then LogAndApply().
///
/// Not thread-safe: the database calls it with its mutex held. The Versions it hands out are
/// another matter, and may be used and dropped from any thread.
class VersionSet {
 public:
  /// Creates an empty version set for the database in `dbname`, over no files yet: call CreateNew()
  /// or Recover() first. Neither `env` nor `table_cache` is owned, and both must outlive it.
  VersionSet(std::string dbname, Env* env, TableCache* table_cache);

  /// Closes the MANIFEST.
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  /// Creates the files of a new database: MANIFEST-000001 holding SnapshotEdit(VersionState{}),
  /// synced, its directory entry synced, and CURRENT naming it, as SetCurrentFile() writes it. The
  /// state is empty and NextFileNumber() is 2. It replaces a CURRENT that is there; whether the
  /// database already exists is the caller's to decide.
  ///
  /// On failure the database is either not there, with no CURRENT and the MANIFEST removed, or it
  /// is whole, if the failure came after CURRENT was switched. In the first case a retry on this
  /// version set goes through. In the second CURRENT may name MANIFEST-000001, and writing it again
  /// would empty a file that CURRENT names, so a retry is refused with the error of the attempt
  /// that failed: recover the database with a new version set.
  ///
  /// The MANIFEST stays open for LogAndApply(). Aborts if this version set was already loaded.
  Status CreateNew();

  /// Rebuilds the state from the MANIFEST that CURRENT names, and installs it. Does not open a
  /// MANIFEST for writing: call WriteNewManifest() before LogAndApply(). The counters are those of
  /// the last record, and NextFileNumber() is also above the MANIFEST's own number.
  ///
  /// Returns Corruption("CURRENT file does not end with newline"), Corruption("CURRENT names an
  /// invalid file") for one that is not a MANIFEST name as SetCurrentFile() writes it, the I/O
  /// error of reading CURRENT or the MANIFEST, and Corruption for any defect the log reader
  /// reports, whether or not a valid record follows it (only the end of the file, cut short, is
  /// the end of the log), for a record that does not decode, for an edit that ApplyEdit() refuses,
  /// for a MANIFEST with no record at all, for a last sequence number that no write could have,
  /// and for a MANIFEST numbered at kMaxFileNumber or above or with a counter above it. A defect
  /// is reported in preference to the failure of a record
  /// after it, which the gap may have caused. Every Corruption but the two about CURRENT names the
  /// MANIFEST in its message. On failure nothing is installed.
  ///
  /// Aborts if this version set was already loaded.
  Status Recover();

  /// Applies `edit` and starts a new MANIFEST: puts NextFileNumber() and LastSequence() into the
  /// edit, applies it to the current state, writes MANIFEST-<n> for a number n it takes from
  /// NewFileNumber(), holding SnapshotEdit of the new state as its one record, syncs it and its
  /// directory entry, makes CURRENT name it with SetCurrentFile(), and then installs the new
  /// version. The MANIFEST stays open for LogAndApply(), and the one before it is closed and left
  /// for the database to delete. The database calls it once, when it is opened. It returns
  /// Corruption, and writes nothing, if the counter is at kMaxFileNumber or above: the snapshot
  /// would record a counter that Recover() refuses.
  ///
  /// On any failure nothing is installed. If the edit is refused, or the new MANIFEST could not be
  /// written, or switching CURRENT failed and CURRENT is seen to name the old MANIFEST still, the
  /// new file is removed and this version set goes on as it was. If switching CURRENT failed and
  /// CURRENT may name the new MANIFEST, as after a failed directory sync, the new file is kept and
  /// this version set refuses LogAndApply() until a WriteNewManifest() succeeds: an edit appended
  /// to the old MANIFEST would be lost if CURRENT names the new one. The new MANIFEST holds the
  /// edit, so a later recovery may find it applied, and a caller must not delete the files the edit
  /// adds.
  /// @param edit what to apply.
  Status WriteNewManifest(VersionEdit edit);

  /// Applies `edit` to the current state, makes it durable in the MANIFEST, and installs the new
  /// version. NextFileNumber() and LastSequence() go into the edit first, replacing what the edit
  /// holds for them, so that every record of the MANIFEST carries the counters; ApplyEdit() checks
  /// the edit against the state, and an edit it refuses, which is Corruption, writes nothing and
  /// installs nothing. The record is appended and synced before the version is installed, so a
  /// version that is installed is one a crash will not take back.
  ///
  /// If appending or syncing fails, nothing is installed, and the MANIFEST may or may not hold the
  /// edit: it will be there if a later recovery finds it, so the caller must not delete files the
  /// edit adds. The MANIFEST is also no longer trusted: after a failed write, what it holds is
  /// unknown, and a later Sync() that reports success proves nothing. So every later call returns
  /// that first error, writing nothing, until WriteNewManifest() starts a new one.
  ///
  /// Returns Corruption, writing nothing and installing nothing, if the counter is above
  /// kMaxFileNumber, which only the numbers given out can have done: the record would be one that
  /// Recover() refuses. Aborts if there is no MANIFEST open, which is before CreateNew() or
  /// WriteNewManifest().
  /// @param edit what to apply; its log number, files and compact pointers are the caller's.
  Status LogAndApply(VersionEdit edit);

  /// Returns the current version.
  std::shared_ptr<const Version> current() const { return current_; }

  /// Returns a file number that has not been used, and will not be again: one for a table, a log
  /// or a MANIFEST. A number at or above kMaxFileNumber is one that no MANIFEST can hold: the
  /// next LogAndApply() and WriteNewManifest() refuse, and nothing read from disk gets the counter
  /// that far. Aborts only if the counter would wrap round the 64 bits, which is a bug.
  uint64_t NewFileNumber();

  /// Makes sure that numbers handed out from now on are above `number`, for a file that was found
  /// and is not in the state: a log that recovery replays, say. Does nothing if they already are,
  /// and does nothing for a `number` at or above kMaxFileNumber: a file named for one cannot be a
  /// file of this database's own, which never numbers one so high, and raising the counter for it
  /// would take it to where the next MANIFEST is refused.
  void MarkFileNumberUsed(uint64_t number);

  /// Returns the number that NewFileNumber() will return next.
  uint64_t NextFileNumber() const { return next_file_number_; }

  /// Returns the sequence number of the newest write: of the tables at first, and moved forward as
  /// the database writes and replays.
  SequenceNumber LastSequence() const { return last_sequence_; }

  /// Moves LastSequence() forward to `sequence`, which is the number the next edit records. Aborts
  /// if it is below the one now, since a sequence number is never reused.
  void SetLastSequence(SequenceNumber sequence);

  /// Returns the number of the oldest write-ahead log that still holds writes not in a table.
  uint64_t LogNumber() const { return current_->state().log_number; }

  /// Returns the number of the MANIFEST in use: the one written, or the one read before
  /// WriteNewManifest(). The database deletes MANIFESTs with a lower number.
  uint64_t ManifestFileNumber() const { return manifest_file_number_; }

  /// Returns the numbers of the files of every Version that is still alive anywhere: the current
  /// one, and every earlier one that a reader or an iterator still holds. These are the files that
  /// must not be deleted. Versions are tracked by weak pointer, so one that is dropped leaves the
  /// set without a call to anything.
  std::set<uint64_t> LiveFiles();

 private:
  // Writes MANIFEST-<number> with `snapshot` as its one record, syncs it, and points CURRENT at it.
  // On success the new MANIFEST is the open one and the earlier one is closed.
  Status OpenNewManifest(uint64_t number, const VersionEdit& snapshot);

  // Whether CURRENT may be naming MANIFEST `number`, for after a switch to it failed: false only if
  // CURRENT is seen to name something else, or to be absent.
  bool CurrentMayNameManifest(uint64_t number);

  // Makes `state` the current version, and remembers the version for LiveFiles().
  void Install(VersionState state);

  const std::string dbname_;
  Env* const env_;
  TableCache* const table_cache_;

  std::shared_ptr<const Version> current_;
  std::vector<std::weak_ptr<const Version>> versions_;  // every version made, expired ones pruned
  uint64_t next_file_number_;
  SequenceNumber last_sequence_ = 0;
  uint64_t manifest_file_number_ = 0;
  bool loaded_ = false;  // CreateNew() or Recover() has succeeded

  // The MANIFEST being appended to, or null. The writer is declared after the file it writes to,
  // so that it is destroyed first.
  std::unique_ptr<WritableFile> manifest_file_;
  std::unique_ptr<log::Writer> manifest_log_;
  // The first error of the MANIFEST that is open, or OK: once set, nothing more is written to it.
  Status manifest_error_;
};

}  // namespace shale

#endif  // SHALE_DB_VERSION_SET_H_
