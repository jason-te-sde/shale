#ifndef SHALE_DB_SNAPSHOT_LIST_H_
#define SHALE_DB_SNAPSHOT_LIST_H_

// The snapshots that are open, which a compaction needs to know: a version of a key that some
// snapshot can still see must survive it.

#include <algorithm>
#include <cstddef>
#include <list>
#include <vector>

#include "shale/db.h"
#include "util/check.h"
#include "util/dbformat.h"

namespace shale {

/// A snapshot as the database makes it: the sequence number it reads as of, and which database
/// made it, so that a snapshot handed back to another database is noticed.
class SnapshotImpl final : public Snapshot {
 public:
  /// Creates a snapshot that reads as of `seq`, made by the database at address `owner`.
  SnapshotImpl(SequenceNumber seq, const void* owner) : seq_(seq), owner_(owner) {}

  /// Returns the sequence number the snapshot reads as of: it sees every write numbered at or
  /// below it.
  SequenceNumber sequence() const { return seq_; }

  /// Returns the address of the database that made the snapshot.
  const void* owner() const { return owner_; }

 private:
  const SequenceNumber seq_;
  const void* const owner_;
};

/// The open snapshots of one database, oldest first. Not thread-safe: the database guards it with
/// its mutex.
///
/// A snapshot is owned by the list from New() until Delete(), and the pointer New() returned stays
/// valid, whatever else is added or deleted, until then. Both rules are checked: a snapshot taken
/// at a lower sequence number than the newest one, which would put the list out of order, and a
/// Delete() of one that is not in the list, a second Delete() of the same snapshot or a snapshot of
/// another list, abort. A pointer that is freed twice is the alternative.
class SnapshotList {
 public:
  /// Adds a snapshot that reads as of `seq`, which must be at least that of every snapshot in the
  /// list, and returns it.
  /// @param seq the sequence number to read as of; the database's newest.
  /// @param owner the address of the database that makes the snapshot.
  const SnapshotImpl* New(SequenceNumber seq, const void* owner) {
    SHALE_CHECK(snapshots_.empty() || seq >= snapshots_.back().sequence(),
                "SnapshotList::New: snapshots are taken in ascending order of sequence number");
    snapshots_.emplace_back(seq, owner);
    return &snapshots_.back();
  }

  /// Removes `snapshot`, which must be in the list, and frees it.
  /// @param snapshot a pointer New() returned and that has not been deleted.
  void Delete(const SnapshotImpl* snapshot) {
    const auto it = std::find_if(snapshots_.begin(), snapshots_.end(),
                                 [snapshot](const SnapshotImpl& s) { return &s == snapshot; });
    SHALE_CHECK(it != snapshots_.end(),
                "SnapshotList::Delete: not a snapshot of this list (released twice?)");
    snapshots_.erase(it);
  }

  /// Returns how many snapshots are in the list.
  size_t size() const { return snapshots_.size(); }

  /// Returns the sequence numbers of the snapshots in ascending order, one for each snapshot, so
  /// that two taken at the same sequence number are listed twice. It is what a compaction is given
  /// to know which versions of a key it must keep.
  std::vector<SequenceNumber> Sequences() const {
    std::vector<SequenceNumber> sequences;
    sequences.reserve(snapshots_.size());
    for (const SnapshotImpl& snapshot : snapshots_) {
      sequences.push_back(snapshot.sequence());
    }
    return sequences;
  }

 private:
  std::list<SnapshotImpl> snapshots_;  // in the order they were taken, so ascending
};

}  // namespace shale

#endif  // SHALE_DB_SNAPSHOT_LIST_H_
