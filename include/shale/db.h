#ifndef SHALE_DB_H_
#define SHALE_DB_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "shale/write_batch.h"

namespace shale {

/// A view of the database as it was at one moment, which stays the same however the database is
/// written afterwards. A snapshot comes from DB::GetSnapshot() and goes back to the same database
/// with DB::ReleaseSnapshot(); holding it keeps the older versions of the keys it can see from
/// being dropped by compaction.
class Snapshot {
 public:
  /// Destroys the snapshot. Only the database that made it does so.
  virtual ~Snapshot();
};

/// An ordered key-value store on disk: a log-structured merge tree with a write-ahead log, tables
/// in levels, and compaction between them.
///
/// Every method may be called from any number of threads at once. A Put(), Delete() or Write() that
/// returns OK has been logged: with WriteOptions::sync, durably; without it, as far as a crash of
/// the process.
///
/// A write that fails with an error of the log, or a flush or a compaction that fails, leaves the
/// database unable to write: every later write returns the first error, reads go on, and opening
/// the database again is what clears it.
class DB {
 public:
  /// Opens the database in the directory `name`, creating it if Options::create_if_missing says so.
  ///
  /// Open takes a lock on the directory, so that no second database, in this process or another,
  /// uses it at the same time; a second open fails with an IOError that names the LOCK file. It
  /// reads the MANIFEST that CURRENT names, replays the write-ahead logs that hold writes the
  /// tables do not, and starts a new log and a new MANIFEST. A write-ahead log that merely ends
  /// short, as a crash leaves it, is not an error. See Options::paranoid_checks for one that is
  /// damaged; a log that cannot be read fails the open with an IOError in either case.
  ///
  /// Returns InvalidArgument for options that ValidateOptions() refuses, for a path with no
  /// database when create_if_missing is false, and for a path with one when error_if_exists is
  /// true; Corruption for a CURRENT, MANIFEST or log that is damaged; and the IOError of whatever
  /// the Env could not do. On failure nothing that was in the directory has been lost, and `*db`
  /// is null.
  /// @param options how to open and run the database.
  /// @param name the directory of the database.
  /// @param db receives the open database.
  static Status Open(const Options& options, const std::string& name, std::unique_ptr<DB>* db);

  /// Creates a database handle that is not open; DB::Open() is how to get one.
  DB() = default;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  /// Closes the database. Aborts if iterators or snapshots that it made are still alive, since they
  /// refer to what the database is about to free.
  virtual ~DB();

  /// Sets `key` to `value`.
  /// @param options how to write.
  /// @param key the key, at most kMaxKeySize bytes; may be empty.
  /// @param value the value, at most kMaxValueSize bytes; may be empty.
  /// @return OK, InvalidArgument for a key or value over the limit, or the error that stops writes.
  virtual Status Put(const WriteOptions& options, std::string_view key, std::string_view value) = 0;

  /// Removes `key`. It is not an error if there is none.
  /// @param options how to write.
  /// @param key the key, at most kMaxKeySize bytes; may be empty.
  /// @return OK, InvalidArgument for a key over the limit, or the error that stops writes.
  virtual Status Delete(const WriteOptions& options, std::string_view key) = 0;

  /// Applies every operation of `updates`, atomically: after a crash, all of them are there or none
  /// is. They take the next sequence numbers, in order, so that a later operation on a key
  /// supersedes an earlier one. The batch is given its sequence number, so it is changed.
  ///
  /// Writers that arrive while another write is being made are queued, and are written together,
  /// in the order they queued, as one log record with one sync. A group holds up to a megabyte of
  /// batches, and less when its first is small, and a write that asked for a sync is never in a
  /// group led by a write that did not.
  /// A write is slowed by a millisecond while level 0 holds Options::l0_slowdown_trigger files or
  /// more, and waits when it needs a new memtable and level 0 holds Options::l0_stop_trigger or
  /// more, or the previous memtable is still being written out.
  ///
  /// Returns InvalidArgument, applying nothing, for a null batch and for one with a key or value
  /// over the limit. An empty batch is accepted and does nothing. Returns Corruption, applying
  /// nothing and stopping no later write, if the batch would need a sequence number above the
  /// largest there can be, which only a database that has taken 2^56 writes, or whose MANIFEST
  /// says it has, can run into; a smaller batch may still fit. Returns the first error of the log
  /// once one has failed.
  /// @param options how to write.
  /// @param updates the batch; not owned.
  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;

