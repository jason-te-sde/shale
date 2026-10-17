#include "core/version_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "shale/status.h"
#include "util/coding.h"
#include "util/dbformat.h"
#include "util/random.h"

namespace shale {
namespace {

constexpr size_t kLevels = static_cast<size_t>(kNumLevels);

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

// An edit that adds `files`, and raises next_file_number so that they are below it.
VersionEdit EditAdding(std::initializer_list<std::pair<int, FileMetaData>> files,
                       uint64_t next_file_number = 100) {
  VersionEdit e;
  e.next_file_number = next_file_number;
  e.new_files.assign(files.begin(), files.end());
  return e;
}

std::vector<uint64_t> Numbers(const std::vector<FileMetaData>& files) {
  std::vector<uint64_t> numbers;
  for (const FileMetaData& f : files) {
    numbers.push_back(f.number);
  }
  return numbers;
}

using NumberList = std::vector<uint64_t>;

// Applies `e` to `base` and expects it to be accepted.
VersionState Applied(const VersionState& base, const VersionEdit& e) {
  VersionState out;
  const Status s = ApplyEdit(base, e, &out);
  EXPECT_TRUE(s.ok()) << s.ToString() << "\n" << e.DebugString();
  return out;
}

// A state that no accepted edit produces, so that a rejected edit that wrote to it shows.
VersionState Sentinel() {
  VersionState s;
  s.log_number = 77;
  s.next_file_number = 88;
  s.last_sequence = 99;
  s.compact_pointers[2] = IKey("sentinel", 1);
  s.files[3].push_back(F(5, "s", "t"));
  return s;
}

// Applies `e` to `base`, expects Corruption and that nothing was written to the output, and returns
// the status text.
std::string ApplyError(const VersionState& base, const VersionEdit& e) {
  VersionState out = Sentinel();
  const Status s = ApplyEdit(base, e, &out);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString() << "\n" << e.DebugString();
  EXPECT_EQ(out, Sentinel()) << "a rejected edit changed the output";
  return s.ToString();
}

TEST(VersionStateTest, DefaultStateIsEmptyAndValid) {
  const VersionState s;
  EXPECT_EQ(s.log_number, 0u);
  EXPECT_EQ(s.next_file_number, 2u);
  EXPECT_EQ(s.last_sequence, 0u);
  for (size_t level = 0; level < kLevels; level++) {
    EXPECT_TRUE(s.files[level].empty());
    EXPECT_TRUE(s.compact_pointers[level].empty());
  }
  const Status checked = CheckInvariants(s);
  EXPECT_TRUE(checked.ok()) << checked.ToString();
}

TEST(VersionStateTest, AddSortsLevels) {
  const VersionState s = Applied(
      VersionState{},
      EditAdding(
          {{0, F(9, "a", "b")}, {0, F(5, "c", "d")}, {1, F(11, "m", "p")}, {1, F(12, "a", "c")}},
          20));
  EXPECT_EQ(Numbers(s.files[0]), (NumberList{5, 9}));
  EXPECT_EQ(Numbers(s.files[1]), (NumberList{12, 11}));  // [a,c] is #12 and comes before [m,p]
  EXPECT_EQ(s.next_file_number, 20u);

  // New files go between and around the ones already there.
  const VersionState t =
      Applied(s, EditAdding({{0, F(7, "a", "b")}, {1, F(13, "e", "g")}, {1, F(14, "x", "z")}}, 30));
  EXPECT_EQ(Numbers(t.files[0]), (NumberList{5, 7, 9}));
  EXPECT_EQ(Numbers(t.files[1]), (NumberList{12, 13, 11, 14}));
}

TEST(VersionStateTest, EmptyEditChangesNothing) {
  const VersionState base =
      Applied(VersionState{}, EditAdding({{0, F(5, "a", "b")}, {2, F(6, "c", "d")}}));
  EXPECT_EQ(Applied(base, VersionEdit{}), base);
}

TEST(VersionStateTest, DeleteRemovesTheFile) {
  const VersionState base = Applied(
      VersionState{}, EditAdding({{0, F(5, "a", "b")}, {1, F(6, "c", "d")}, {1, F(7, "e", "f")}}));
  VersionEdit e;
  e.deleted_files = {{0, 5}, {1, 7}};
  const VersionState s = Applied(base, e);
  EXPECT_TRUE(s.files[0].empty());
  EXPECT_EQ(Numbers(s.files[1]), (NumberList{6}));
}

TEST(VersionStateTest, DeleteMissingFileIsRejected) {
  const VersionState base = Applied(VersionState{}, EditAdding({{1, F(5, "a", "c")}}));

  VersionEdit e;
  e.deleted_files = {{1, 9}};
  EXPECT_EQ(ApplyError(base, e), "Corruption: VersionEdit deletes file 9 not at level 1");

  // The right number at the wrong level.
  e.deleted_files = {{2, 5}};
  EXPECT_EQ(ApplyError(base, e), "Corruption: VersionEdit deletes file 5 not at level 2");

  // A file the same edit adds is not in the state yet, so it cannot be deleted by that edit.
  VersionEdit both = EditAdding({{1, F(6, "x", "z")}});
  both.deleted_files = {{1, 6}};
  EXPECT_EQ(ApplyError(base, both), "Corruption: VersionEdit deletes file 6 not at level 1");
}

TEST(VersionStateTest, MovingAFileDownIsOneEdit) {
  // A trivial move: the file leaves one level and joins the next under the same number. The number
  // appears once in the result, so it is not a duplicate.
  const VersionState base = Applied(VersionState{}, EditAdding({{1, F(5, "a", "c")}}));
  VersionEdit move;
  move.deleted_files = {{1, 5}};
  move.new_files = {{2, F(5, "a", "c")}};
  const VersionState s = Applied(base, move);
  EXPECT_TRUE(s.files[1].empty());
  EXPECT_EQ(Numbers(s.files[2]), (NumberList{5}));
}

TEST(VersionStateTest, OverlapAtLevel1IsRejected) {
  // [a,c] and [c,d] share the user key c, whatever the sequence numbers say.
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{1, F(1, "a", "c")}, {1, F(2, "c", "d")}})),
            "Corruption: files 1 and 2 overlap at level 1");
  // Added the other way round, the files are named in the order they sort.
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{1, F(2, "c", "d")}, {1, F(1, "a", "c")}})),
            "Corruption: files 1 and 2 overlap at level 1");
  // One range inside another.
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{3, F(1, "a", "z")}, {3, F(2, "m", "n")}})),
            "Corruption: files 1 and 2 overlap at level 3");
  // The overlap may be with a file the state already holds.
  const VersionState base = Applied(VersionState{}, EditAdding({{2, F(1, "e", "g")}}));
  EXPECT_EQ(ApplyError(base, EditAdding({{2, F(2, "g", "h")}})),
            "Corruption: files 1 and 2 overlap at level 2");
}

