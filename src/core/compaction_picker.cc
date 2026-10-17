#include "core/compaction_picker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"
#include "util/check.h"
#include "util/dbformat.h"

namespace shale {

namespace {

size_t Index(int level) {
  SHALE_CHECK(level >= 0 && level < kNumLevels, "level is out of range");
  return static_cast<size_t>(level);
}

uint64_t SaturatingMultiply(uint64_t a, uint64_t b) {
  if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
    return std::numeric_limits<uint64_t>::max();
  }
  return a * b;
}

// The smallest and largest user key in a set of files, as views into the files' own keys: the
// files must outlive the range, and must not move while it is in use.
class UserKeyRange {
 public:
  void Add(const std::vector<FileMetaData>& files) {
    for (const FileMetaData& f : files) {
      const std::string_view smallest = ExtractUserKey(f.smallest);
      const std::string_view largest = ExtractUserKey(f.largest);
      if (!smallest_ || BytewiseCompare(smallest, *smallest_) < 0) {
        smallest_ = smallest;
      }
      if (!largest_ || BytewiseCompare(largest, *largest_) > 0) {
        largest_ = largest;
      }
    }
  }

  // Both are nullopt until a file has been added.
  std::optional<std::string_view> smallest() const { return smallest_; }
  std::optional<std::string_view> largest() const { return largest_; }

 private:
  std::optional<std::string_view> smallest_;
  std::optional<std::string_view> largest_;
};

// Fills in what follows from the files taken at `level`: the overlap at level + 1, the
// grandparents, and where the level's pointer should move to. `inputs` must not be empty.
CompactionPlan BuildPlan(const VersionState& state, int level, std::vector<FileMetaData> inputs,
                         bool manual) {
  SHALE_CHECK(!inputs.empty(), "a compaction needs at least one input file");
  CompactionPlan plan;
  plan.level = level;
  plan.manual = manual;
  plan.inputs[0] = std::move(inputs);

  // The ranges view into `plan`'s files, which stay where they are until the plan is returned.
  UserKeyRange range;
  range.Add(plan.inputs[0]);
  plan.inputs[1] = OverlappingFiles(state, level + 1, range.smallest(), range.largest());
  range.Add(plan.inputs[1]);
  // The last level a compaction can write to has no level below it, so no grandparents.
  if (level + 2 < kNumLevels) {
    plan.grandparents = OverlappingFiles(state, level + 2, range.smallest(), range.largest());
  }

  const std::string* largest = &plan.inputs[0].front().largest;
  for (const FileMetaData& f : plan.inputs[0]) {
    if (CompareInternalKey(f.largest, *largest) > 0) {
      largest = &f.largest;
    }
  }
  plan.next_compact_pointer = *largest;
  return plan;
}

// The file of a level below 0 that the level's compact pointer says is next. The level is not
// empty.
const FileMetaData& NextFileAfterPointer(const std::vector<FileMetaData>& files,
                                         const std::string& pointer) {
  if (pointer.empty()) {
    return files.front();
  }
  // The files are sorted and disjoint, so those whose largest key is not after the pointer come
  // first.
  const auto it = std::partition_point(files.begin(), files.end(), [&](const FileMetaData& f) {
    return CompareInternalKey(f.largest, pointer) <= 0;
  });
  return it == files.end() ? files.front() : *it;
}

}  // namespace

uint64_t MaxGrandparentOverlapBytes(uint64_t target_file_size) {
  return SaturatingMultiply(kMaxGrandparentOverlapTargets, target_file_size);
}

bool CompactionPlan::IsTrivialMove(const CompactionOptions& options) const {
  // A manual compaction is asked for to rewrite files, to drop what they hold that is dead, and a
  // move down a level rewrites nothing.
  return !manual && inputs[0].size() == 1 && inputs[1].empty() &&
         TotalFileSize(grandparents) <= MaxGrandparentOverlapBytes(options.target_file_size);
}

uint64_t MaxBytesForLevel(int level, const CompactionOptions& options) {
  SHALE_CHECK(
      level >= 1 && level < kNumLevels,
      "MaxBytesForLevel: level 0 is limited by file count, and there is no level past the last");
  SHALE_CHECK(options.max_bytes_for_level_base > 0, "max_bytes_for_level_base must be positive");
  SHALE_CHECK(options.level_size_multiplier > 0, "level_size_multiplier must be positive");
  uint64_t limit = options.max_bytes_for_level_base;
  for (int i = 1; i < level; i++) {
    limit = SaturatingMultiply(limit, static_cast<uint64_t>(options.level_size_multiplier));
  }
  return limit;
}

double LevelScore(const VersionState& state, int level, const CompactionOptions& options) {
  const std::vector<FileMetaData>& files = state.files[Index(level)];
  if (level == 0) {
    SHALE_CHECK(options.l0_compaction_trigger > 0, "l0_compaction_trigger must be positive");
    return static_cast<double>(files.size()) / static_cast<double>(options.l0_compaction_trigger);
  }
  return static_cast<double>(TotalFileSize(files)) /
         static_cast<double>(MaxBytesForLevel(level, options));
}

std::optional<CompactionPlan> PickCompaction(const VersionState& state,
                                             const CompactionOptions& options) {
  // Strictly greater than the best so far, so that of two levels with one score the lower stays.
  // The last level is left out: its files have nowhere to go.
  int level = -1;
  double best_score = 0;
  for (int candidate = 0; candidate < kNumLevels - 1; candidate++) {
    const double score = LevelScore(state, candidate, options);
    if (score >= 1.0 && score > best_score) {
      level = candidate;
      best_score = score;
    }
  }
  if (level < 0) {
    return std::nullopt;
  }

  // A score of 1.0 or more means the level holds at least one file: it has a trigger's worth of
  // files at level 0, and at least the limit's worth of bytes, which is positive, below it.
  const std::vector<FileMetaData>& files = state.files[Index(level)];
  if (level == 0) {
    const FileMetaData& oldest = files.front();
    return BuildPlan(
        state, 0,
        OverlappingFiles(state, 0, ExtractUserKey(oldest.smallest), ExtractUserKey(oldest.largest)),
        /*manual=*/false);
  }
  return BuildPlan(state, level,
                   {NextFileAfterPointer(files, state.compact_pointers[Index(level)])},
                   /*manual=*/false);
}

std::optional<CompactionPlan> PickRangeCompaction(const VersionState& state, int level,
                                                  std::optional<std::string_view> begin_user_key,
                                                  std::optional<std::string_view> end_user_key,
                                                  const CompactionOptions& /*options*/) {
  SHALE_CHECK(level >= 0 && level < kNumLevels - 1,
              "PickRangeCompaction: the last level has no level below it to compact into");
  std::vector<FileMetaData> inputs = OverlappingFiles(state, level, begin_user_key, end_user_key);
  if (inputs.empty()) {
    return std::nullopt;
  }
  return BuildPlan(state, level, std::move(inputs), /*manual=*/true);
}

}  // namespace shale
