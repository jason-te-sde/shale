// The two-level iterator: an index over blocks, read as one sequence.
//
// What matters most is what happens at the seams. An empty block must not end the iteration, and
// an error in a block must: an iterator that took a failed block for an exhausted one would go on
// to the next and return a key sequence with a hole in it, which is how a deleted key comes back.
// The blocks here are vector-backed iterators and the index maps a block's last key to its number.

#include "io/two_level_iterator.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/iterator_util.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/test_util.h"

namespace shale {
namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;

// Drains `it` from its current position: the keys, in order.
std::vector<std::string> Remaining(Iterator* it) {
  std::vector<std::string> keys;
  for (; it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
  }
  return keys;
}

using Keys = std::vector<std::string>;

// Blocks given as lists of entries, and an index with one entry for each, whose value is the
// block's number as a decimal string. The key of an index entry is given with the block.
struct Fixture {
  std::vector<std::pair<std::string, Entries>> blocks;  // (index key, entries)
  int opened = 0;                                       // block function calls so far
  std::vector<int> opened_blocks;                       // which blocks, in order

  std::unique_ptr<Iterator> Index() const {
    Entries index;
    for (size_t i = 0; i < blocks.size(); ++i) {
      index.emplace_back(blocks[i].first, std::to_string(i));
    }
    return NewVectorIterator(std::move(index));
  }

  BlockFunction Function() {
    return [this](std::string_view index_value) -> std::unique_ptr<Iterator> {
      ++opened;
      const size_t number = static_cast<size_t>(std::stoi(std::string(index_value)));
      opened_blocks.push_back(static_cast<int>(number));
      return NewVectorIterator(blocks[number].second);
    };
  }

  std::unique_ptr<Iterator> Make() { return NewTwoLevelIterator(Index(), Function()); }
};

Entries Block(std::initializer_list<const char*> keys) {
  Entries entries;
  for (const char* key : keys) {
    entries.emplace_back(key, std::string("v-") + key);
  }
  return entries;
}

TEST(TwoLevelIteratorTest, SkipsEmptyBlocksAndCrossesBlocks) {
  Fixture f;
  f.blocks = {{"b", Block({"a", "b"})}, {"b", {}}, {"c", Block({"c"})}, {"c", {}}};
  std::unique_ptr<Iterator> it = f.Make();
  EXPECT_FALSE(it->Valid()) << "not positioned until the first seek";

  it->SeekToFirst();
  EXPECT_EQ(Remaining(it.get()), (Keys{"a", "b", "c"}));
  EXPECT_OK(it->status()) << "running out of blocks is not an error";

  it->Seek("bb");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "c");
  EXPECT_EQ(it->value(), "v-c");
  EXPECT_EQ(Remaining(it.get()), (Keys{"c"}));

  it->Seek("a");
  EXPECT_EQ(Remaining(it.get()), (Keys{"a", "b", "c"}));
  it->Seek("b");
  EXPECT_EQ(Remaining(it.get()), (Keys{"b", "c"}));
  it->Seek("d");
  EXPECT_FALSE(it->Valid()) << "past the last key";
  EXPECT_OK(it->status());
  it->Seek("");
  EXPECT_EQ(Remaining(it.get()), (Keys{"a", "b", "c"}));
  it->SeekToFirst();
  EXPECT_EQ(it->key(), "a") << "a seek, from wherever the iterator is";
}

TEST(TwoLevelIteratorTest, SeekIntoAnEmptyBlockMovesToTheNextEntry) {
  Fixture f;
  f.blocks = {{"a", Block({"a"})}, {"f", {}}, {"m", Block({"m", "n"})}};
  std::unique_ptr<Iterator> it = f.Make();
  it->Seek("c");  // the index sends this to the empty block
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "m");
  EXPECT_EQ(Remaining(it.get()), (Keys{"m", "n"}));
}

TEST(TwoLevelIteratorTest, IndexThatSendsASeekToABlockWithNothingBigEnoughMovesOn) {
  // An index key says its block ends at or after the target, which a damaged or hand-made index
  // can get wrong. The iterator reads what the blocks hold, and does not take the index's word.
  Fixture f;
  f.blocks = {{"z", Block({"a", "b"})}, {"zz", Block({"x", "y"})}};
  std::unique_ptr<Iterator> it = f.Make();
  it->Seek("c");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "x");
}

TEST(TwoLevelIteratorTest, NothingToIterate) {
  {
    Fixture f;  // no blocks at all
    std::unique_ptr<Iterator> it = f.Make();
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid());
    EXPECT_OK(it->status());
    it->Seek("a");
    EXPECT_FALSE(it->Valid());
    EXPECT_OK(it->status());
    EXPECT_EQ(f.opened, 0);
  }
  {
    Fixture f;  // blocks, and every one empty
    f.blocks = {{"a", {}}, {"b", {}}, {"c", {}}};
    std::unique_ptr<Iterator> it = f.Make();
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid());
    EXPECT_OK(it->status());
    EXPECT_EQ(f.opened, 3) << "each empty block was opened and passed";
  }
}

TEST(TwoLevelIteratorTest, OpensBlocksOnlyWhenReachedAndOncePerPass) {
  Fixture f;
  f.blocks = {{"b", Block({"a", "b"})}, {"d", Block({"c", "d"})}, {"f", Block({"e", "f"})}};
  std::unique_ptr<Iterator> it = f.Make();
  EXPECT_EQ(f.opened, 0) << "making the iterator reads nothing";
  it->SeekToFirst();
  EXPECT_EQ(f.opened_blocks, (std::vector<int>{0}));
  it->Next();
  EXPECT_EQ(f.opened_blocks, (std::vector<int>{0})) << "still in the first block";
  it->Next();
  EXPECT_EQ(f.opened_blocks, (std::vector<int>{0, 1}));
  f.opened_blocks.clear();
  it->Seek("e");
  EXPECT_EQ(f.opened_blocks, (std::vector<int>{2})) << "a seek opens the one block it needs";
}

