#include "core/compaction_picker.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"
#include "shale/status.h"
#include "util/dbformat.h"

namespace shale {
namespace {

constexpr uint64_t kMiB = uint64_t{1} << 20;

std::string IKey(std::string_view user_key, SequenceNumber seq,
                 ValueType type = ValueType::kValue) {
  std::string result;
  AppendInternalKey(&result, {user_key, seq, type});
  return result;
}

// A file whose smallest key is (small_user, 100) and whose largest is (large_user, 1).
FileMetaData F(uint64_t n, std::string_view small_user, std::string_view large_user,
               uint64_t size = 100) {
  FileMetaData f;
  f.number = n;
  f.file_size = size;
  f.smallest = IKey(small_user, 100);
  f.largest = IKey(large_user, 1);
  return f;
}

// Builds a state from (level, file) pairs through ApplyEdit, so that a fixture that breaks a rule
// of the table set fails here and not as a puzzling plan.
VersionState StateWith(std::initializer_list<std::pair<int, FileMetaData>> files) {
  VersionEdit edit;
  edit.next_file_number = 1000;
  edit.new_files.assign(files.begin(), files.end());
  VersionState state;
  const Status s = ApplyEdit(VersionState{}, edit, &state);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return state;
}

using NumberList = std::vector<uint64_t>;

NumberList Numbers(const std::vector<FileMetaData>& files) {
  NumberList numbers;
  for (const FileMetaData& f : files) {
    numbers.push_back(f.number);
  }
  return numbers;
}

// The plan the picker chooses, which the test expects to exist.
CompactionPlan Picked(const VersionState& state, const CompactionOptions& options = {}) {
  const std::optional<CompactionPlan> plan = PickCompaction(state, options);
  EXPECT_TRUE(plan.has_value()) << "the picker found nothing to compact";
  return plan.value_or(CompactionPlan{});
}

// Six disjoint files of 2 MiB at level 1, [a,b] to [k,l], numbered 11 to 16: 12 MiB, over the 10
// MiB limit.
VersionState OversizedLevelOne() {
  return StateWith({{1, F(11, "a", "b", 2 * kMiB)},
                    {1, F(12, "c", "d", 2 * kMiB)},
                    {1, F(13, "e", "f", 2 * kMiB)},
                    {1, F(14, "g", "h", 2 * kMiB)},
                    {1, F(15, "i", "j", 2 * kMiB)},
                    {1, F(16, "k", "l", 2 * kMiB)}});
}

TEST(CompactionPickerTest, MaxBytesForLevel) {
  const CompactionOptions defaults;
  EXPECT_EQ(MaxBytesForLevel(1, defaults), 10 * kMiB);
  EXPECT_EQ(MaxBytesForLevel(2, defaults), 100 * kMiB);
  EXPECT_EQ(MaxBytesForLevel(3, defaults), 1000 * kMiB);

  // Not the defaults' arithmetic by accident: the base and the multiplier both matter.
  CompactionOptions options;
  options.max_bytes_for_level_base = 100;
  options.level_size_multiplier = 4;
  EXPECT_EQ(MaxBytesForLevel(1, options), 100u);
  EXPECT_EQ(MaxBytesForLevel(2, options), 400u);
  EXPECT_EQ(MaxBytesForLevel(3, options), 1600u);
  EXPECT_EQ(MaxBytesForLevel(kNumLevels - 1, options), 100u * 4 * 4 * 4 * 4 * 4);
}

TEST(CompactionPickerTest, MaxBytesForLevelSaturatesInsteadOfWrapping) {
  CompactionOptions options;
  options.max_bytes_for_level_base = UINT64_MAX / 2;
  options.level_size_multiplier = 10;
  EXPECT_EQ(MaxBytesForLevel(1, options), UINT64_MAX / 2);
  EXPECT_EQ(MaxBytesForLevel(2, options), UINT64_MAX);
  EXPECT_EQ(MaxBytesForLevel(kNumLevels - 1, options), UINT64_MAX);
}

TEST(CompactionPickerTest, LevelScore) {
  const CompactionOptions options;
  const VersionState three_l0 =
      StateWith({{0, F(1, "a", "b")}, {0, F(2, "c", "d")}, {0, F(3, "e", "f")}});
  EXPECT_DOUBLE_EQ(LevelScore(three_l0, 0, options), 0.75);

  const VersionState four_l0 = StateWith(
      {{0, F(1, "a", "b")}, {0, F(2, "c", "d")}, {0, F(3, "e", "f")}, {0, F(4, "g", "h")}});
  EXPECT_DOUBLE_EQ(LevelScore(four_l0, 0, options), 1.0);

  // Level 0 counts files, whatever their size: the trigger is about how many a read must look at.
  const VersionState big_l0 = StateWith({{0, F(1, "a", "b", 500 * kMiB)}});
  EXPECT_DOUBLE_EQ(LevelScore(big_l0, 0, options), 0.25);

  const VersionState l1 =
      StateWith({{1, F(11, "a", "b", 5 * kMiB)}, {1, F(12, "c", "d", 10 * kMiB)}});
  EXPECT_DOUBLE_EQ(LevelScore(l1, 1, options), 1.5);
  EXPECT_DOUBLE_EQ(LevelScore(l1, 2, options), 0.0);

  const VersionState l2 = StateWith({{2, F(21, "a", "b", 50 * kMiB)}});
  EXPECT_DOUBLE_EQ(LevelScore(l2, 2, options), 0.5);
}

TEST(CompactionPickerTest, NothingToDo) {
  const VersionState state = StateWith({{0, F(1, "a", "b")},
                                        {0, F(2, "c", "d")},
                                        {0, F(3, "e", "f")},
                                        {1, F(11, "a", "z", 9 * kMiB)},
                                        {2, F(21, "a", "m", 50 * kMiB)},
                                        {2, F(22, "n", "z", 49 * kMiB)}});
  EXPECT_FALSE(PickCompaction(state, CompactionOptions{}).has_value());
  EXPECT_FALSE(PickCompaction(VersionState{}, CompactionOptions{}).has_value());
}

TEST(CompactionPickerTest, Level0AtTrigger) {
  const VersionState state = StateWith({{0, F(1, "a", "c")},
                                        {0, F(2, "b", "d")},
                                        {0, F(3, "x", "y")},
                                        {0, F(4, "c", "e")},
                                        {1, F(11, "a", "b")},
                                        {1, F(12, "m", "n")}});
  const CompactionPlan plan = Picked(state);
  EXPECT_EQ(plan.level, 0);
  EXPECT_EQ(Numbers(plan.inputs[0]), (NumberList{1, 2, 4}));
  EXPECT_EQ(Numbers(plan.inputs[1]), (NumberList{11}));
  EXPECT_TRUE(plan.grandparents.empty());
  EXPECT_EQ(plan.next_compact_pointer, IKey("e", 1)) << "the largest key of file 4";
  EXPECT_FALSE(plan.manual);
}

TEST(CompactionPickerTest, Level0StartsFromTheOldestFile) {
  // Four files is the trigger, but the oldest file overlaps nothing, so it goes alone: the newer
  // files stay, and with them nothing older is left behind for their keys.
  const VersionState state = StateWith(
      {{0, F(1, "x", "y")}, {0, F(2, "a", "c")}, {0, F(3, "b", "d")}, {0, F(4, "a", "b")}});
  const CompactionPlan plan = Picked(state);
  EXPECT_EQ(plan.level, 0);
  EXPECT_EQ(Numbers(plan.inputs[0]), (NumberList{1}));
  EXPECT_TRUE(plan.inputs[1].empty());
}

TEST(CompactionPickerTest, Level0ExpandsTransitively) {
  // [a,b] and [d,f] do not touch, but [b,d] touches both and is newer than both: all three or an
  // old version of a key could be left above a newer one.
  const VersionState state = StateWith({{0, F(1, "a", "b")},
                                        {0, F(2, "d", "f")},
                                        {0, F(3, "b", "d")},
                                        {0, F(4, "x", "y")},
                                        {1, F(11, "e", "e")},
                                        {1, F(12, "g", "h")}});
  const CompactionPlan plan = Picked(state);
  EXPECT_EQ(plan.level, 0);
  EXPECT_EQ(Numbers(plan.inputs[0]), (NumberList{1, 2, 3}));
  EXPECT_EQ(Numbers(plan.inputs[1]), (NumberList{11}));
}

TEST(CompactionPickerTest, OversizedLevelUsesRoundRobinPointer) {
  VersionState state = OversizedLevelOne();
  ASSERT_TRUE(CheckInvariants(state).ok());

  // No pointer yet: the first file.
  CompactionPlan plan = Picked(state);
  EXPECT_EQ(plan.level, 1);
  EXPECT_EQ(Numbers(plan.inputs[0]), (NumberList{11}));
  EXPECT_EQ(plan.next_compact_pointer, IKey("b", 1));

  // The pointer is the largest key of the third file: the fourth is next.
  state.compact_pointers[1] = IKey("f", 1);
  plan = Picked(state);
  EXPECT_EQ(Numbers(plan.inputs[0]), (NumberList{14}));
  EXPECT_EQ(plan.next_compact_pointer, IKey("h", 1));

  // A pointer between two files, as one left by a file that has since been compacted away.
  state.compact_pointers[1] = IKey("fz", 7);
  EXPECT_EQ(Numbers(Picked(state).inputs[0]), (NumberList{14}));

  // The pointer is past the last file: back to the first.
  state.compact_pointers[1] = IKey("l", 1);
  EXPECT_EQ(Numbers(Picked(state).inputs[0]), (NumberList{11}));
  state.compact_pointers[1] = IKey("zzz", 1);
  EXPECT_EQ(Numbers(Picked(state).inputs[0]), (NumberList{11}));

  // A pointer inside the third file leaves that file's largest key still ahead of it.
  state.compact_pointers[1] = IKey("e", 50);
  EXPECT_EQ(Numbers(Picked(state).inputs[0]), (NumberList{13}));
}

TEST(CompactionPickerTest, RoundRobinPointerIsComparedAsAnInternalKey) {
  VersionState state = OversizedLevelOne();
  // The third file ends at ("f", 1). At one user key the newer entry sorts first, so ("f", 5) is
  // before it and the third file is not behind the pointer, while ("f", 0) is after it.
  state.compact_pointers[1] = IKey("f", 5);
  EXPECT_EQ(Numbers(Picked(state).inputs[0]), (NumberList{13}));
  state.compact_pointers[1] = IKey("f", 0);
  EXPECT_EQ(Numbers(Picked(state).inputs[0]), (NumberList{14}));
}

TEST(CompactionPickerTest, RoundRobinPicksTheNextLevelOverlap) {
  VersionState state = StateWith({{1, F(11, "a", "b", 2 * kMiB)},
                                  {1, F(12, "c", "d", 2 * kMiB)},
                                  {1, F(13, "e", "f", 2 * kMiB)},
                                  {1, F(14, "g", "h", 2 * kMiB)},
                                  {1, F(15, "i", "j", 2 * kMiB)},
                                  {1, F(16, "k", "l", 2 * kMiB)},
                                  {2, F(21, "g", "i")},
                                  {2, F(22, "m", "n")}});
  EXPECT_TRUE(Picked(state).inputs[1].empty());
  state.compact_pointers[1] = IKey("f", 1);
  const CompactionPlan plan = Picked(state);
  EXPECT_EQ(Numbers(plan.inputs[0]), (NumberList{14}));
  EXPECT_EQ(Numbers(plan.inputs[1]), (NumberList{21}));
}

TEST(CompactionPickerTest, HighestScoreWins) {
  // Level 0 is at 1.0 and level 1 at 1.2; level 2 is at 1.5, the highest.
  const VersionState state = StateWith({{0, F(1, "a", "b")},
                                        {0, F(2, "c", "d")},
                                        {0, F(3, "e", "f")},
                                        {0, F(4, "g", "h")},
                                        {1, F(11, "a", "z", 12 * kMiB)},
                                        {2, F(21, "a", "f", 75 * kMiB)},
                                        {2, F(22, "g", "z", 75 * kMiB)}});
  const CompactionPlan plan = Picked(state);
  EXPECT_EQ(plan.level, 2);
  EXPECT_EQ(Numbers(plan.inputs[0]), (NumberList{21}));
}

TEST(CompactionPickerTest, TiesGoToTheLowerLevel) {
  // Level 0 is at exactly 1.0, level 1 at exactly 1.0 and level 2 at exactly 1.0.
  const VersionState state = StateWith({{0, F(1, "a", "b")},
                                        {0, F(2, "c", "d")},
                                        {0, F(3, "e", "f")},
                                        {0, F(4, "g", "h")},
                                        {1, F(11, "a", "z", 10 * kMiB)},
                                        {2, F(21, "a", "z", 100 * kMiB)}});
  EXPECT_EQ(Picked(state).level, 0);

  const VersionState without_l0 =
      StateWith({{1, F(11, "a", "z", 10 * kMiB)}, {2, F(21, "a", "z", 100 * kMiB)}});
  EXPECT_EQ(Picked(without_l0).level, 1);
}

TEST(CompactionPickerTest, JustUnderTheLimitIsNotPicked) {
  const VersionState state = StateWith({{1, F(11, "a", "z", 10 * kMiB - 1)}});
  EXPECT_FALSE(PickCompaction(state, CompactionOptions{}).has_value());
}

TEST(CompactionPickerTest, GrandparentsCollected) {
  const VersionState state = StateWith({{1, F(11, "d", "h", 11 * kMiB)},
                                        {1, F(12, "p", "q")},
                                        {3, F(31, "a", "c")},
                                        {3, F(32, "e", "f")},
                                        {3, F(33, "g", "k")},
                                        {3, F(34, "m", "n")}});
  const CompactionPlan plan = Picked(state);
  EXPECT_EQ(plan.level, 1);
  EXPECT_EQ(Numbers(plan.inputs[0]), (NumberList{11}));
  EXPECT_TRUE(plan.inputs[1].empty());
  EXPECT_EQ(Numbers(plan.grandparents), (NumberList{32, 33}));
}

TEST(CompactionPickerTest, GrandparentsCoverTheWholeCompactionRange) {
  // The level 2 file widens the range from [d,h] to [b,h], and the grandparent that overlaps only
  // the widened part is part of the same output's footprint.
  const VersionState state = StateWith({{1, F(11, "d", "h", 11 * kMiB)},
                                        {2, F(21, "b", "e")},
                                        {3, F(31, "a", "b")},
                                        {3, F(32, "c", "c")},
                                        {3, F(33, "i", "j")}});
  const CompactionPlan plan = Picked(state);
  EXPECT_EQ(Numbers(plan.inputs[1]), (NumberList{21}));
  EXPECT_EQ(Numbers(plan.grandparents), (NumberList{31, 32}));
}

TEST(CompactionPickerTest, TrivialMove) {
  const CompactionOptions options;  // target_file_size 2 MiB, so 20 MiB of grandparents
  const VersionState alone = StateWith({{1, F(11, "d", "h", 11 * kMiB)},
                                        {2, F(21, "a", "b")},
                                        {2, F(22, "x", "z")},
                                        {3, F(31, "e", "f", 1 * kMiB)}});
  const CompactionPlan plan = Picked(alone, options);
  ASSERT_EQ(plan.level, 1);
  EXPECT_EQ(Numbers(plan.grandparents), (NumberList{31})) << "small, but not none";
  EXPECT_TRUE(plan.IsTrivialMove(options));

  // A level 2 file overlaps: the input has to be merged with it.
  const VersionState overlapped = StateWith(
      {{1, F(11, "d", "h", 11 * kMiB)}, {2, F(21, "g", "k")}, {3, F(31, "a", "c", 1 * kMiB)}});
  EXPECT_FALSE(Picked(overlapped, options).IsTrivialMove(options));

  // 30 MiB of grandparents: moving the file down would hand the next compaction too much to merge.
  const VersionState heavy = StateWith({{1, F(11, "d", "h", 11 * kMiB)},
                                        {3, F(31, "e", "f", 10 * kMiB)},
                                        {3, F(32, "g", "g", 10 * kMiB)},
                                        {3, F(33, "h", "k", 10 * kMiB)}});
  const CompactionPlan heavy_plan = Picked(heavy, options);
  EXPECT_EQ(TotalFileSize(heavy_plan.grandparents), 30 * kMiB);
  EXPECT_FALSE(heavy_plan.IsTrivialMove(options));
}

TEST(CompactionPickerTest, TrivialMoveGrandparentLimitIsTenTargetFiles) {
  CompactionOptions options;
  options.target_file_size = 2 * kMiB;
  CompactionPlan plan;
  plan.level = 1;
  plan.inputs[0] = {F(11, "d", "h")};

  plan.grandparents = {F(31, "e", "f", 20 * kMiB)};
  EXPECT_TRUE(plan.IsTrivialMove(options)) << "exactly 10 target files is within the limit";
  plan.grandparents = {F(31, "e", "f", 20 * kMiB + 1)};
  EXPECT_FALSE(plan.IsTrivialMove(options));

  // The limit follows the option, it is not a constant.
  options.target_file_size = 4 * kMiB;
  EXPECT_TRUE(plan.IsTrivialMove(options));
}

TEST(CompactionPickerTest, TrivialMoveNeedsExactlyOneInputFile) {
  const CompactionOptions options;
  CompactionPlan plan;
  EXPECT_FALSE(plan.IsTrivialMove(options)) << "no input at all";
  plan.inputs[0] = {F(1, "a", "b"), F(2, "b", "c")};
  EXPECT_FALSE(plan.IsTrivialMove(options)) << "two input files";
  plan.inputs[0] = {F(1, "a", "b")};
  EXPECT_TRUE(plan.IsTrivialMove(options));
  plan.inputs[1] = {F(11, "a", "a")};
  EXPECT_FALSE(plan.IsTrivialMove(options)) << "a file at the next level";
}

TEST(CompactionPickerTest, LastLevelIsNeverAnInputLevel) {
  const VersionState state =
      StateWith({{6, F(61, "a", "m", 2000000 * kMiB)}, {6, F(62, "n", "z", 2000000 * kMiB)}});
  EXPECT_DOUBLE_EQ(LevelScore(state, 6, CompactionOptions{}), 4.0);  // limit: 1000000 MiB
  EXPECT_FALSE(PickCompaction(state, CompactionOptions{}).has_value());
}

TEST(CompactionPickerTest, LevelBeforeTheLastCompactsIntoItWithoutGrandparents) {
  const VersionState state = StateWith({{5, F(51, "a", "m", 500000 * kMiB)},  // limit: 100000 MiB
                                        {6, F(61, "b", "c")},
                                        {6, F(62, "x", "z")}});
  const CompactionPlan plan = Picked(state);
  EXPECT_EQ(plan.level, 5);
  EXPECT_EQ(Numbers(plan.inputs[0]), (NumberList{51}));
  EXPECT_EQ(Numbers(plan.inputs[1]), (NumberList{61}));
  EXPECT_TRUE(plan.grandparents.empty()) << "there is no level 7";
}

TEST(CompactionPickerTest, RangeCompaction) {
  const VersionState state = StateWith({{1, F(11, "a", "b")},
                                        {1, F(12, "c", "d")},
                                        {1, F(13, "e", "f")},
                                        {1, F(14, "g", "h")},
                                        {2, F(21, "d", "e")},
                                        {2, F(22, "p", "q")},
                                        {3, F(31, "a", "z")}});
  const CompactionOptions options;
  const std::optional<CompactionPlan> plan = PickRangeCompaction(state, 1, "c", "f", options);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->level, 1);
  EXPECT_EQ(Numbers(plan->inputs[0]), (NumberList{12, 13}));
  EXPECT_EQ(Numbers(plan->inputs[1]), (NumberList{21})) << "the overlap of [c,f] is [d,e]";
  EXPECT_EQ(Numbers(plan->grandparents), (NumberList{31}));
  EXPECT_EQ(plan->next_compact_pointer, IKey("f", 1));
  EXPECT_TRUE(plan->manual);

