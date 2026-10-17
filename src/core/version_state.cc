#include "core/version_state.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/dbformat.h"

namespace shale {

namespace {

size_t Index(int level) {
  SHALE_CHECK(level >= 0 && level < kNumLevels, "level is out of range");
  return static_cast<size_t>(level);
}

// The int a level index prints and travels as.
int LevelOf(size_t index) { return static_cast<int>(index); }

bool IsInternalKey(std::string_view key) {
  ParsedInternalKey unused;
  return ParseInternalKey(key, &unused);
}

Status LevelOutOfRange(int level) {
  return Status::Corruption("VersionEdit: level " + std::to_string(level) + " out of range");
}

// An edit can be built in memory as well as decoded, and a level the decoder would have refused
// must not index an array here.
Status CheckEditLevels(const VersionEdit& edit) {
  const auto in_range = [](int level) { return level >= 0 && level < kNumLevels; };
  for (const auto& entry : edit.compact_pointers) {
    if (!in_range(entry.first)) {
      return LevelOutOfRange(entry.first);
    }
  }
  for (const auto& entry : edit.deleted_files) {
    if (!in_range(entry.first)) {
      return LevelOutOfRange(entry.first);
    }
  }
  for (const auto& entry : edit.new_files) {
    if (!in_range(entry.first)) {
      return LevelOutOfRange(entry.first);
    }
  }
  return Status::OK();
}

Status CheckNotDecreasing(std::string_view field, uint64_t before,
                          const std::optional<uint64_t>& after) {
  if (after && *after < before) {
    return Status::Corruption(std::string(field) + " decreases from " + std::to_string(before) +
                              " to " + std::to_string(*after));
  }
  return Status::OK();
}

// Every key in the state parses. Run before anything compares keys, since comparing aborts on one
// that is shorter than its tag.
Status CheckKeys(const VersionState& state) {
  for (const auto& level : state.files) {
    for (const FileMetaData& f : level) {
      if (!IsInternalKey(f.smallest) || !IsInternalKey(f.largest)) {
        return Status::Corruption("file " + std::to_string(f.number) +
                                  " has a malformed internal key");
      }
    }
  }
  for (size_t level = 0; level < state.compact_pointers.size(); level++) {
    const std::string& pointer = state.compact_pointers[level];
    if (!pointer.empty() && !IsInternalKey(pointer)) {
      return Status::Corruption("compact pointer at level " + std::to_string(level) +
                                " is not an internal key");
    }
  }
  return Status::OK();
}

// The order a level keeps its files in: by number at level 0, by smallest key below it.
bool InLevelOrder(size_t level, const FileMetaData& a, const FileMetaData& b) {
  if (level == 0) {
    return a.number < b.number;
  }
  const int by_key = CompareInternalKey(a.smallest, b.smallest);
  return by_key != 0 ? by_key < 0 : a.number < b.number;
}

// Every rule except the form of the keys, which CheckKeys() has already seen to.
Status CheckStructure(const VersionState& state) {
  std::unordered_set<uint64_t> seen;
  for (const auto& level : state.files) {
    for (const FileMetaData& f : level) {
      if (f.number >= state.next_file_number) {
        return Status::Corruption("file " + std::to_string(f.number) +
                                  " not below next_file_number " +
                                  std::to_string(state.next_file_number));
      }
      if (!seen.insert(f.number).second) {
        return Status::Corruption("file " + std::to_string(f.number) + " appears twice");
      }
      if (CompareInternalKey(f.smallest, f.largest) > 0) {
        return Status::Corruption("file " + std::to_string(f.number) + " has smallest > largest");
      }
    }
  }
  for (size_t level = 0; level < state.files.size(); level++) {
    const std::vector<FileMetaData>& files = state.files[level];
    for (size_t i = 1; i < files.size(); i++) {
      const FileMetaData& before = files[i - 1];
      const FileMetaData& after = files[i];
      const std::string pair =
          "files " + std::to_string(before.number) + " and " + std::to_string(after.number);
      // Numbers are unique by now, so the order is strict at level 0.
      if (!InLevelOrder(level, before, after)) {
        return Status::Corruption(pair + " out of order at level " +
                                  std::to_string(LevelOf(level)));
      }
      // Sorted by smallest key, two files overlap if and only if neighbours do: a file that
      // reaches past a later one reaches the one between.
      if (level > 0 &&
          BytewiseCompare(ExtractUserKey(before.largest), ExtractUserKey(after.smallest)) >= 0) {
        return Status::Corruption(pair + " overlap at level " + std::to_string(LevelOf(level)));
      }
    }
  }
  return Status::OK();
}

}  // namespace

Status ApplyEdit(const VersionState& base, const VersionEdit& edit, VersionState* out) {
  SHALE_CHECK(out != nullptr, "ApplyEdit needs somewhere to put the new state");

  if (Status s = CheckEditLevels(edit); !s.ok()) {
    return s;
  }
  if (Status s = CheckNotDecreasing("log_number", base.log_number, edit.log_number); !s.ok()) {
    return s;
  }
  if (Status s =
          CheckNotDecreasing("next_file_number", base.next_file_number, edit.next_file_number);
      !s.ok()) {
    return s;
  }
  if (Status s = CheckNotDecreasing("last_sequence", base.last_sequence, edit.last_sequence);
      !s.ok()) {
    return s;
  }

  // Everything happens on a copy, so that a refusal at any point leaves `*out` alone, and so that
  // `out` may be `&base`.
  VersionState next = base;
  next.log_number = edit.log_number.value_or(base.log_number);
  next.next_file_number = edit.next_file_number.value_or(base.next_file_number);
  next.last_sequence = edit.last_sequence.value_or(base.last_sequence);
  for (const auto& [level, key] : edit.compact_pointers) {
    next.compact_pointers[Index(level)] = key;
  }

  // Deletions come first and are checked against `base`: a file the edit adds is not there to
  // delete, and one that moves down a level is deleted from the old level before it is added.
  std::array<bool, kNumLevels> touched{};
  for (const auto& deleted : edit.deleted_files) {
    const int level = deleted.first;
    const uint64_t number = deleted.second;
    std::vector<FileMetaData>& files = next.files[Index(level)];
    const auto it = std::find_if(files.begin(), files.end(),
                                 [number](const FileMetaData& f) { return f.number == number; });
    if (it == files.end()) {
      return Status::Corruption("VersionEdit deletes file " + std::to_string(number) +
                                " not at level " + std::to_string(level));
    }
    files.erase(it);
  }
  for (const auto& [level, file] : edit.new_files) {
    next.files[Index(level)].push_back(file);
    touched[Index(level)] = true;
  }

  if (Status s = CheckKeys(next); !s.ok()) {
    return s;
  }
  for (size_t level = 0; level < touched.size(); level++) {
    if (touched[level]) {
      std::sort(next.files[level].begin(), next.files[level].end(),
                [level](const FileMetaData& a, const FileMetaData& b) {
                  return InLevelOrder(level, a, b);
                });
    }
  }
  if (Status s = CheckStructure(next); !s.ok()) {
    return s;
  }
  *out = std::move(next);
  return Status::OK();
}

Status CheckInvariants(const VersionState& state) {
  if (Status s = CheckKeys(state); !s.ok()) {
    return s;
  }
  return CheckStructure(state);
}

VersionEdit SnapshotEdit(const VersionState& state) {
  VersionEdit edit;
  edit.log_number = state.log_number;
  edit.next_file_number = state.next_file_number;
  edit.last_sequence = state.last_sequence;
  for (size_t level = 0; level < state.files.size(); level++) {
    if (!state.compact_pointers[level].empty()) {
      edit.compact_pointers.emplace_back(LevelOf(level), state.compact_pointers[level]);
    }
    for (const FileMetaData& f : state.files[level]) {
      edit.new_files.emplace_back(LevelOf(level), f);
    }
  }
  return edit;
}

uint64_t TotalFileSize(const std::vector<FileMetaData>& files) {
  uint64_t total = 0;
  for (const FileMetaData& f : files) {
    total += f.file_size;
  }
  return total;
}

std::vector<FileMetaData> OverlappingFiles(const VersionState& state, int level,
                                           std::optional<std::string_view> begin_user_key,
                                           std::optional<std::string_view> end_user_key) {
  const std::vector<FileMetaData>& files = state.files[Index(level)];
  std::optional<std::string_view> begin = begin_user_key;
  std::optional<std::string_view> end = end_user_key;
  if (begin && end && BytewiseCompare(*begin, *end) > 0) {
    return {};
  }

  // The views point into the files of `state`, which outlives this call, so the range can be
  // widened to a file's own keys without copying them.
  std::vector<bool> picked(files.size(), false);
  bool widened = true;
  while (widened) {
    widened = false;
    for (size_t i = 0; i < files.size(); i++) {
      if (picked[i]) {
        continue;
      }
      const std::string_view smallest = ExtractUserKey(files[i].smallest);
      const std::string_view largest = ExtractUserKey(files[i].largest);
      if ((end && BytewiseCompare(smallest, *end) > 0) ||
          (begin && BytewiseCompare(largest, *begin) < 0)) {
        continue;
      }
      picked[i] = true;
      if (level != 0) {
        continue;
      }
      // Level 0 files overlap each other, so one that is taken may reach beyond the range, and
      // then the files in that stretch are taken too. A pass that widened the range goes again,
      // because a file passed over earlier may now be in reach.
      if (begin && BytewiseCompare(smallest, *begin) < 0) {
        begin = smallest;
        widened = true;
      }
      if (end && BytewiseCompare(largest, *end) > 0) {
        end = largest;
        widened = true;
      }
    }
  }

  std::vector<FileMetaData> result;
  for (size_t i = 0; i < files.size(); i++) {
    if (picked[i]) {
      result.push_back(files[i]);
    }
  }
  return result;
}

size_t FindFile(const std::vector<FileMetaData>& files, std::string_view internal_key) {
  const auto it = std::partition_point(files.begin(), files.end(), [&](const FileMetaData& f) {
    return CompareInternalKey(f.largest, internal_key) < 0;
  });
  return static_cast<size_t>(it - files.begin());
}

bool IsBaseLevelForKey(const VersionState& state, int level, std::string_view user_key) {
  for (size_t deeper = Index(level) + 1; deeper < state.files.size(); deeper++) {
    const std::vector<FileMetaData>& files = state.files[deeper];
    // Below level 0 the files are disjoint and sorted, so the first one that ends at or after the
    // key is the only one that can contain it.
    const auto it = std::partition_point(files.begin(), files.end(), [&](const FileMetaData& f) {
      return BytewiseCompare(ExtractUserKey(f.largest), user_key) < 0;
    });
    if (it != files.end() && BytewiseCompare(ExtractUserKey(it->smallest), user_key) <= 0) {
      return false;
    }
  }
  return true;
}

}  // namespace shale