TEST(TwoLevelIteratorTest, BlockErrorPropagates) {
  Fixture f;
  f.blocks = {{"b", Block({"a", "b"})}, {"d", Block({"c", "d"})}, {"f", Block({"e", "f"})}};
  int calls = 0;
  const BlockFunction function = [&](std::string_view index_value) -> std::unique_ptr<Iterator> {
    if (++calls == 2) {
      return NewErrorIterator(Status::IOError("x"));
    }
    return f.Function()(index_value);
  };
  std::unique_ptr<Iterator> it = NewTwoLevelIterator(f.Index(), function);

  it->SeekToFirst();
  Keys seen;
  for (; it->Valid(); it->Next()) {
    seen.emplace_back(it->key());
  }
  EXPECT_EQ(seen, (Keys{"a", "b"})) << "the iteration stops at the block that failed";
  EXPECT_TRUE(it->status().IsIOError()) << it->status().ToString();
  EXPECT_EQ(calls, 2) << "and does not go on to open the next block";

  // The failure is not forgotten by moving the iterator: a caller that seeks past the bad block
  // would otherwise be told all is well by a status that had cleared.
  it->Seek("e");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsIOError());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsIOError());
}

// Serves `limit` entries of keys "a", "b", ..., then becomes invalid with an error, as an iterator
// does that finds damage after it has returned some entries.
class FailsAfter final : public Iterator {
 public:
  FailsAfter(int limit, Status error) : limit_(limit), error_(std::move(error)) {}

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
  Status status() const override { return position_ >= limit_ ? error_ : Status(); }

 private:
  static std::string Key(int i) { return std::string(1, static_cast<char>('a' + i)); }

  const int limit_;
  const Status error_;
  int position_ = -1;
  mutable std::string key_;
};

TEST(TwoLevelIteratorTest, ErrorAfterSomeEntriesOfABlockIsNotTakenForItsEnd) {
  int calls = 0;
  std::unique_ptr<Iterator> index =
      NewVectorIterator({{"c", "0"}, {"z", "1"}});  // two blocks; the first fails after "a", "b"
  std::unique_ptr<Iterator> it = NewTwoLevelIterator(
      std::move(index), [&](std::string_view index_value) -> std::unique_ptr<Iterator> {
        ++calls;
        if (index_value == "0") {
          return std::make_unique<FailsAfter>(2, Status::Corruption("bad entry"));
        }
        return NewVectorIterator({{"x", "v"}, {"y", "v"}});
      });
  it->SeekToFirst();
  Keys seen;
  for (; it->Valid(); it->Next()) {
    seen.emplace_back(it->key());
  }
  EXPECT_EQ(seen, (Keys{"a", "b"})) << "a block that failed is not a block that ended";
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
  EXPECT_EQ(calls, 1);
}

TEST(TwoLevelIteratorTest, IndexErrorPropagates) {
  // An index that cannot be read at all.
  {
    std::unique_ptr<Iterator> it = NewTwoLevelIterator(
        NewErrorIterator(Status::Corruption("bad index")),
        [](std::string_view) -> std::unique_ptr<Iterator> { return NewEmptyIterator(); });
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid());
    EXPECT_TRUE(it->status().IsCorruption());
    it->Seek("a");
    EXPECT_FALSE(it->Valid());
    EXPECT_TRUE(it->status().IsCorruption());
  }
  // An index that fails after its first entry: the block that entry names is returned, and then
  // the iteration ends with the error and not with an end.
  {
    int opened = 0;
    std::unique_ptr<Iterator> it =
        NewTwoLevelIterator(std::make_unique<FailsAfter>(1, Status::IOError("index read")),
                            [&](std::string_view) -> std::unique_ptr<Iterator> {
                              ++opened;
                              return NewVectorIterator({{"1", "v"}, {"2", "v"}});
                            });
    it->SeekToFirst();
    Keys seen;
    for (; it->Valid(); it->Next()) {
      seen.emplace_back(it->key());
    }
    EXPECT_EQ(seen, (Keys{"1", "2"}));
    EXPECT_TRUE(it->status().IsIOError()) << it->status().ToString();
    EXPECT_EQ(opened, 1);
  }
}

TEST(TwoLevelIteratorTest, StatusOfAnIndexThatFailedBeforeAnySeek) {
  const std::unique_ptr<Iterator> it = NewTwoLevelIterator(
      NewErrorIterator(Status::Corruption("bad index")),
      [](std::string_view) -> std::unique_ptr<Iterator> { return NewEmptyIterator(); });
  EXPECT_TRUE(it->status().IsCorruption()) << "the error is visible before the first seek";
}

TEST(TwoLevelIteratorDeathTest, ContractViolationsAbort) {
  Fixture f;
  f.blocks = {{"a", Block({"a"})}};
  EXPECT_DEATH(
      {
        std::unique_ptr<Iterator> it = f.Make();
        it->SeekToFirst();
        it->Next();
        it->Next();
      },
      "requires Valid");
  EXPECT_DEATH(
      {
        std::unique_ptr<Iterator> it = f.Make();
        (void)it->key();
      },
      "requires Valid");
  EXPECT_DEATH(
      {
        std::unique_ptr<Iterator> it = NewTwoLevelIterator(
            NewVectorIterator({{"a", "0"}}),
            [](std::string_view) -> std::unique_ptr<Iterator> { return nullptr; });
        it->SeekToFirst();
      },
      "block function");
}

}  // namespace
}  // namespace shale