TEST(VersionStateTest, TouchingButDisjointRangesAreAccepted) {
  // "c" and "c\0" are different user keys, and "c" sorts first: nothing is shared.
  const VersionState s = Applied(VersionState{}, EditAdding({{1, F(1, "a", "c")},
                                                             {1, F(2, std::string("c\0", 2), "d")},
                                                             {1, F(3, "e", "f")}}));
  EXPECT_EQ(Numbers(s.files[1]), (NumberList{1, 2, 3}));
}

TEST(VersionStateTest, OverlapAtLevel0IsAllowed) {
  const VersionState s = Applied(
      VersionState{}, EditAdding({{0, F(1, "a", "m")}, {0, F(2, "c", "z")}, {0, F(3, "a", "z")}}));
  EXPECT_EQ(Numbers(s.files[0]), (NumberList{1, 2, 3}));
}

TEST(VersionStateTest, DuplicateNumberIsRejected) {
  // The same number at two levels.
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{0, F(5, "a", "b")}, {2, F(5, "c", "d")}})),
            "Corruption: file 5 appears twice");
  // At one level, with ranges that do not overlap.
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{1, F(5, "a", "b")}, {1, F(5, "c", "d")}})),
            "Corruption: file 5 appears twice");
  // A new file with the number of one the state still holds.
  const VersionState base = Applied(VersionState{}, EditAdding({{1, F(5, "a", "c")}}));
  EXPECT_EQ(ApplyError(base, EditAdding({{3, F(5, "x", "z")}})),
            "Corruption: file 5 appears twice");
}

TEST(VersionStateTest, NumberAtOrAboveNextFileIsRejected) {
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{0, F(5, "a", "b")}}, 5)),
            "Corruption: file 5 not below next_file_number 5");
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{0, F(6, "a", "b")}}, 5)),
            "Corruption: file 6 not below next_file_number 5");
  const VersionState below = Applied(VersionState{}, EditAdding({{0, F(4, "a", "b")}}, 5));
  EXPECT_EQ(Numbers(below.files[0]), (NumberList{4}));

  // An edit that leaves next_file_number alone is held to the one in the state: 2 to start with.
  VersionEdit e;
  e.new_files = {{0, F(2, "a", "b")}};
  EXPECT_EQ(ApplyError(VersionState{}, e), "Corruption: file 2 not below next_file_number 2");
}

TEST(VersionStateTest, SmallestAboveLargestIsRejected) {
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{0, F(3, "z", "a")}})),
            "Corruption: file 3 has smallest > largest");
  // The same user key, with the newer entry as the largest: internal keys order the newer entry
  // first, so this file has its range backwards.
  FileMetaData backwards = F(3, "k", "k");
  backwards.smallest = IKey("k", 1);
  backwards.largest = IKey("k", 100);
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{0, backwards}})),
            "Corruption: file 3 has smallest > largest");

  // A file of one key, and a file whose two keys are identical, are fine.
  FileMetaData single = F(4, "k", "k");
  FileMetaData same = F(5, "m", "m");
  same.largest = same.smallest;
  const VersionState s = Applied(VersionState{}, EditAdding({{0, single}, {1, same}}));
  EXPECT_EQ(Numbers(s.files[0]), (NumberList{4}));
  EXPECT_EQ(Numbers(s.files[1]), (NumberList{5}));
}

