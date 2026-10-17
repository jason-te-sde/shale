#ifndef SHALE_CORE_VERSION_STATE_H_
#define SHALE_CORE_VERSION_STATE_H_

// The table set at one moment, and the one function that moves it forward.
//
// A VersionState says which table files exist at which level and where the counters stand. It is a
// plain value: ApplyEdit() takes one and an edit and returns the next, or refuses, and nothing
// else changes it. Everything the database promises about its files, that no number is used twice,
// that the levels below 0 hold disjoint ranges, that the counters never go back, is checked here,
// on the edit's way in. A MANIFEST that replays into a state ApplyEdit accepted is one the rest of
// the engine can trust, and one it cannot is refused at the door instead of being found by a read.
//
// This is the pure kernel's: no I/O, no clock, no threads, no locks.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/version_edit.h"
#include "shale/status.h"
#include "util/dbformat.h"

namespace shale {

/// Which files make up the database, level by level, and the counters that go with them.
struct VersionState {
  /// The files of each level. Level 0 is sorted by file number, ascending, which is the order they
  /// were flushed in, and its files may overlap. Every other level is sorted by smallest key, and
  /// its files' user-key ranges are disjoint.
  std::array<std::vector<FileMetaData>, kNumLevels> files;
  /// Where the next compaction of each level starts, as an internal key; the empty string means
  /// the level has no pointer yet.
  std::array<std::string, kNumLevels> compact_pointers;
  /// The number of the oldest write-ahead log that still holds writes not yet in a table.
  uint64_t log_number = 0;
  /// The next number to give a file; every file in the state is numbered below it. A new
  /// database starts it at 2.
  uint64_t next_file_number = 2;
  /// The sequence number of the newest write that the tables hold.
  SequenceNumber last_sequence = 0;

  /// Two states are equal when every file, pointer and counter is.
  friend bool operator==(const VersionState&, const VersionState&) = default;
};

/// Applies `edit` to `base`. On success `*out` is the new state; on failure `*out` is untouched and
/// the Corruption message names the rule that failed. `out` may be `&base`.
///
/// The edit's deletions are applied first, against `base`, then its additions, then the counters
/// and compact pointers it sets (a level named twice takes the later pointer). The result is then
/// held to these rules, and the first one broken is reported:
///  - a level in the edit is in [0, kNumLevels): "VersionEdit: level <n> out of range"
///  - log_number, next_file_number and last_sequence never decrease:
///    "<field> decreases from <x> to <y>"
///  - every deleted file is in `base` at that level:
///    "VersionEdit deletes file <n> not at level <l>"
///  - every key is a well-formed internal key: "file <n> has a malformed internal key" and
///    "compact pointer at level <l> is not an internal key". This is a rule because comparing a
///    malformed key aborts, and an edit read from a MANIFEST must not be able to do that.
///  - a file number is below the resulting next_file_number:
///    "file <n> not below next_file_number <m>"
///  - no file number appears twice, at one level or two: "file <n> appears twice". A file that
///    moves down a level, deleted from one and added to the next under the same number, appears
///    once, so a trivial move is one edit.
///  - every file has smallest <= largest in internal-key order: "file <n> has smallest > largest"
///  - below level 0, user-key ranges are disjoint, so that two files that share even one user key
///    are refused: "files <a> and <b> overlap at level <l>"
///
/// `base` is expected to satisfy CheckInvariants(), as every state ApplyEdit returns does. A `base`
/// that does not is refused with the rule it breaks.
Status ApplyEdit(const VersionState& base, const VersionEdit& edit, VersionState* out);

/// Checks the rules ApplyEdit() holds its result to, on a whole state: the key, number, range and
/// overlap rules above, plus that level 0 is sorted by number ("files <a> and <b> out of order at
/// level 0") and the other levels by smallest key (the same message with their level). For tests
/// and the simulator, which build states by hand.
Status CheckInvariants(const VersionState& state);

/// Returns one edit that rebuilds `state` when applied to VersionState{}: it sets all three
/// counters and every compact pointer and adds every file. This is the first record of a new
/// MANIFEST. `state` must be one ApplyEdit() could have produced.
VersionEdit SnapshotEdit(const VersionState& state);

/// Returns the sum of the sizes of `files`, in bytes.
uint64_t TotalFileSize(const std::vector<FileMetaData>& files);

/// Returns the files at `level` whose user-key range intersects [begin_user_key, end_user_key],
/// both ends inclusive and either one unbounded when it is nullopt, in the order the state holds
/// them. A range that ends before it starts holds no file. At level 0, whose files overlap each
/// other, the range grows to take in every file that overlaps a file already in the result, until
/// no more do: a compaction of the result must not leave an older version of a key in a file
/// beside it. `level` must be in [0, kNumLevels). The state must satisfy CheckInvariants().
std::vector<FileMetaData> OverlappingFiles(const VersionState& state, int level,
                                           std::optional<std::string_view> begin_user_key,
                                           std::optional<std::string_view> end_user_key);

/// Returns the index of the first file in `files` whose largest key is at or after `internal_key`,
/// which is the one file that can hold it, or files.size() if there is none. `files` is the files
/// of one level below 0, sorted and disjoint, so the search is a binary one. `internal_key` must
/// be a well-formed internal key.
size_t FindFile(const std::vector<FileMetaData>& files, std::string_view internal_key);

/// Returns true if no level deeper than `level` has a file whose user-key range contains
/// `user_key`: nothing below can hold an older entry for it, so a compaction that writes into
/// `level` may drop a deletion marker for the key. `level` must be in [0, kNumLevels). The state
/// must satisfy CheckInvariants().
bool IsBaseLevelForKey(const VersionState& state, int level, std::string_view user_key);

}  // namespace shale

#endif  // SHALE_CORE_VERSION_STATE_H_
