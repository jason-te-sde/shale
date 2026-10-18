#ifndef SHALE_IO_MEMTABLE_H_
#define SHALE_IO_MEMTABLE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "io/skiplist.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/arena.h"
#include "util/dbformat.h"

namespace shale {

/// The in-memory table that every write goes into first, ordered by internal key.
///
/// A write is an entry with a sequence number, so an update does not replace the entry it
/// supersedes and a deletion does not remove one: they are added next to it, and a read picks the
/// newest entry its snapshot may see. That is what lets readers run with no lock against the one
/// writer, and what lets a snapshot read an older state. The memtable only grows; when it is big
/// enough the engine writes it out as a table and starts another.
///
/// One thread at a time may call Add(), and it aborts if two overlap, because the arena that holds
/// the entries and the skip list that orders them both assume it. Get() and the iterators may be
/// used from any number of threads, at any time, including while Add() runs. A MemTable must be
/// created with std::make_shared: an iterator keeps it alive, and NewIterator() aborts otherwise.
///
/// An entry is stored in the arena as the varint32 length of the internal key, the internal key,
/// the varint32 length of the value, and the value. The skip list holds a pointer to each.
class MemTable : public std::enable_shared_from_this<MemTable> {
 public:
  /// Creates an empty memtable. `seed` seeds the skip list's choice of node heights.
  explicit MemTable(uint64_t seed);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  /// Returns how much memory the memtable holds from the system: the entries, the skip list's
  /// nodes, and the arena's own bookkeeping. This is the figure the engine compares with the write
  /// buffer size to decide when to flush. Safe to call from any thread at any time.
  size_t ApproximateMemoryUsage() const;

  /// Returns an iterator over the entries in internal key order: user key ascending, then
  /// newest first. Its key() is an internal key, and Seek() takes one, aborting on anything
  /// shorter than the 8-byte tag, a user key most likely. The iterator shares
  /// ownership of the memtable, so it stays valid after every other owner is gone. Aborts if the
  /// memtable is not owned by a shared_ptr.
  std::unique_ptr<Iterator> NewIterator() const;

  /// Adds an entry. Aborts if another Add() is in progress, if an entry with the same user key,
  /// sequence number and type is already there, or if `seq` is larger than kMaxSequenceNumber.
  /// The sequence number is what keeps two entries for one user key apart; the engine never
  /// issues one twice.
  /// @param seq the sequence number of the write.
  /// @param type kValue for a put, kDeletion for a deletion.
  /// @param key the user key; its bytes are copied.
  /// @param value the value, copied; must be empty for a deletion.
  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  /// Looks `key` up as of its sequence number: finds the newest entry for its user key that was
  /// written at or before that sequence number.
  ///
  /// Returns true if there is one, and then a put sets `*value` to its value and `*s` to OK, and a
  /// deletion sets `*s` to NotFound and leaves `*value` alone. Returns false, touching neither
  /// output, if the memtable has no entry for the user key at that sequence number, which says
  /// nothing about older data in the tables: the caller goes on looking there.
  /// @param key the user key and the snapshot to read at.
  /// @param value receives the value of a put; must not be null.
  /// @param s receives the result; must not be null.
  bool Get(const LookupKey& key, std::string* value, Status* s) const;

 private:
  class Cursor;

  // Orders two entries by their internal keys. It is handed pointers to entries, or to a
  // LookupKey's memtable_key(), which has the same layout up to the end of the internal key.
  struct KeyComparator {
    int operator()(const char* a, const char* b) const;
  };

  using Table = SkipList<const char*, KeyComparator>;

  // Declared before table_, which allocates from it.
  Arena arena_;
  Table table_;
  std::atomic<bool> adding_{false};
};

}  // namespace shale

#endif  // SHALE_IO_MEMTABLE_H_