TEST(VersionStateTest, CountersNeverDecrease) {
  VersionEdit first;
  first.log_number = 10;
  first.next_file_number = 20;
  first.last_sequence = 30;
  const VersionState base = Applied(VersionState{}, first);
  EXPECT_EQ(base.log_number, 10u);
  EXPECT_EQ(base.next_file_number, 20u);
  EXPECT_EQ(base.last_sequence, 30u);

  VersionEdit e;
  e.log_number = 9;
  EXPECT_EQ(ApplyError(base, e), "Corruption: log_number decreases from 10 to 9");
  e = VersionEdit{};
  e.next_file_number = 19;
  EXPECT_EQ(ApplyError(base, e), "Corruption: next_file_number decreases from 20 to 19");
  e = VersionEdit{};
  e.last_sequence = 29;
  EXPECT_EQ(ApplyError(base, e), "Corruption: last_sequence decreases from 30 to 29");

  // Staying where they are is not a decrease, and a counter the edit leaves out stays put.
  e = VersionEdit{};
  e.log_number = 10;
  e.last_sequence = 31;
  const VersionState moved = Applied(base, e);
  EXPECT_EQ(moved.log_number, 10u);
  EXPECT_EQ(moved.next_file_number, 20u);
  EXPECT_EQ(moved.last_sequence, 31u);
}

TEST(VersionStateTest, CompactPointersAreSetPerLevel) {
  VersionEdit first;
  first.compact_pointers = {{1, IKey("m", 5)}, {3, IKey("q", 6)}};
  const VersionState base = Applied(VersionState{}, first);
  EXPECT_EQ(base.compact_pointers[1], IKey("m", 5));
  EXPECT_EQ(base.compact_pointers[3], IKey("q", 6));
  EXPECT_TRUE(base.compact_pointers[2].empty());

  // A level the edit does not name keeps its pointer; a level it names twice takes the later one.
  VersionEdit second;
  second.compact_pointers = {{1, IKey("a", 1)}, {1, IKey("b", 2)}};
  const VersionState moved = Applied(base, second);
  EXPECT_EQ(moved.compact_pointers[1], IKey("b", 2));
  EXPECT_EQ(moved.compact_pointers[3], IKey("q", 6));
}

TEST(VersionStateTest, MalformedKeysAreRefusedNotAborted) {
  // Comparing keys aborts on one shorter than its tag, and these edits can come out of a file.
  FileMetaData short_smallest = F(3, "a", "b");
  short_smallest.smallest = "abc";
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{1, short_smallest}})),
            "Corruption: file 3 has a malformed internal key");

  FileMetaData short_largest = F(3, "a", "b");
  short_largest.largest.clear();
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{0, short_largest}})),
            "Corruption: file 3 has a malformed internal key");

  // A key long enough, with a type that does not exist.
  FileMetaData bad_type = F(3, "a", "b");
  bad_type.largest[bad_type.largest.size() - kInternalKeyTagSize] = '\x07';
  EXPECT_EQ(ApplyError(VersionState{}, EditAdding({{2, bad_type}})),
            "Corruption: file 3 has a malformed internal key");

  VersionEdit pointer;
  pointer.compact_pointers = {{1, "abc"}};
  EXPECT_EQ(ApplyError(VersionState{}, pointer),
            "Corruption: compact pointer at level 1 is not an internal key");

  // "No pointer" is the empty string, and an edit may say so.
  pointer.compact_pointers = {{1, ""}};
  EXPECT_EQ(Applied(VersionState{}, pointer), VersionState{});
}

TEST(VersionStateTest, LevelOutsideTheRangeIsRefusedNotIndexed) {
  for (const int level : {-1, kNumLevels, 1000}) {
    const std::string want =
        "Corruption: VersionEdit: level " + std::to_string(level) + " out of range";
    VersionEdit added = EditAdding({{level, F(3, "a", "b")}});
    EXPECT_EQ(ApplyError(VersionState{}, added), want);
    VersionEdit deleted;
    deleted.deleted_files = {{level, 3}};
    EXPECT_EQ(ApplyError(VersionState{}, deleted), want);
    VersionEdit pointer;
    pointer.compact_pointers = {{level, IKey("a", 1)}};
    EXPECT_EQ(ApplyError(VersionState{}, pointer), want);
  }
}

