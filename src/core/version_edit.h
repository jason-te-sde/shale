#ifndef SHALE_CORE_VERSION_EDIT_H_
#define SHALE_CORE_VERSION_EDIT_H_

// The change that turns one version of the table set into the next, and its byte encoding.
//
// An edit is what the MANIFEST holds: one record per edit, replayed in order to rebuild the table
// set after a restart. This file is the pure kernel's: it does no I/O and reads no clock, and it
// knows nothing about whether an edit makes sense. version_state.h decides that.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shale/status.h"
#include "util/dbformat.h"

namespace shale {

/// The number of levels. Level 0 holds tables flushed from memtables, which may overlap each
/// other; every level below it holds tables whose key ranges are disjoint.
inline constexpr int kNumLevels = 7;

/// What the version set records about one table file.
struct FileMetaData {
  /// The file's number, which names it on disk and is never reused.
  uint64_t number = 0;
  /// The file's size in bytes.
  uint64_t file_size = 0;
  /// The smallest internal key in the file, in CompareInternalKey() order.
  std::string smallest;
  /// The largest internal key in the file, in CompareInternalKey() order.
  std::string largest;

  /// Two descriptions are equal when every field is.
  friend bool operator==(const FileMetaData&, const FileMetaData&) = default;
};

/// A change to the table set: which files appear, which disappear, and how far the counters have
/// moved. A field that is not set leaves the matching part of the state as it was.
///
/// The encoding is a sequence of records, each a tag followed by the record's fields, and nothing
/// else: an edit with no field set encodes to the empty string. The tags are
///  1  log_number          varint64
///  2  next_file_number    varint64
///  3  last_sequence       varint64
///  4  compact pointer     level, length-prefixed internal key
///  5  deleted file        level, varint64 number
///  6  new file            level, varint64 number, varint64 size,
///                         length-prefixed smallest key, length-prefixed largest key
/// EncodeTo() writes them in that order, so one edit has one encoding, and writes a tag and a level
/// as a varint32. A level is always below kNumLevels.
///
/// DecodeFrom() reads a tag and a level as a varint64 and range-checks the value, so a tag of 2^32
/// or more is an unknown tag and a level of 2^32 or more is a level out of range, and not a
/// truncation, as a 32-bit read would call them. A varint of more than 64 bits is reported as
/// truncated, since it cannot be told from one that was cut off. An over-long encoding of a small
/// value, a continuation byte followed by a zero byte, is accepted, as the varint readers accept
/// it, and means the value it spells.
struct VersionEdit {
  /// The number of the oldest write-ahead log that still holds data not in a table.
  std::optional<uint64_t> log_number;
  /// The next number to give a file; every file already numbered is below it.
  std::optional<uint64_t> next_file_number;
  /// The sequence number of the newest write the tables hold.
  std::optional<SequenceNumber> last_sequence;
  /// Where the next compaction of each level starts: (level, internal key). When one level is
  /// named twice, the later entry wins. An empty key means no pointer: applied to a state, it
  /// clears the level's.
  std::vector<std::pair<int, std::string>> compact_pointers;
  /// The files that leave the set: (level, file number).
  std::set<std::pair<int, uint64_t>> deleted_files;
  /// The files that join the set: (level, file).
  std::vector<std::pair<int, FileMetaData>> new_files;

  /// Appends the encoding of this edit to `*dst`. Aborts if a level is outside [0, kNumLevels): a
  /// record the decoder would refuse must not reach the MANIFEST, where it would make the
  /// database unopenable.
  void EncodeTo(std::string* dst) const;

  /// Replaces this edit with the one `src` encodes. Returns Corruption, with a message that starts
  /// "VersionEdit: ", and leaves this edit as it was if `src` has an unknown tag, a level that is
  /// out of range, or a record that ends before its last field does. A field that is set twice
  /// takes the later value. `src` is not kept.
  Status DecodeFrom(std::string_view src);

  /// Returns a description for logs and test failures: one line per field that is set, with keys
  /// shown as the user key, the sequence number and the type.
  std::string DebugString() const;

  /// Two edits are equal when every field is, the order of the lists included.
  friend bool operator==(const VersionEdit&, const VersionEdit&) = default;
};

}  // namespace shale

#endif  // SHALE_CORE_VERSION_EDIT_H_
