// The merging iterator: several sorted sequences read as one.
//
// The model for most of these tests is std::merge, which is stable, so that of two equal elements
// it puts the one from the first range first, and that is the order the iterator promises for equal
// keys: the child with the lower index comes first. The children are vector-backed iterators, whose
// Seek() orders keys bytewise, so a test that merges under another order does not seek.

#include "io/merging_iterator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/iterator_util.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/random.h"

namespace shale {
namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;
using Keys = std::vector<std::string>;

std::vector<std::unique_ptr<Iterator>> Children(std::vector<Entries> lists) {
  std::vector<std::unique_ptr<Iterator>> children;
  for (Entries& list : lists) {
    children.push_back(NewVectorIterator(std::move(list)));
  }
  return children;
}

// Entries with the given keys, each with the value "v-<key>".
Entries Of(std::initializer_list<const char*> keys) {
  Entries entries;
  for (const char* key : keys) {
    entries.emplace_back(key, std::string("v-") + key);
  }
  return entries;
}

// The keys `it` returns from where it is.
Keys Remaining(Iterator* it) {
  Keys keys;
  for (; it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
  }
  return keys;
}

Entries RemainingEntries(Iterator* it) {
  Entries entries;
  for (; it->Valid(); it->Next()) {
    entries.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  return entries;
}

TEST(MergingIteratorTest, NoChildrenIsEmpty) {
  std::unique_ptr<Iterator> it = NewMergingIterator({}, BytewiseCompare);
  EXPECT_FALSE(it->Valid());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->Seek("a");
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

TEST(MergingIteratorTest, SingleChildPassesThrough) {
  const Entries entries = Of({"a", "b", "c"});
  std::unique_ptr<Iterator> it = NewMergingIterator(Children({entries}), BytewiseCompare);
  EXPECT_FALSE(it->Valid()) << "not positioned until the first seek";
  it->SeekToFirst();
  EXPECT_EQ(RemainingEntries(it.get()), entries);
  EXPECT_OK(it->status());
  it->Seek("b");
  EXPECT_EQ(Remaining(it.get()), (Keys{"b", "c"}));
  it->Seek("d");
  EXPECT_FALSE(it->Valid());
}

TEST(MergingIteratorTest, InterleavedChildren) {
  std::unique_ptr<Iterator> it = NewMergingIterator(
      Children({Of({"a", "d", "g"}), Of({"b", "e"}), Of({"c", "f", "h"})}), BytewiseCompare);
  it->SeekToFirst();
  EXPECT_EQ(Remaining(it.get()), (Keys{"a", "b", "c", "d", "e", "f", "g", "h"}));
  EXPECT_OK(it->status());

  it->Seek("e");
  EXPECT_EQ(Remaining(it.get()), (Keys{"e", "f", "g", "h"}));
  it->Seek("z");
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
  it->Seek("");
  EXPECT_EQ(Remaining(it.get()).size(), size_t{8});
  it->Seek("dd");  // between keys of different children
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "e");
  EXPECT_EQ(it->value(), "v-e");
  it->SeekToFirst();
  EXPECT_EQ(it->key(), "a") << "a seek, from wherever the iterator is";
}

TEST(MergingIteratorTest, EmptyChildrenIgnored) {
  std::unique_ptr<Iterator> it =
      NewMergingIterator(Children({{}, Of({"a", "c"}), {}, Of({"b"}), {}}), BytewiseCompare);
  it->SeekToFirst();
  EXPECT_EQ(Remaining(it.get()), (Keys{"a", "b", "c"}));
  EXPECT_OK(it->status());

  std::unique_ptr<Iterator> all_empty = NewMergingIterator(Children({{}, {}, {}}), BytewiseCompare);
  all_empty->SeekToFirst();
  EXPECT_FALSE(all_empty->Valid());
  EXPECT_OK(all_empty->status());
  all_empty->Seek("a");
  EXPECT_FALSE(all_empty->Valid());
  EXPECT_OK(all_empty->status());
}

TEST(MergingIteratorTest, EqualKeysLowerIndexFirst) {
  std::unique_ptr<Iterator> it =
      NewMergingIterator(Children({{{"k", "0"}}, {{"k", "1"}}}), BytewiseCompare);
  it->SeekToFirst();
  EXPECT_EQ(RemainingEntries(it.get()), (Entries{{"k", "0"}, {"k", "1"}}));

  // Four children that all have the key, given in an order that is not the order of their values.
  std::unique_ptr<Iterator> four = NewMergingIterator(
      Children({{{"a", "x"}, {"k", "0"}}, {{"k", "1"}}, {{"j", "y"}, {"k", "2"}}, {{"k", "3"}}}),
      BytewiseCompare);
  four->SeekToFirst();
  EXPECT_EQ(RemainingEntries(four.get()),
            (Entries{{"a", "x"}, {"j", "y"}, {"k", "0"}, {"k", "1"}, {"k", "2"}, {"k", "3"}}));
  // A seek to the key lands on the first of the equal ones.
  four->Seek("k");
  ASSERT_TRUE(four->Valid());
  EXPECT_EQ(four->value(), "0");
  four->Seek("ja");
  ASSERT_TRUE(four->Valid());
  EXPECT_EQ(four->value(), "0");
}

TEST(MergingIteratorTest, ChildErrorSurfaces) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(NewVectorIterator(Of({"a", "c"})));
  children.push_back(NewVectorIterator(Of({"b"}), Status::Corruption("bad child")));
  std::unique_ptr<Iterator> it = NewMergingIterator(std::move(children), BytewiseCompare);
  EXPECT_TRUE(it->status().IsCorruption()) << "even before the first seek";
  it->SeekToFirst();
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
  while (it->Valid()) {
    it->Next();
  }
  EXPECT_TRUE(it->status().IsCorruption()) << "and after the last entry";
}

TEST(MergingIteratorTest, StatusIsTheFirstErrorOfTheChildren) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(NewVectorIterator(Of({"a"})));
  children.push_back(NewVectorIterator({}, Status::IOError("second")));
  children.push_back(NewVectorIterator({}, Status::Corruption("third")));
  std::unique_ptr<Iterator> it = NewMergingIterator(std::move(children), BytewiseCompare);
  it->SeekToFirst();
  EXPECT_TRUE(it->status().IsIOError()) << it->status().ToString();
}