TEST(VersionStateTest, OutputMayBeTheBase) {
  VersionState s = Applied(VersionState{}, EditAdding({{1, F(1, "a", "c")}}));
  const VersionState expected = Applied(s, EditAdding({{1, F(2, "e", "g")}}));
  ASSERT_TRUE(ApplyEdit(s, EditAdding({{1, F(2, "e", "g")}}), &s).ok());
  EXPECT_EQ(s, expected);

  // A refusal leaves it as it was, too.
  const VersionState before = s;
  const Status refused = ApplyEdit(s, EditAdding({{1, F(3, "f", "h")}}), &s);
  EXPECT_TRUE(refused.IsCorruption()) << refused.ToString();
  EXPECT_EQ(s, before);
}

TEST(VersionStateTest, BaseIsNotModified) {
  const VersionState base = Applied(VersionState{}, EditAdding({{1, F(1, "a", "c")}}));
  const VersionState copy = base;
  VersionEdit e = EditAdding({{1, F(2, "e", "g")}});
  e.deleted_files = {{1, 1}};
  (void)Applied(base, e);
  EXPECT_EQ(base, copy);
}

// A state with files at several levels and every counter set, in valid form.
VersionState SmallValidState() {
  VersionEdit e = EditAdding({{0, F(5, "a", "m")},
                              {0, F(9, "c", "z")},
                              {1, F(3, "a", "c")},
                              {1, F(4, "m", "p")},
                              {2, F(6, "b", "d")},
                              {3, F(7, "e", "g")}},
                             20);
  e.log_number = 4;
  e.last_sequence = 500;
  e.compact_pointers = {{1, IKey("c", 7)}};
  return Applied(VersionState{}, e);
}

// Random states that satisfy the invariants, covering every level, an empty level, a level 0 whose
// files overlap and the lower levels' disjoint ranges.
std::string PaddedKey(uint32_t i) {
  const std::string digits = std::to_string(i);
  return "k" + std::string(4 - digits.size(), '0') + digits;
}

VersionState RandomState(Random* rnd) {
  VersionState s;
  std::array<uint32_t, kLevels> counts{};
  uint32_t total = 0;
  for (uint32_t& c : counts) {
    c = rnd->Uniform(6);
    total += c;
  }

  // Distinct numbers, dealt out to the levels in a shuffled order.
  std::vector<uint64_t> numbers;
  uint64_t number = 2 + rnd->Uniform(5);
  for (uint32_t i = 0; i < total; i++) {
    numbers.push_back(number);
    number += 1 + rnd->Uniform(3);
  }
  for (size_t i = numbers.size(); i > 1; i--) {
    std::swap(numbers[i - 1], numbers[rnd->Uniform(static_cast<uint32_t>(i))]);
  }

  const auto make = [&](uint64_t n, uint32_t lo, uint32_t hi) {
    FileMetaData f;
    f.number = n;
    f.file_size = 1 + rnd->Uniform(1u << 20);
    const ValueType small_type = rnd->OneIn(2) ? ValueType::kValue : ValueType::kDeletion;
    const ValueType large_type = rnd->OneIn(2) ? ValueType::kValue : ValueType::kDeletion;
    // The smallest key's sequence is above the largest's, so a file of one user key is in order.
    f.smallest = IKey(PaddedKey(lo), 100 + rnd->Uniform(50), small_type);
    f.largest = IKey(PaddedKey(hi), 1 + rnd->Uniform(50), large_type);
    return f;
  };

  size_t next_number = 0;
  for (size_t level = 0; level < kLevels; level++) {
    if (level == 0) {
      for (uint32_t i = 0; i < counts[level]; i++) {
        const uint32_t a = rnd->Uniform(1000);
        const uint32_t b = rnd->Uniform(1000);
        s.files[level].push_back(make(numbers[next_number++], std::min(a, b), std::max(a, b)));
      }
      std::sort(s.files[level].begin(), s.files[level].end(),
                [](const FileMetaData& x, const FileMetaData& y) { return x.number < y.number; });
    } else {
      std::set<uint32_t> points;
      while (points.size() < 2 * counts[level]) {
        points.insert(rnd->Uniform(1000));
      }
      for (auto it = points.begin(); it != points.end();) {
        const uint32_t lo = *it++;
        const uint32_t hi = *it++;
        s.files[level].push_back(make(numbers[next_number++], lo, hi));
      }
    }
    if (rnd->OneIn(2)) {
      s.compact_pointers[level] = IKey(PaddedKey(rnd->Uniform(1000)), 1 + rnd->Uniform(100),
                                       rnd->OneIn(2) ? ValueType::kValue : ValueType::kDeletion);
    }
  }
  const uint64_t max_number =
      numbers.empty() ? 1 : *std::max_element(numbers.begin(), numbers.end());
  s.next_file_number = max_number + 1 + rnd->Uniform(5);
  s.log_number = rnd->Uniform(static_cast<uint32_t>(s.next_file_number));
  s.last_sequence = rnd->Uniform(1000);
  return s;
}

TEST(VersionStateTest, RandomStatesAreValid) {
  // Guards the generator the other tests lean on: a state it makes that is invalid would make a
  // passing round trip meaningless.
  Random rnd(5);
  for (int i = 0; i < 200; i++) {
    const VersionState s = RandomState(&rnd);
    const Status checked = CheckInvariants(s);
    ASSERT_TRUE(checked.ok()) << checked.ToString();
  }
}