  /// Looks up `key`.
  /// @param options how to read; `options.snapshot` reads as of a snapshot.
  /// @param key the key to look for.
  /// @param value receives the value; left alone unless the status is OK.
  /// @return OK if the key has a value, NotFound if it has none or was removed, or the Corruption
  ///   or IOError of a table that could not be read: a table that cannot be read cannot say that a
  ///   key is absent, so that is never reported as NotFound.
  virtual Status Get(const ReadOptions& options, std::string_view key, std::string* value) = 0;

  /// Returns an iterator over the whole database, as it is now or as of `options.snapshot`. It is
  /// not affected by writes made after it was created. It must be destroyed before the database.
  /// Valid() being false after a seek or a step may mean that a table could not be read and not
  /// that the data ran out, so check status() when it is.
  /// @param options how to read.
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;

  /// Returns a snapshot of the database as it is now. Give it back with ReleaseSnapshot().
  virtual const Snapshot* GetSnapshot() = 0;

  /// Gives back a snapshot from GetSnapshot() of this database, which must not be used again.
  /// Aborts for a snapshot that another database made.
  /// @param snapshot the snapshot to release.
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  /// Writes what the memtable holds out as a table, and returns when that is done. Does nothing
  /// if the memtable is empty and none is waiting to be written.
  /// @return OK, or the error that stops writes, if one has: the data is then in the log still, and
  ///   the next open recovers it.
  virtual Status Flush() = 0;

  /// Compacts the files that hold keys from `begin` to `end`, so that the space that superseded and
  /// removed entries take is given back in the files that are rewritten. Returns when that is done.
  ///
  /// The memtable is written out first. Then the range is compacted one level at a time, from
  /// level 0 down to the level just above the deepest level that holds a file overlapping the
  /// range, and always level 0: the files of each of those levels that overlap the range are
  /// merged, with the files of the next level that they overlap, into that next level, and are
  /// rewritten whether or not they could have moved down as they were. The range ends up in the
  /// deepest level that had any of it, or in level 1 if that is deeper. The deepest populated level
  /// is rewritten only when a shallower level overlaps the range and is merged into it: entries
  /// that are only there, with nothing above to merge with, stay as they are, and what they take is
  /// not given back.
  /// @param begin the first key of the range; unbounded if nullopt.
  /// @param end the last key of the range; unbounded if nullopt.
  virtual Status CompactRange(std::optional<std::string_view> begin,
                              std::optional<std::string_view> end) = 0;

  /// Reads a property of the database by name, such as "shale.num-files-at-level0".
  /// @param property the name of the property.
  /// @param value receives the property's value as text.
  /// @return false if there is no property of that name.
  virtual bool GetProperty(std::string_view property, std::string* value) = 0;
};

/// Deletes the database in the directory `name`: every file the database makes, a log that a
/// lenient open set aside as "<log name>.dropped" among them, and then the directory if nothing
/// else is in it. A file that is not the database's is left alone, and so is the directory that
/// holds it. It is a success if there is no such directory.
///
/// It takes the database's lock first, so that it fails with an IOError that names the LOCK file
/// for a database that is open, and removes nothing.
/// @param name the directory of the database.
/// @param options only `options.env` is used; null means Env::Default().
Status DestroyDB(const std::string& name, const Options& options);

}  // namespace shale

#endif  // SHALE_DB_H_