  // Nothing at the level overlaps: nothing to do, even though other levels have files there.
  EXPECT_FALSE(PickRangeCompaction(state, 1, "i", "o", options).has_value());
  EXPECT_FALSE(PickRangeCompaction(state, 1, "x", "z", options).has_value());
  EXPECT_FALSE(PickRangeCompaction(state, 4, "a", "z", options).has_value());
  // A range that ends before it starts holds no file.
  EXPECT_FALSE(PickRangeCompaction(state, 1, "f", "c", options).has_value());
}

TEST(CompactionPickerTest, RangeCompactionBoundsAreInclusiveAndOptional) {
  const VersionState state = StateWith(
      {{1, F(11, "a", "b")}, {1, F(12, "c", "d")}, {1, F(13, "e", "f")}, {1, F(14, "g", "h")}});
  const CompactionOptions options;
  const auto numbers = [&](std::optional<std::string_view> begin,
                           std::optional<std::string_view> end) {
    const std::optional<CompactionPlan> plan = PickRangeCompaction(state, 1, begin, end, options);
    EXPECT_TRUE(plan.has_value());
    return plan ? Numbers(plan->inputs[0]) : NumberList{};
  };
  EXPECT_EQ(numbers("d", "e"), (NumberList{12, 13})) << "a file's last and first key count";
  EXPECT_EQ(numbers("b", "b"), (NumberList{11}));
  EXPECT_EQ(numbers(std::nullopt, "c"), (NumberList{11, 12}));
  EXPECT_EQ(numbers("f", std::nullopt), (NumberList{13, 14}));
  EXPECT_EQ(numbers(std::nullopt, std::nullopt), (NumberList{11, 12, 13, 14}));
}

TEST(CompactionPickerTest, RangeCompactionAtLevel0TakesEveryOverlappingFile) {
  // [c,c] touches only file 2, but file 2 overlaps files 1 and 3, which are older and newer.
  const VersionState state = StateWith({{0, F(1, "a", "c")},
                                        {0, F(2, "c", "e")},
                                        {0, F(3, "e", "g")},
                                        {0, F(4, "x", "y")},
                                        {1, F(11, "f", "f")}});
  const std::optional<CompactionPlan> plan =
      PickRangeCompaction(state, 0, "c", "c", CompactionOptions{});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->level, 0);
  EXPECT_EQ(Numbers(plan->inputs[0]), (NumberList{1, 2, 3}));
  EXPECT_EQ(Numbers(plan->inputs[1]), (NumberList{11}));
  EXPECT_TRUE(plan->manual);
}