TEST(VersionStateTest, SnapshotEditRebuildsState) {
  Random rnd(5);
  size_t files = 0;
  for (int i = 0; i < 200; i++) {
    const VersionState s = RandomState(&rnd);
    const VersionEdit snapshot = SnapshotEdit(s);
    VersionState rebuilt;
    const Status applied = ApplyEdit(VersionState{}, snapshot, &rebuilt);
    ASSERT_TRUE(applied.ok()) << applied.ToString() << "\n" << snapshot.DebugString();
    ASSERT_EQ(rebuilt, s) << snapshot.DebugString();
    for (const auto& level : s.files) {
      files += level.size();
    }

    // The way it travels: through the MANIFEST's bytes and back.
    std::string bytes;
    snapshot.EncodeTo(&bytes);
    VersionEdit decoded;
    const Status read = decoded.DecodeFrom(bytes);
    ASSERT_TRUE(read.ok()) << read.ToString();
    VersionState replayed;
    ASSERT_TRUE(ApplyEdit(VersionState{}, decoded, &replayed).ok());
    ASSERT_EQ(replayed, s);
  }
  EXPECT_GT(files, 500u) << "the random states are too small to make the test mean anything";
}

TEST(VersionStateTest, SnapshotOfTheEmptyStateSetsTheCounters) {
  const VersionEdit e = SnapshotEdit(VersionState{});
  EXPECT_EQ(e.log_number, std::optional<uint64_t>(0));
  EXPECT_EQ(e.next_file_number, std::optional<uint64_t>(2));
  EXPECT_EQ(e.last_sequence, std::optional<SequenceNumber>(0));
  EXPECT_TRUE(e.compact_pointers.empty());
  EXPECT_TRUE(e.deleted_files.empty());
  EXPECT_TRUE(e.new_files.empty());
}

TEST(VersionStateTest, TotalFileSizeAddsUp) {
  EXPECT_EQ(TotalFileSize({}), 0u);
  EXPECT_EQ(TotalFileSize({F(1, "a", "b", 10), F(2, "c", "d", 0), F(3, "e", "f", 5)}), 15u);
  EXPECT_EQ(TotalFileSize({F(1, "a", "b", uint64_t{1} << 40), F(2, "c", "d", uint64_t{1} << 40)}),
            uint64_t{1} << 41);
}

std::vector<uint64_t> OverlapNumbers(const VersionState& s, int level,
                                     std::optional<std::string_view> begin,
                                     std::optional<std::string_view> end) {
  return Numbers(OverlappingFiles(s, level, begin, end));
}

VersionState ThreeFilesAtLevel1() {
  return Applied(VersionState{},
                 EditAdding({{1, F(1, "a", "c")}, {1, F(2, "e", "g")}, {1, F(3, "i", "k")}}));
}

TEST(VersionStateTest, OverlappingFilesLevel1) {
  const VersionState s = ThreeFilesAtLevel1();
  EXPECT_EQ(OverlapNumbers(s, 1, "f", "j"), (NumberList{2, 3}));
  EXPECT_EQ(OverlapNumbers(s, 1, std::nullopt, std::nullopt), (NumberList{1, 2, 3}));
  EXPECT_EQ(OverlapNumbers(s, 1, "f", std::nullopt), (NumberList{2, 3}));
  EXPECT_EQ(OverlapNumbers(s, 1, std::nullopt, "f"), (NumberList{1, 2}));
  // The bounds are inclusive: a file whose last key is the first one asked for is in.
  EXPECT_EQ(OverlapNumbers(s, 1, "c", "e"), (NumberList{1, 2}));
  EXPECT_EQ(OverlapNumbers(s, 1, "g", "g"), (NumberList{2}));
  // Between files, before the first and after the last.
  EXPECT_EQ(OverlapNumbers(s, 1, "d", "d"), (NumberList{}));
  EXPECT_EQ(OverlapNumbers(s, 1, "", "A"), (NumberList{}));
  EXPECT_EQ(OverlapNumbers(s, 1, "z", std::nullopt), (NumberList{}));
  // A range that ends before it starts holds nothing.
  EXPECT_EQ(OverlapNumbers(s, 1, "j", "f"), (NumberList{}));
  // Another level of the same state is empty, and the result is copies of the files.
  EXPECT_EQ(OverlapNumbers(s, 2, std::nullopt, std::nullopt), (NumberList{}));
  EXPECT_EQ(OverlappingFiles(s, 1, "e", "e").front(), s.files[1][1]);
}

