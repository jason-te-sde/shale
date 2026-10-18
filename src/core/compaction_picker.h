#ifndef SHALE_CORE_COMPACTION_PICKER_H_
#define SHALE_CORE_COMPACTION_PICKER_H_

// Which files the next compaction should merge, chosen from the table set alone.
//
// The picker reads a VersionState and the size limits and returns a plan: the files to read, from
// one level and the level below it, and the files two levels down that the output will have to sit
// beside. Running the plan, and recording its result as a VersionEdit, belong to the database. The
// same state and options always give the same plan, so a simulator can replay a decision and a test
// can state it exactly.
//
// This is the pure kernel's: no I/O, no clock, no threads, no locks.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"

namespace shale {

/// The size limits that decide when a level is too big and what a compaction may move whole.
struct CompactionOptions {
  /// The number of level 0 files at which level 0 is due for compaction. Must be positive.
  int l0_compaction_trigger = 4;
  /// The size limit of level 1, in bytes. Must be positive.
  uint64_t max_bytes_for_level_base = 10 << 20;
  /// How many times larger each level's limit is than the one above it. Must be positive.
  int level_size_multiplier = 10;
  /// The size a compaction aims for in each file it writes, in bytes. It sets how much overlap
  /// with the level below a file may have and still move there without being rewritten.
  uint64_t target_file_size = 2 << 20;
};

/// How many target-sized files of the level below its output's a compaction's output may overlap,
/// and how many a file may have as grandparents and still be moved down whole, before it is too
/// much for the next compaction of that level to merge at once. The bound LevelDB uses.
inline constexpr uint64_t kMaxGrandparentOverlapTargets = 10;

/// Returns the most bytes of grandparent files, those two levels below a compaction's input, that
/// an output of the compaction may overlap, and that a file may have and still be moved down whole:
/// kMaxGrandparentOverlapTargets times `target_file_size`, or UINT64_MAX if that is too big for 64
/// bits. The picker's trivial move and the job that writes the outputs both use it, so that the two
/// cannot disagree about how much overlap is too much.
/// @param target_file_size the size a compaction aims for in each file it writes, in bytes.
uint64_t MaxGrandparentOverlapBytes(uint64_t target_file_size);

/// One compaction: what to read and where the result goes.
struct CompactionPlan {
  /// The inputs come from this level and the one below it; the outputs go to level + 1.
  int level = 0;
  /// The input files: [0] at `level`, [1] at level + 1. Each is in the order the state holds it.
  std::array<std::vector<FileMetaData>, 2> inputs;
  /// The files at level + 2 whose user-key range overlaps the range of all the inputs, or none
  /// when there is no level + 2. The output of the compaction must not overlap too many of them,
  /// or the next compaction one level down would have to rewrite all of them at once.
  std::vector<FileMetaData> grandparents;
  /// The largest internal key among inputs[0]: where the next compaction of `level` should start.
  /// The caller records it as the level's compact pointer.
  std::string next_compact_pointer;
  /// True for a plan from PickRangeCompaction(), which a caller asked for, false for one the
  /// picker chose because a level was too big.
  bool manual = false;

  /// Returns true if the one input file can move to level + 1 as it is, without being read: the
  /// plan is not manual, the file is the only input at `level`, no file at level + 1 overlaps it,
  /// and its grandparents total no more than ten times options.target_file_size, because a file
  /// moved down that overlaps more would hand the next compaction too much to merge at once. A
  /// manual plan is never a move: it is asked for to rewrite files and give back what they hold
  /// that is dead, which a move does not do.
  bool IsTrivialMove(const CompactionOptions& options) const;

  /// Two plans are equal when every field is.
  friend bool operator==(const CompactionPlan&, const CompactionPlan&) = default;
};

/// Returns the size limit of `level`, in bytes: max_bytes_for_level_base times
/// level_size_multiplier to the power level - 1. A limit too big for 64 bits is UINT64_MAX, which
/// nothing reaches. `level` must be in [1, kNumLevels): level 0 is limited by
/// its file count, not its size. The options must be positive, as documented on them.
uint64_t MaxBytesForLevel(int level, const CompactionOptions& options);

/// Returns how due `level` is for compaction: 1.0 or more means it is. At level 0 it is the number
/// of files over l0_compaction_trigger, since what level 0 costs a read is a lookup in every file.
/// Below it, it is the level's bytes over MaxBytesForLevel(). `level` must be in
/// [0, kNumLevels).
double LevelScore(const VersionState& state, int level, const CompactionOptions& options);

/// Returns the compaction to run next, or nullopt if no level is due.
///
/// The level is the one with the highest LevelScore() that is at least 1, the lower of two that
/// tie. The last level is never chosen: it has nowhere to compact into. Which files of the level go
/// depends on it:
///  - Level 0 starts from its oldest file and takes every file that overlaps the ones taken, until
///    none does, since leaving behind an older version of a key beside a newer one in level 1
///    would make a read return the old one.
///  - Any other level takes one file, the first whose largest key is after the level's compact
///    pointer in CompareInternalKey() order, or the first file of the level if none is or the
///    pointer is empty. So successive compactions of a level walk it from one end to the other
///    instead of returning to the same file.
/// inputs[1] is then every file at level + 1 that overlaps the user-key range of inputs[0], and
/// the grandparents are the files at level + 2 that overlap the range of both. The state must
/// satisfy CheckInvariants() and the options must be positive.
std::optional<CompactionPlan> PickCompaction(const VersionState& state,
                                             const CompactionOptions& options);

/// Returns the compaction of the files at `level` whose user-key range intersects
/// [begin_user_key, end_user_key], both ends inclusive and either one unbounded when it is nullopt,
/// together with the files they overlap at level + 1 and the grandparents, or nullopt if no file
/// at `level` intersects the range. At level 0 the files taken grow as OverlappingFiles() grows
/// them. The plan is marked manual and is chosen whatever the levels' sizes. `level` must be in
/// [0, kNumLevels - 1), since the last level has nowhere to compact into. `options` is not
/// consulted: it is taken so that the two pickers read alike.
std::optional<CompactionPlan> PickRangeCompaction(const VersionState& state, int level,
                                                  std::optional<std::string_view> begin_user_key,
                                                  std::optional<std::string_view> end_user_key,
                                                  const CompactionOptions& options);

}  // namespace shale

#endif  // SHALE_CORE_COMPACTION_PICKER_H_