TEST(CompactionPickerTest, RangeCompactionIsNotGatedByScore) {
  // Nothing is over any limit, and a manual compaction runs anyway.
  const VersionState state = StateWith({{1, F(11, "a", "b")}});
  EXPECT_FALSE(PickCompaction(state, CompactionOptions{}).has_value());
  EXPECT_TRUE(PickRangeCompaction(state, 1, "a", "a", CompactionOptions{}).has_value());
}

TEST(CompactionPickerTest, PureFunction) {
  VersionState state = StateWith({{0, F(1, "a", "c")},
                                  {0, F(2, "b", "d")},
                                  {0, F(3, "x", "y")},
                                  {0, F(4, "c", "e")},
                                  {1, F(11, "a", "b")},
                                  {1, F(12, "m", "n")},
                                  {2, F(21, "a", "z")}});
  state.compact_pointers[1] = IKey("b", 1);
  const VersionState before = state;
  const CompactionOptions options;

  const std::optional<CompactionPlan> first = PickCompaction(state, options);
  const std::optional<CompactionPlan> second = PickCompaction(state, options);
  const std::optional<CompactionPlan> from_copy = PickCompaction(before, options);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first, second);
  EXPECT_EQ(first, from_copy);
  EXPECT_EQ(state, before) << "the picker must not change the state";

  const std::optional<CompactionPlan> range_1 = PickRangeCompaction(state, 1, "a", "z", options);
  const std::optional<CompactionPlan> range_2 = PickRangeCompaction(state, 1, "a", "z", options);
  ASSERT_TRUE(range_1.has_value());
  EXPECT_EQ(range_1, range_2);
  EXPECT_EQ(state, before);
}

TEST(CompactionPickerDeathTest, PreconditionsAreChecked) {
  const VersionState state = StateWith({{1, F(11, "a", "b")}});
  const CompactionOptions options;
  EXPECT_DEATH(MaxBytesForLevel(0, options), "level");
  EXPECT_DEATH(MaxBytesForLevel(kNumLevels, options), "level");
  // The last level has no level below it to compact into.
  EXPECT_DEATH(PickRangeCompaction(state, kNumLevels - 1, "a", "z", options), "level");
  EXPECT_DEATH(PickRangeCompaction(state, -1, "a", "z", options), "level");
  CompactionOptions bad = options;
  bad.l0_compaction_trigger = 0;
  EXPECT_DEATH(LevelScore(state, 0, bad), "l0_compaction_trigger");
}

}  // namespace
}  // namespace shale