TEST(VersionStateTest, OverlappingFilesLevel0ExpandsTransitively) {
  const VersionState s = Applied(
      VersionState{},
      EditAdding(
          {{0, F(1, "a", "c")}, {0, F(2, "b", "f")}, {0, F(3, "e", "g")}, {0, F(4, "x", "z")}}));
  // [a,b] meets #1 and #2; #2 reaches f, which meets #3; nothing reaches x.
  EXPECT_EQ(OverlapNumbers(s, 0, "a", "b"), (NumberList{1, 2, 3}));
  // Starting from the far end, the range grows downwards, and #1 and #2 come before #3 in the
  // state: they are passed over, and then taken on a later pass once #3 has widened the range.
  EXPECT_EQ(OverlapNumbers(s, 0, "g", "g"), (NumberList{1, 2, 3}));
  EXPECT_EQ(OverlapNumbers(s, 0, "f", "f"), (NumberList{1, 2, 3}));
  EXPECT_EQ(OverlapNumbers(s, 0, "c", "c"), (NumberList{1, 2, 3}));
  EXPECT_EQ(OverlapNumbers(s, 0, "y", "y"), (NumberList{4}));
  EXPECT_EQ(OverlapNumbers(s, 0, "h", "w"), (NumberList{}));
  EXPECT_EQ(OverlapNumbers(s, 0, std::nullopt, std::nullopt), (NumberList{1, 2, 3, 4}));
  EXPECT_EQ(OverlapNumbers(s, 0, std::nullopt, "b"), (NumberList{1, 2, 3}));
}

TEST(VersionStateTest, OverlappingFilesLevel0KeepsStateOrder) {
  // Files come back in the order the state holds them: oldest first.
  const VersionState s = Applied(
      VersionState{}, EditAdding({{0, F(8, "a", "z")}, {0, F(3, "c", "d")}, {0, F(5, "b", "x")}}));
  EXPECT_EQ(OverlapNumbers(s, 0, "c", "c"), (NumberList{3, 5, 8}));
}

TEST(VersionStateTest, OverlappingFilesMatchesAScanOnRandomStates) {
  Random rnd(11);
  for (int i = 0; i < 100; i++) {
    const VersionState s = RandomState(&rnd);
    for (int probe = 0; probe < 20; probe++) {
      const std::string lo = PaddedKey(rnd.Uniform(1000));
      const std::string hi = PaddedKey(rnd.Uniform(1000));
      const std::optional<std::string_view> begin =
          rnd.OneIn(5) ? std::nullopt : std::optional<std::string_view>(lo);
      const std::optional<std::string_view> end =
          rnd.OneIn(5) ? std::nullopt : std::optional<std::string_view>(hi);
      for (int level = 0; level < kNumLevels; level++) {
        const std::vector<FileMetaData> got = OverlappingFiles(s, level, begin, end);
        const std::vector<FileMetaData>& files = s.files[static_cast<size_t>(level)];
        const auto touches = [&](const FileMetaData& f, std::optional<std::string_view> from,
                                 std::optional<std::string_view> to) {
          return (!to || BytewiseCompare(ExtractUserKey(f.smallest), *to) <= 0) &&
                 (!from || BytewiseCompare(ExtractUserKey(f.largest), *from) >= 0);
        };
        if (begin && end && BytewiseCompare(*begin, *end) > 0) {
          EXPECT_TRUE(got.empty());
          continue;
        }
        if (level > 0) {
          std::vector<FileMetaData> want;
          for (const FileMetaData& f : files) {
            if (touches(f, begin, end)) {
              want.push_back(f);
            }
          }
          ASSERT_EQ(got, want) << "level " << level;
          continue;
        }
        // Level 0: everything that touches the range is in, and nothing outside the result
        // touches any file in it, so the result cannot be grown by another overlap.
        for (const FileMetaData& f : files) {
          const bool in = std::find(got.begin(), got.end(), f) != got.end();
          if (touches(f, begin, end)) {
            ASSERT_TRUE(in) << "file " << f.number << " touches the range but is missing";
          }
          if (!in) {
            for (const FileMetaData& member : got) {
              ASSERT_FALSE(
                  touches(f, ExtractUserKey(member.smallest), ExtractUserKey(member.largest)))
                  << "file " << f.number << " overlaps result file " << member.number;
            }
          }
        }
        // And the result keeps the state's order.
        for (size_t k = 1; k < got.size(); k++) {
          ASSERT_LT(got[k - 1].number, got[k].number);
        }
      }
    }
  }
}

TEST(VersionStateTest, FindFileBinarySearch) {
  const VersionState s = ThreeFilesAtLevel1();
  const std::vector<FileMetaData>& files = s.files[1];  // [a,c] [e,g] [i,k]
  EXPECT_EQ(FindFile({}, IKey("a", 1)), 0u);
  EXPECT_EQ(FindFile(files, IKey("", 1)), 0u);
  EXPECT_EQ(FindFile(files, IKey("a", 1)), 0u);
  EXPECT_EQ(FindFile(files, IKey("b", 50)), 0u);
  EXPECT_EQ(FindFile(files, IKey("d", 50)), 1u);
  EXPECT_EQ(FindFile(files, IKey("e", 50)), 1u);
  EXPECT_EQ(FindFile(files, IKey("g", 50)), 1u);
  EXPECT_EQ(FindFile(files, IKey("h", 50)), 2u);
  EXPECT_EQ(FindFile(files, IKey("k", 50)), 2u);
  EXPECT_EQ(FindFile(files, IKey("l", 50)), 3u);
  EXPECT_EQ(FindFile(files, IKey("zzz", 50)), 3u);

  // At the largest key of a file, the sequence number decides. The file's largest key is (c, 1):
  // an entry for c with a larger sequence number sorts before it and is in the file's range, one
  // with a smaller sequence number sorts after it and is not.
  EXPECT_EQ(FindFile(files, IKey("c", 5)), 0u);
  EXPECT_EQ(FindFile(files, IKey("c", 1)), 0u);
  EXPECT_EQ(FindFile(files, IKey("c", 0)), 1u);
}