TEST(MergingIteratorTest, ChildThatFailsEndsTheIteration) {
  // A child that is invalid because it failed has not run out of entries. Merging on without it
  // would return the others' entries with its own missing, and among them a deletion marker that
  // hides an older value in another child: the key would come back from the dead. The merged
  // iterator stops at the failure and says why.
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(NewVectorIterator(Of({"a", "c", "e"})));
  children.push_back(NewErrorIterator(Status::Corruption("bad child")));
  std::unique_ptr<Iterator> it = NewMergingIterator(std::move(children), BytewiseCompare);
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
  // And the error stays: a later seek does not turn it into a clean end.
  it->Seek("c");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

// Serves the keys "b", "d", ..., `limit` of them, then becomes invalid with an error, as an
// iterator does that finds damage after it has returned some entries.
class FailsAfter final : public Iterator {
 public:
  explicit FailsAfter(int limit) : limit_(limit) {}

  bool Valid() const override { return position_ >= 0 && position_ < limit_; }
  void SeekToFirst() override { position_ = 0; }
  void Seek(std::string_view target) override {
    position_ = 0;
    while (position_ < limit_ && Key(position_) < target) {
      ++position_;
    }
  }
  void Next() override { ++position_; }
  std::string_view key() const override {
    key_ = Key(position_);
    return key_;
  }
  std::string_view value() const override { return "v"; }
  Status status() const override {
    return position_ >= limit_ ? Status::Corruption("failed after some entries") : Status();
  }

 private:
  static std::string Key(int i) { return std::string(1, static_cast<char>('b' + 2 * i)); }

  const int limit_;
  int position_ = -1;
  mutable std::string key_;
};

TEST(MergingIteratorTest, ChildThatFailsMidwayEndsTheIterationThere) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(NewVectorIterator(Of({"a", "c", "e", "g"})));
  children.push_back(std::make_unique<FailsAfter>(2));  // "b", "d", then it fails
  std::unique_ptr<Iterator> it = NewMergingIterator(std::move(children), BytewiseCompare);
  it->SeekToFirst();
  const Keys seen = Remaining(it.get());
  EXPECT_EQ(seen, (Keys{"a", "b", "c", "d"})) << "everything up to the failure, and nothing after";
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();

  // The failed child would serve its entries again after a seek, as this one does, and the merged
  // iterator would then go on as if nothing had happened.
  it->Seek("a");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(MergingIteratorTest, ReseekAfterExhaustionStartsOver) {
  std::unique_ptr<Iterator> it =
      NewMergingIterator(Children({Of({"a", "c"}), Of({"b", "d"})}), BytewiseCompare);
  it->SeekToFirst();
  EXPECT_EQ(Remaining(it.get()), (Keys{"a", "b", "c", "d"}));
  EXPECT_FALSE(it->Valid());
  it->Seek("c");
  EXPECT_EQ(Remaining(it.get()), (Keys{"c", "d"}));
  it->Seek("a");
  EXPECT_EQ(Remaining(it.get()), (Keys{"a", "b", "c", "d"}));
}

int ReverseBytewise(std::string_view a, std::string_view b) { return BytewiseCompare(b, a); }

TEST(MergingIteratorTest, UsesTheComparisonItIsGiven) {
  // Children sorted by the reverse order, merged under it. A merge that compared bytewise
  // regardless would interleave them wrongly. These children are not sought.
  std::unique_ptr<Iterator> it = NewMergingIterator(
      Children({Of({"g", "d", "a"}), Of({"f", "e", "b"}), Of({"h", "c"})}), ReverseBytewise);
  it->SeekToFirst();
  EXPECT_EQ(Remaining(it.get()), (Keys{"h", "g", "f", "e", "d", "c", "b", "a"}));
}

TEST(MergingIteratorTest, MergesInternalKeysNewestFirst) {
  // Internal keys: one user key's entries are ordered by sequence number, newest first, whichever
  // child holds them; the memtable's entry for a key comes before the table's older one.
  auto key = [](std::string_view user, SequenceNumber sequence, ValueType type) {
    std::string k;
    AppendInternalKey(&k, ParsedInternalKey{user, sequence, type});
    return k;
  };
  Entries memtable = {{key("a", 9, ValueType::kValue), "a9"},
                      {key("b", 8, ValueType::kDeletion), ""}};
  Entries newer_table = {{key("a", 5, ValueType::kValue), "a5"},
                         {key("c", 4, ValueType::kValue), "c4"}};
  Entries older_table = {{key("b", 2, ValueType::kValue), "b2"},
                         {key("c", 1, ValueType::kValue), "c1"}};
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(NewVectorIterator(memtable));
  children.push_back(NewVectorIterator(newer_table));
  children.push_back(NewVectorIterator(older_table));
  std::unique_ptr<Iterator> it = NewMergingIterator(std::move(children), CompareInternalKey);
  it->SeekToFirst();
  const Entries got = RemainingEntries(it.get());
  const Entries expected = {
      {key("a", 9, ValueType::kValue), "a9"},  {key("a", 5, ValueType::kValue), "a5"},
      {key("b", 8, ValueType::kDeletion), ""}, {key("b", 2, ValueType::kValue), "b2"},
      {key("c", 4, ValueType::kValue), "c4"},  {key("c", 1, ValueType::kValue), "c1"}};
  EXPECT_EQ(got, expected);
}

TEST(MergingIteratorTest, OwnsItsChildren) {
  int destroyed = 0;
  std::vector<std::unique_ptr<Iterator>> children;
  for (int i = 0; i < 3; ++i) {
    children.push_back(
        NewCleanupIterator(NewVectorIterator(Of({"a"})), [&destroyed] { ++destroyed; }));
  }
  std::unique_ptr<Iterator> it = NewMergingIterator(std::move(children), BytewiseCompare);
  it->SeekToFirst();
  EXPECT_EQ(destroyed, 0);
  it.reset();
  EXPECT_EQ(destroyed, 3) << "the merged iterator took the children and let go of them";
}

TEST(MergingIteratorTest, ManyChildren) {
  // More children than any engine configuration has, to see that nothing depends on the number.
  Random rnd(3);
  std::vector<Entries> lists(100);
  Entries expected;
  for (size_t child = 0; child < lists.size(); ++child) {
    for (int i = 0; i < 20; ++i) {
      char key[16];
      std::snprintf(key, sizeof(key), "%05u", rnd.Uniform(1000));
      lists[child].emplace_back(key, "c" + std::to_string(child));
    }
    std::stable_sort(lists[child].begin(), lists[child].end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    lists[child].erase(std::unique(lists[child].begin(), lists[child].end(),
                                   [](const auto& a, const auto& b) { return a.first == b.first; }),
                       lists[child].end());
    Entries merged;
    std::merge(expected.begin(), expected.end(), lists[child].begin(), lists[child].end(),
               std::back_inserter(merged),
               [](const auto& a, const auto& b) { return a.first < b.first; });
    expected = std::move(merged);
  }
  std::unique_ptr<Iterator> it = NewMergingIterator(Children(lists), BytewiseCompare);
  it->SeekToFirst();
  EXPECT_EQ(RemainingEntries(it.get()), expected);
}

// Random children of random sorted unique strings, against std::merge applied child after child,
// which is stable and so puts equal keys in the order of the children's indexes.
TEST(MergingIteratorTest, RandomizedAgainstStdMerge) {
  const auto by_key = [](const auto& a, const auto& b) { return a.first < b.first; };
  for (uint64_t seed = 0; seed < 100; ++seed) {
    Random rnd(seed);
    const size_t num_children = 1 + rnd.Uniform(8);
    std::vector<Entries> lists(num_children);
    Entries expected;
    for (size_t child = 0; child < num_children; ++child) {
      // A small alphabet and short strings, so that the children share keys.
      std::vector<std::string> keys;
      const uint32_t count = rnd.Uniform(40);
      for (uint32_t i = 0; i < count; ++i) {
        std::string key(1 + rnd.Uniform(3), 'a');
        for (char& c : key) {
          c = static_cast<char>('a' + rnd.Uniform(4));
        }
        keys.push_back(key);
      }
      std::sort(keys.begin(), keys.end());
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
      for (const std::string& key : keys) {
        lists[child].emplace_back(key, "c" + std::to_string(child));
      }
      Entries merged;
      std::merge(expected.begin(), expected.end(), lists[child].begin(), lists[child].end(),
                 std::back_inserter(merged), by_key);
      expected = std::move(merged);
    }

    std::unique_ptr<Iterator> it = NewMergingIterator(Children(lists), BytewiseCompare);
    it->SeekToFirst();
    EXPECT_EQ(RemainingEntries(it.get()), expected) << "seed " << seed;
    EXPECT_OK(it->status());

    // Seeks to random targets land where lower_bound does, and read on in the same order.
    for (int i = 0; i < 10; ++i) {
      std::string target(rnd.Uniform(4), 'a');
      for (char& c : target) {
        c = static_cast<char>('a' + rnd.Uniform(5));
      }
      it->Seek(target);
      const Entries rest = RemainingEntries(it.get());
      const auto first =
          std::lower_bound(expected.begin(), expected.end(), target,
                           [](const auto& e, const std::string& t) { return e.first < t; });
      EXPECT_EQ(rest, Entries(first, expected.end()))
          << "seed " << seed << " target '" << target << "'";
    }
  }
}

TEST(MergingIteratorDeathTest, ContractViolationsAbort) {
  EXPECT_DEATH(
      {
        std::unique_ptr<Iterator> it =
            NewMergingIterator(Children({Of({"a"}), Of({"b"})}), BytewiseCompare);
        it->SeekToFirst();
        it->Next();
        it->Next();
        it->Next();
      },
      "requires Valid");
  EXPECT_DEATH(
      {
        std::unique_ptr<Iterator> it =
            NewMergingIterator(Children({Of({"a"}), Of({"b"})}), BytewiseCompare);
        (void)it->key();
      },
      "requires Valid");
  EXPECT_DEATH(
      {
        std::vector<std::unique_ptr<Iterator>> children;
        children.push_back(nullptr);
        children.push_back(NewVectorIterator(Of({"a"})));
        NewMergingIterator(std::move(children), BytewiseCompare);
      },
      "child");
}

}  // namespace
}  // namespace shale