TEST(VersionStateTest, FindFileMatchesAScanOnRandomStates) {
  Random rnd(13);
  for (int i = 0; i < 100; i++) {
    const VersionState s = RandomState(&rnd);
    for (size_t level = 1; level < kLevels; level++) {
      const std::vector<FileMetaData>& files = s.files[level];
      for (int probe = 0; probe < 40; probe++) {
        const std::string key = IKey(PaddedKey(rnd.Uniform(1000)), rnd.Uniform(160),
                                     rnd.OneIn(2) ? ValueType::kValue : ValueType::kDeletion);
        size_t want = files.size();
        for (size_t k = 0; k < files.size(); k++) {
          if (CompareInternalKey(files[k].largest, key) >= 0) {
            want = k;
            break;
          }
        }
        ASSERT_EQ(FindFile(files, key), want) << "level " << level;
      }
    }
  }
}

TEST(VersionStateTest, IsBaseLevelForKey) {
  const VersionState s = Applied(VersionState{}, EditAdding({{3, F(1, "e", "g")}}));
  // "f" lies in the only file below level 2, so level 2 is not the base for it; nothing is
  // deeper than level 3.
  EXPECT_FALSE(IsBaseLevelForKey(s, 2, "f"));
  EXPECT_TRUE(IsBaseLevelForKey(s, 3, "f"));
  EXPECT_FALSE(IsBaseLevelForKey(s, 0, "f"));
  EXPECT_FALSE(IsBaseLevelForKey(s, 1, "f"));
  EXPECT_TRUE(IsBaseLevelForKey(s, 4, "f"));
  EXPECT_TRUE(IsBaseLevelForKey(s, kNumLevels - 1, "f"));
  // The range's ends are in it; the keys just outside are not.
  EXPECT_FALSE(IsBaseLevelForKey(s, 2, "e"));
  EXPECT_FALSE(IsBaseLevelForKey(s, 2, "g"));
  EXPECT_TRUE(IsBaseLevelForKey(s, 2, "d"));
  EXPECT_TRUE(IsBaseLevelForKey(s, 2, std::string("g\0", 2)));
  EXPECT_TRUE(IsBaseLevelForKey(s, 2, "h"));
}

TEST(VersionStateTest, IsBaseLevelForKeyIgnoresLevelsAboveAndTheLevelItself) {
  // Files at levels 0 to 2 hold "m" and none deeper does.
  const VersionState s = Applied(
      VersionState{},
      EditAdding(
          {{0, F(1, "a", "z")}, {1, F(2, "a", "z")}, {2, F(3, "a", "z")}, {4, F(4, "a", "c")}}));
  EXPECT_TRUE(IsBaseLevelForKey(s, 2, "m"));
  EXPECT_FALSE(IsBaseLevelForKey(s, 1, "m"));
  EXPECT_FALSE(IsBaseLevelForKey(s, 0, "m"));
  // A key in a gap between the files of a deeper level.
  const VersionState gaps = Applied(
      VersionState{}, EditAdding({{2, F(1, "a", "c")}, {2, F(2, "i", "k")}, {2, F(3, "q", "s")}}));
  EXPECT_TRUE(IsBaseLevelForKey(gaps, 1, "f"));
  EXPECT_TRUE(IsBaseLevelForKey(gaps, 1, "m"));
  EXPECT_FALSE(IsBaseLevelForKey(gaps, 1, "j"));
  EXPECT_FALSE(IsBaseLevelForKey(gaps, 1, "q"));
  EXPECT_TRUE(IsBaseLevelForKey(gaps, 1, "t"));
}

TEST(VersionStateTest, IsBaseLevelForKeyMatchesAScanOnRandomStates) {
  Random rnd(17);
  for (int i = 0; i < 100; i++) {
    const VersionState s = RandomState(&rnd);
    for (int probe = 0; probe < 40; probe++) {
      const std::string key = PaddedKey(rnd.Uniform(1000));
      for (int level = 0; level < kNumLevels; level++) {
        bool want = true;
        for (int deeper = level + 1; deeper < kNumLevels; deeper++) {
          for (const FileMetaData& f : s.files[static_cast<size_t>(deeper)]) {
            if (BytewiseCompare(ExtractUserKey(f.smallest), key) <= 0 &&
                BytewiseCompare(key, ExtractUserKey(f.largest)) <= 0) {
              want = false;
            }
          }
        }
        ASSERT_EQ(IsBaseLevelForKey(s, level, key), want) << "level " << level << " key " << key;
      }
    }
  }
}

TEST(VersionStateTest, CheckInvariantsAlarmsAndPasses) {
  const VersionState valid = SmallValidState();
  const Status ok = CheckInvariants(valid);
  EXPECT_TRUE(ok.ok()) << ok.ToString();

  // The overlapping level-2 files the hand-built state of the task asks for.
  VersionState overlapping = valid;
  overlapping.files[2] = {F(16, "a", "c"), F(17, "b", "d")};
  const Status refused = CheckInvariants(overlapping);
  EXPECT_TRUE(refused.IsCorruption()) << refused.ToString();
  EXPECT_EQ(refused.ToString(), "Corruption: files 16 and 17 overlap at level 2");

  // One state per rule, each valid but for one thing, so that every rule is seen to fire.
  struct Case {
    const char* name;
    void (*break_it)(VersionState*);
    const char* message;
  };
  const Case cases[] = {
      {"duplicate number", [](VersionState* s) { s->files[3].push_back(F(5, "x", "y")); },
       "Corruption: file 5 appears twice"},
      {"number not below next_file_number", [](VersionState* s) { s->next_file_number = 9; },
       "Corruption: file 9 not below next_file_number 9"},
      {"smallest above largest", [](VersionState* s) { s->files[1][0] = F(3, "c", "a"); },
       "Corruption: file 3 has smallest > largest"},
      {"overlap", [](VersionState* s) { s->files[2] = {F(16, "a", "c"), F(17, "b", "d")}; },
       "Corruption: files 16 and 17 overlap at level 2"},
      {"nested overlap", [](VersionState* s) { s->files[2] = {F(16, "a", "z"), F(17, "b", "d")}; },
       "Corruption: files 16 and 17 overlap at level 2"},
      {"level 0 out of order", [](VersionState* s) { std::swap(s->files[0][0], s->files[0][1]); },
       "Corruption: files 9 and 5 out of order at level 0"},
      {"level 1 out of order", [](VersionState* s) { std::swap(s->files[1][0], s->files[1][1]); },
       "Corruption: files 4 and 3 out of order at level 1"},
      {"malformed key", [](VersionState* s) { s->files[3][0].smallest = "x"; },
       "Corruption: file 7 has a malformed internal key"},
      {"malformed compact pointer", [](VersionState* s) { s->compact_pointers[4] = "x"; },
       "Corruption: compact pointer at level 4 is not an internal key"},
  };
  for (const Case& c : cases) {
    VersionState broken = valid;
    c.break_it(&broken);
    const Status s = CheckInvariants(broken);
    EXPECT_TRUE(s.IsCorruption()) << c.name << ": " << s.ToString();
    EXPECT_EQ(s.ToString(), c.message) << c.name;
  }
}

TEST(VersionStateTest, AcceptedEditsAlwaysLeaveAValidState) {
  // A random walk of edits, most of them refused: whatever ApplyEdit accepts passes the whole-state
  // check, so the two cannot disagree about what a valid state is.
  Random rnd(23);
  VersionState state;
  uint64_t next_number = 2;
  size_t accepted = 0;
  size_t refused = 0;
  for (int step = 0; step < 2000; step++) {
    VersionEdit e;
    for (uint32_t k = rnd.Uniform(3); k > 0; k--) {
      const int level = static_cast<int>(rnd.Uniform(kNumLevels));
      const uint32_t a = rnd.Uniform(40);
      const uint32_t b = a + rnd.Uniform(5);
      // Mostly fresh numbers; now and then one that is already taken.
      const uint64_t number =
          rnd.OneIn(8) ? 2 + rnd.Uniform(static_cast<uint32_t>(next_number)) : next_number++;
      e.new_files.emplace_back(level, F(number, PaddedKey(a * 3), PaddedKey(b * 3)));
    }
    if (rnd.OneIn(2)) {
      const size_t level = rnd.Uniform(kNumLevels);
      if (!state.files[level].empty()) {
        e.deleted_files.emplace(
            static_cast<int>(level),
            state.files[level][rnd.Uniform(static_cast<uint32_t>(state.files[level].size()))]
                .number);
      }
    }
    e.next_file_number = next_number + rnd.Uniform(2);
    VersionState out;
    const Status s = ApplyEdit(state, e, &out);
    if (s.ok()) {
      accepted++;
      const Status checked = CheckInvariants(out);
      ASSERT_TRUE(checked.ok()) << checked.ToString();
      state = std::move(out);
    } else {
      refused++;
      ASSERT_TRUE(s.IsCorruption()) << s.ToString();
    }
  }
  EXPECT_GT(accepted, 200u);
  EXPECT_GT(refused, 100u);
}

}  // namespace
}  // namespace shale
