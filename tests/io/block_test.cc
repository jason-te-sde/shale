// Data blocks: what the builder writes, and what the decoder does with bytes that are not what the
// builder wrote.
//
// A block comes off the disk, so the decoder has to treat it as hostile. The round-trip tests show
// that the format works; the rest feed it damaged blocks, which must end in a Corruption status
// and never in a read past the end of the buffer. The truncation and bit-flip tests only mean
// something under AddressSanitizer, where such a read is an error, so that is where they matter.

#include "io/block.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/block_builder.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/dbformat.h"
#include "util/random.h"

namespace shale {
namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;

// More entries than any block in these tests can hold: an iteration that gets this far is not
// going to end.
constexpr size_t kMaxSteps = size_t{1} << 16;

// A KeyCompareFn is a plain function pointer, so a comparison that counts its calls has to keep the
// count somewhere a function can reach it. Each test thread has its own, and the fixture zeroes it
// before every test.
thread_local int64_t compare_calls = 0;

// More comparisons than a correct seek makes in any of these tests, damaged block or not: a walk
// over a block is bounded by the entries it holds, and a search by the log of its restart points. A
// search that stops narrowing, or a loop that never ends, calls the comparison again and again;
// without a budget the test would hang until the test runner gave up on it, and with one the
// first comparison past it aborts and says why.
constexpr int64_t kCompareBudget = int64_t{1} << 26;

int CountedCompare(std::string_view a, std::string_view b) {
  SHALE_CHECK(++compare_calls <= kCompareBudget,
              "block_test: the comparison budget is spent, so a seek is not making progress");
  return BytewiseCompare(a, b);
}

class BlockTest : public testing::Test {
 protected:
  void SetUp() override { compare_calls = 0; }
};

std::string KeyOf(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%06d", i);
  return buf;
}

// `n` entries with keys "key000000", "key000001", ... and values of skewed random lengths.
Entries SortedEntries(int n, uint64_t seed) {
  Random rnd(seed);
  Entries entries;
  for (int i = 0; i < n; ++i) {
    entries.emplace_back(KeyOf(i), RandomString(&rnd, rnd.Skewed(8)));
  }
  return entries;
}

std::string BuildBlock(const Entries& entries, int restart_interval) {
  BlockBuilder builder(restart_interval);
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  return std::string(builder.Finish());
}

std::unique_ptr<Iterator> NewIter(std::string contents, KeyCompareFn cmp = CountedCompare,
                                  size_t min_key_size = 0) {
  return NewBlockIterator(std::make_shared<const Block>(std::move(contents)), cmp, min_key_size);
}

// Every entry from SeekToFirst() until the iterator is invalid.
Entries Scan(Iterator* it) {
  Entries scanned;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    scanned.emplace_back(std::string(it->key()), std::string(it->value()));
    if (scanned.size() > kMaxSteps) {
      ADD_FAILURE() << "the iteration does not end";
      break;
    }
  }
  return scanned;
}

// Copies the current key and value, which reads every byte the iterator says it exposes: if one of
// them is not inside the block, AddressSanitizer reports it here.
void ReadCurrent(const Iterator& it) {
  const std::string copy = std::string(it.key()) + std::string(it.value());
  EXPECT_EQ(copy.size(), it.key().size() + it.value().size());
}

// Seeks `it` to `target` and reports whether it is valid and positioned at a key not less than
// the target, which is all a seek promises whatever the block holds.
void ExpectSeekIsSane(Iterator* it, std::string_view target) {
  it->Seek(target);
  if (it->Valid()) {
    EXPECT_GE(BytewiseCompare(it->key(), target), 0);
    ReadCurrent(*it);
  }
}

void ExpectCorruption(const Iterator& it, std::string_view message) {
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.status().IsCorruption()) << it.status().ToString();
  EXPECT_NE(it.status().ToString().find(message), std::string::npos)
      << "wanted \"" << message << "\", got " << it.status().ToString();
}

// One entry as the format lays it out: shared, non_shared and value_len as varints, the bytes of
// the key after the shared prefix, and the value.
std::string RawEntry(uint32_t shared, std::string_view key_delta, std::string_view value) {
  std::string entry;
  PutVarint32(&entry, shared);
  PutVarint32(&entry, static_cast<uint32_t>(key_delta.size()));
  PutVarint32(&entry, static_cast<uint32_t>(value.size()));
  entry.append(key_delta);
  entry.append(value);
  return entry;
}

// A block of the given entry bytes and a single restart point at the start.
std::string RawBlock(const std::string& entry_bytes) {
  std::string block = entry_bytes;
  PutFixed32(&block, 0);
  PutFixed32(&block, 1);
  return block;
}

void SetLastFixed32(std::string* block, uint32_t value) {
  EncodeFixed32(block->data() + block->size() - 4, value);
}

TEST_F(BlockTest, EmptyBlock) {
  BlockBuilder builder(16);
  EXPECT_TRUE(builder.empty());
  const std::string_view finished = builder.Finish();
  std::string expected;
  PutFixed32(&expected, 0);
  PutFixed32(&expected, 1);
  EXPECT_EQ(finished, expected);

  const std::unique_ptr<Iterator> it = NewIter(std::string(finished));
  EXPECT_FALSE(it->Valid());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
  it->Seek("anything");
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

TEST_F(BlockTest, RoundTripWithPrefixSharing) {
  const Entries entries = SortedEntries(1000, 1);
  const std::unique_ptr<Iterator> it = NewIter(BuildBlock(entries, 16));

  EXPECT_EQ(Scan(it.get()), entries);
  EXPECT_OK(it->status());

  for (const auto& [key, value] : entries) {
    it->Seek(key);
    ASSERT_TRUE(it->Valid()) << key;
    EXPECT_EQ(it->key(), key);
    EXPECT_EQ(it->value(), value);
  }

  it->Seek("key000500a");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "key000501") << "a target between two keys finds the larger";

  it->Seek("key000990");
  size_t remaining = 0;
  for (; it->Valid(); it->Next()) {
    ++remaining;
  }
  EXPECT_EQ(remaining, 10U) << "Next() carries on from a seek to the end";
  EXPECT_OK(it->status());

  it->Seek("zzz");
  EXPECT_FALSE(it->Valid()) << "a target past the last key";
  EXPECT_OK(it->status()) << "is the end of the data, not an error";

  it->Seek("");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), entries.front().first);
}

TEST_F(BlockTest, RestartIntervalExtremes) {
  const Entries entries = SortedEntries(300, 2);
  for (const int interval : {1, 1024}) {
    SCOPED_TRACE("restart interval " + std::to_string(interval));
    const std::unique_ptr<Iterator> it = NewIter(BuildBlock(entries, interval));
    EXPECT_EQ(Scan(it.get()), entries);
    for (const auto& [key, value] : entries) {
      it->Seek(key);
      ASSERT_TRUE(it->Valid()) << key;
      EXPECT_EQ(it->key(), key);
      EXPECT_EQ(it->value(), value);
    }
    it->Seek("key000100a");
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), "key000101");
  }
}

TEST_F(BlockTest, SeekSearchesRestartPointsInsteadOfScanningEverything) {
  constexpr int kEntries = 10000;
  constexpr int kInterval = 16;
  const auto block =
      std::make_shared<const Block>(BuildBlock(SortedEntries(kEntries, 10), kInterval));
  const std::unique_ptr<Iterator> it = NewBlockIterator(block, CountedCompare);

  // 625 restart points take at most 10 probes to search, and the walk after them covers one
  // interval. A scan from the start would take thousands.
  constexpr int kMostComparisons = 10 + kInterval;
  for (const int target : {0, 1, 17, 4999, 5000, 9983, 9999}) {
    compare_calls = 0;
    it->Seek(KeyOf(target));
    ASSERT_TRUE(it->Valid()) << target;
    EXPECT_EQ(it->key(), KeyOf(target));
    EXPECT_LE(compare_calls, int64_t{kMostComparisons}) << "seeking " << target;
  }
}

TEST_F(BlockTest, BinaryKeysAndEmptyValues) {
  // Keys of any bytes, the empty key among them, and values that are often empty. A map keeps them
  // sorted and unique in the order the iterator is given.
  Random rnd(3);
  std::map<std::string, std::string> sorted;
  sorted.emplace("", "empty key");
  for (int i = 0; i < 500; ++i) {
    sorted.emplace(RandomBinaryString(&rnd, rnd.Skewed(5)),
                   rnd.OneIn(3) ? std::string() : RandomBinaryString(&rnd, rnd.Skewed(6)));
  }
  const Entries entries(sorted.begin(), sorted.end());

  const std::unique_ptr<Iterator> it = NewIter(BuildBlock(entries, 16));
  EXPECT_EQ(Scan(it.get()), entries);
  for (const auto& [key, value] : entries) {
    it->Seek(key);
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), key);
    EXPECT_EQ(it->value(), value);
  }
}

TEST_F(BlockTest, PrefixCompressionSavesSpace) {
  const std::string prefix(50, 'p');
  Entries entries;
  size_t key_bytes = 0;
  for (int i = 0; i < 100; ++i) {
    std::string key = prefix + KeyOf(i);
    key_bytes += key.size();
    entries.emplace_back(std::move(key), "");
  }

  const std::string block = BuildBlock(entries, 16);
  EXPECT_LT(block.size(), key_bytes) << "the common prefix is stored once per restart point";
  EXPECT_LT(block.size(), BuildBlock(entries, 1).size())
      << "and more often with a shorter interval";
}

TEST_F(BlockTest, BuilderResetAndEstimate) {
  BlockBuilder builder(4);
  EXPECT_EQ(builder.CurrentSizeEstimate(), 8U) << "one restart point and the count";

  const Entries entries = SortedEntries(20, 4);
  size_t previous_estimate = builder.CurrentSizeEstimate();
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
    EXPECT_GT(builder.CurrentSizeEstimate(), previous_estimate);
    previous_estimate = builder.CurrentSizeEstimate();
  }
  EXPECT_FALSE(builder.empty());
  const std::string first(builder.Finish());
  EXPECT_EQ(first.size(), previous_estimate) << "the estimate is the size Finish() produces";

  builder.Reset();
  EXPECT_TRUE(builder.empty());
  EXPECT_EQ(builder.CurrentSizeEstimate(), 8U);
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  EXPECT_EQ(builder.Finish(), first) << "a reset builder starts from nothing";
}

TEST_F(BlockTest, IteratorKeepsTheBlockAlive) {
  auto block = std::make_shared<const Block>(BuildBlock(SortedEntries(50, 5), 16));
  const std::weak_ptr<const Block> watcher = block;
  std::unique_ptr<Iterator> it = NewBlockIterator(std::move(block), CountedCompare);
  EXPECT_FALSE(watcher.expired());

  EXPECT_EQ(Scan(it.get()).size(), 50U);
  it.reset();
  EXPECT_TRUE(watcher.expired());
}

TEST_F(BlockTest, TooShortIsCorruption) {
  for (const std::string& contents : {std::string(), std::string("a"), std::string("abc")}) {
    SCOPED_TRACE("size " + std::to_string(contents.size()));
    const Block block(contents);
    EXPECT_EQ(block.size(), contents.size());

    const std::unique_ptr<Iterator> it = NewIter(contents);
    EXPECT_FALSE(it->Valid());
    ExpectCorruption(*it, "bad block contents");
    it->SeekToFirst();
    ExpectCorruption(*it, "bad block contents");
    it->Seek("k");
    ExpectCorruption(*it, "bad block contents");
  }
}

TEST_F(BlockTest, RestartCountTooLargeIsCorruption) {
  const std::string valid = BuildBlock(SortedEntries(10, 6), 4);
  const auto room_for = static_cast<uint32_t>((valid.size() - 4) / 4);

  // One more restart point than the block has room for, a huge count, and none at all: the array
  // does not fit, or cannot describe the entries in front of it.
  for (const uint32_t count :
       {room_for + 1, uint32_t{0xffffffffU}, uint32_t{0x40000000U}, uint32_t{0}}) {
    SCOPED_TRACE("restart count " + std::to_string(count));
    std::string damaged = valid;
    SetLastFixed32(&damaged, count);
    const std::unique_ptr<Iterator> it = NewIter(damaged);
    ExpectCorruption(*it, "bad block contents");
    it->SeekToFirst();
    ExpectCorruption(*it, "bad block contents");
    it->Seek("key000003");
    ExpectCorruption(*it, "bad block contents");
  }
}

TEST_F(BlockTest, FirstRestartPointMustBeTheStartOfTheEntries) {
  std::string damaged = BuildBlock(SortedEntries(10, 7), 4);
  const uint32_t count = DecodeFixed32(damaged.data() + damaged.size() - 4);
  EncodeFixed32(damaged.data() + damaged.size() - 4 * (size_t{count} + 1), 3);

  ExpectCorruption(*NewIter(damaged), "bad block contents");
}

TEST_F(BlockTest, EntryRunningPastTheRestartArrayIsCorruption) {
  // The second entry says its key has 100 bytes, and only two follow before the restart array.
  std::string entries = RawEntry(0, "a", "1");
  PutVarint32(&entries, 0);
  PutVarint32(&entries, 100);
  PutVarint32(&entries, 1);
  entries += "xy";
  const std::unique_ptr<Iterator> it = NewIter(RawBlock(entries));

  it->SeekToFirst();
  ASSERT_TRUE(it->Valid()) << "the entry before the damage is still there";
  EXPECT_EQ(it->key(), "a");
  EXPECT_EQ(it->value(), "1");
  it->Next();
  ExpectCorruption(*it, "bad entry in block");
}

TEST_F(BlockTest, LengthsThatWrapAroundAreCorruption) {
  // non_shared + value_len is 1 modulo 2^32, which a decoder that adds in 32 bits would take for a
  // key of one byte.
  std::string entry;
  PutVarint32(&entry, 0);
  PutVarint32(&entry, 0xffffffffU);
  PutVarint32(&entry, 2);
  entry += "abc";
  const std::unique_ptr<Iterator> it = NewIter(RawBlock(entry));

  it->SeekToFirst();
  ExpectCorruption(*it, "bad entry in block");
}

TEST_F(BlockTest, EntryHeaderCutShortIsCorruption) {
  // A varint that is still going when the entries end, then a header with only two of its three.
  for (const std::string& entry : {std::string("\x80"), std::string("\x00\x01", 2)}) {
    const std::unique_ptr<Iterator> it = NewIter(RawBlock(entry));
    it->SeekToFirst();
    ExpectCorruption(*it, "bad entry in block");
  }
}

TEST_F(BlockTest, SharedBytesBeyondThePreviousKeyAreCorruption) {
  // The second entry shares 4 bytes with a previous key of 3.
  const std::unique_ptr<Iterator> it =
      NewIter(RawBlock(RawEntry(0, "abc", "1") + RawEntry(4, "x", "2")));
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "abc");
  it->Next();
  ExpectCorruption(*it, "bad entry in block");

  // And an entry at a restart point, where there is no previous key at all, that claims to share.
  const std::unique_ptr<Iterator> first = NewIter(RawBlock(RawEntry(1, "x", "1")));
  first->SeekToFirst();
  ExpectCorruption(*first, "bad entry in block");
}

TEST_F(BlockTest, CorruptionIsStickyUntilTheIteratorIsDropped) {
  const std::unique_ptr<Iterator> it =
      NewIter(RawBlock(RawEntry(0, "a", "1") + RawEntry(9, "x", "2")));
  it->SeekToFirst();
  it->Next();
  ExpectCorruption(*it, "bad entry in block");

  it->SeekToFirst();
  EXPECT_FALSE(it->Valid()) << "a failed iterator does not start again and report a clean status";
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST_F(BlockTest, RestartPointOutsideTheEntriesIsCorruptionOnSeek) {
  // Two restart points, the second far beyond the entries. Walking the block never looks at it;
  // a seek binary-searches over the restart points and has to read the key at the second.
  std::string block = RawEntry(0, "a", "1") + RawEntry(0, "b", "2");
  PutFixed32(&block, 0);
  PutFixed32(&block, 0x7fffffffU);
  PutFixed32(&block, 2);

  const std::unique_ptr<Iterator> walker = NewIter(block);
  EXPECT_EQ(Scan(walker.get()), (Entries{{"a", "1"}, {"b", "2"}}));
  EXPECT_OK(walker->status());

  const std::unique_ptr<Iterator> seeker = NewIter(block);
  seeker->Seek("zzz");
  ExpectCorruption(*seeker, "bad entry in block");
}

TEST_F(BlockTest, RestartPointInTheMiddleOfAnEntryReadsWhatIsThereAndStaysInside) {
  // The key of the first entry holds, in its middle, the bytes of another entry: "xyz" -> "v" with
  // its header. A restart point that lands there decodes as a plausible entry, so the search is
  // not stopped by a bad header at the probe, and the walk from it goes on into the bytes after it,
  // which are not an entry.
  const std::string hidden = RawEntry(0, "xyz", "v");
  const std::string first_key = "ab" + hidden + "cd";
  const std::string entries = RawEntry(0, first_key, "1234") + RawEntry(0, "zz", "9");
  const size_t hidden_offset = entries.find(hidden);
  ASSERT_NE(hidden_offset, std::string::npos);
  std::string block = entries;
  PutFixed32(&block, 0);
  PutFixed32(&block, static_cast<uint32_t>(hidden_offset));
  PutFixed32(&block, 2);

  // A fresh iterator for each target: damage found by one seek is sticky.
  struct Expected {
    std::string target;
    std::string key;  // The key it lands on, or empty for corruption.
  };
  const std::vector<Expected> expected = {
      {"", first_key},  // "xyz" is not below it, so the walk starts at the front
      {"ab", first_key},
      {"abz", "zz"},  // "xyz" is not below it either, and the walk passes the first entry
      {"xyz", "zz"},  // the probe finds "xyz" equal to the target, and the walk starts at the front
      {"xz", ""},     // "xyz" is below it, so the walk starts in the middle of the first entry
      {"zz", ""},
      {"zzz", ""},
  };
  for (const auto& [target, key] : expected) {
    SCOPED_TRACE("seeking \"" + target + "\"");
    const std::unique_ptr<Iterator> it = NewIter(block);
    it->Seek(target);
    if (key.empty()) {
      ExpectCorruption(*it, "bad entry in block");
    } else {
      ASSERT_TRUE(it->Valid());
      EXPECT_EQ(it->key(), key);
      ReadCurrent(*it);
      EXPECT_OK(it->status());
    }
  }
}

// The offset just past each entry in `block`, whose entries end at `entries_end`, found by reading
// the headers the way the format lays them out, which is not the decoder under test.
std::vector<size_t> EntryEnds(const std::string& block, size_t entries_end) {
  std::vector<size_t> ends;
  const char* const base = block.data();
  size_t offset = 0;
  while (offset < entries_end) {
    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_len = 0;
    const char* p = GetVarint32Ptr(base + offset, base + entries_end, &shared);
    p = p == nullptr ? nullptr : GetVarint32Ptr(p, base + entries_end, &non_shared);
    p = p == nullptr ? nullptr : GetVarint32Ptr(p, base + entries_end, &value_len);
    if (p == nullptr) {
      ADD_FAILURE() << "the valid block has a bad entry header at " << offset;
      break;
    }
    offset = static_cast<size_t>(p - base) + non_shared + value_len;
    ends.push_back(offset);
  }
  return ends;
}

TEST_F(BlockTest, TruncatedEntriesEndWithCorruption) {
  Entries entries = SortedEntries(100, 8);
  entries.back().second = "last value";
  const std::string full = BuildBlock(entries, 16);

  const uint32_t count = DecodeFixed32(full.data() + full.size() - 4);
  const size_t entries_end = full.size() - 4 * (size_t{count} + 1);
  std::vector<uint32_t> restarts;
  for (uint32_t i = 0; i < count; ++i) {
    restarts.push_back(DecodeFixed32(full.data() + entries_end + 4 * size_t{i}));
  }
  const std::vector<size_t> ends = EntryEnds(full, entries_end);
  ASSERT_EQ(ends.size(), entries.size());
  ASSERT_EQ(ends.back(), entries_end);

  for (size_t cut = 1; cut < entries_end; ++cut) {
    SCOPED_TRACE("cut at " + std::to_string(cut));
    // The entries cut short at `cut`, and a restart array that fits what is left: the restart
    // points that are still inside the entries, and their count. The block is well-formed except
    // that its last entry may stop in the middle.
    std::string damaged = full.substr(0, cut);
    uint32_t kept = 0;
    for (const uint32_t restart : restarts) {
      if (restart < cut) {
        PutFixed32(&damaged, restart);
        ++kept;
      }
    }
    PutFixed32(&damaged, kept);

    // A cut at the end of an entry is a block with fewer entries, and reads as one. A cut anywhere
    // else leaves an entry that is not all there, and is damage, which must not be taken for the
    // end of the data.
    const bool on_boundary = std::binary_search(ends.begin(), ends.end(), cut);
    const auto complete =
        static_cast<size_t>(std::upper_bound(ends.begin(), ends.end(), cut) - ends.begin());

    const std::unique_ptr<Iterator> it = NewIter(damaged);
    const Entries scanned = Scan(it.get());
    ASSERT_EQ(scanned.size(), complete) << "every entry that is all there, and no other";
    EXPECT_TRUE(std::equal(scanned.begin(), scanned.end(), entries.begin()));
    EXPECT_EQ(it->status().ok(), on_boundary);
    if (!on_boundary) {
      ExpectCorruption(*it, "bad entry in block");
    }

    // The restart points are used by Seek() too, and must keep it inside the block.
    for (size_t i = 0; i < entries.size(); i += 7) {
      const std::unique_ptr<Iterator> seeker = NewIter(damaged);
      ExpectSeekIsSane(seeker.get(), entries[i].first);
    }
  }
}

// An internal key for `user_key`, which is 8 bytes longer.
std::string InternalKeyOf(std::string_view user_key) {
  std::string key;
  AppendInternalKey(&key, {user_key, 1, ValueType::kValue});
  return key;
}

TEST_F(BlockTest, ShortKeyAtTheFrontStopsTheWalkBeforeTheComparator) {
  // CompareInternalKey aborts on a key shorter than its 8-byte tag, so a block read from a file
  // must not be able to hand it one. With a minimum key size the iterator refuses such an entry
  // first, and the walk is where the first entry of the block is read.
  const std::string block = BuildBlock({{"abc", "1"}, {InternalKeyOf("b"), "2"}}, 16);

  const std::unique_ptr<Iterator> first = NewIter(block, CompareInternalKey, 8);
  first->SeekToFirst();
  ExpectCorruption(*first, "bad entry in block");

  const std::unique_ptr<Iterator> seeker = NewIter(block, CompareInternalKey, 8);
  seeker->Seek(InternalKeyOf("a"));
  ExpectCorruption(*seeker, "bad entry in block");
}

TEST_F(BlockTest, ShortRestartKeyStopsTheSearchBeforeTheComparator) {
  // Every entry is a restart point, and the middle one has a 3-byte key. The first probe of the
  // binary search reads it straight from the block, and hands it to the comparator unless it has
  // been checked.
  const Entries entries = {{InternalKeyOf("a"), "1"},
                           {InternalKeyOf("b"), "2"},
                           {"abc", "3"},
                           {InternalKeyOf("d"), "4"},
                           {InternalKeyOf("e"), "5"}};
  const std::string block = BuildBlock(entries, 1);

  const std::unique_ptr<Iterator> seeker = NewIter(block, CompareInternalKey, 8);
  seeker->Seek(InternalKeyOf("c"));
  ExpectCorruption(*seeker, "bad entry in block");

  const std::unique_ptr<Iterator> walker = NewIter(block, CompareInternalKey, 8);
  walker->SeekToFirst();
  ASSERT_TRUE(walker->Valid());
  walker->Next();
  ASSERT_TRUE(walker->Valid());
  walker->Next();
  ExpectCorruption(*walker, "bad entry in block");

  // The same block, with no minimum and a comparison that takes any bytes, is an ordinary one.
  const std::unique_ptr<Iterator> plain = NewIter(block);
  EXPECT_EQ(Scan(plain.get()), entries);
  EXPECT_OK(plain->status());
}

TEST_F(BlockTest, ShortKeyAfterTheRestartKeyStopsTheWalkBeforeTheComparator) {
  // The second key shares two bytes with the first and adds one: it is 3 bytes long in all, and
  // only the key it is rebuilt into, not the bytes stored for it, shows that.
  const Entries entries = {{"abcdefgh12", "1"}, {"abd", "2"}};
  const std::string block = BuildBlock(entries, 16);
  const std::unique_ptr<Iterator> plain = NewIter(block);
  EXPECT_EQ(Scan(plain.get()), entries) << "an ordinary block when nothing is required of the keys";

  const std::unique_ptr<Iterator> seeker = NewIter(block, CompareInternalKey, 8);
  seeker->Seek(InternalKeyOf("zz"));
  ExpectCorruption(*seeker, "bad entry in block");

  const std::unique_ptr<Iterator> walker = NewIter(block, CompareInternalKey, 8);
  walker->SeekToFirst();
  ASSERT_TRUE(walker->Valid());
  EXPECT_EQ(walker->key(), "abcdefgh12");
  walker->Next();
  ExpectCorruption(*walker, "bad entry in block");
}

TEST_F(BlockTest, MinimumKeySizeIsInclusive) {
  for (const size_t key_size : {size_t{7}, size_t{8}, size_t{9}}) {
    SCOPED_TRACE("key size " + std::to_string(key_size));
    const std::string key(key_size, 'k');
    const std::unique_ptr<Iterator> it = NewIter(BuildBlock({{key, "v"}}, 16), BytewiseCompare, 8);
    it->SeekToFirst();
    if (key_size < 8) {
      ExpectCorruption(*it, "bad entry in block");
    } else {
      ASSERT_TRUE(it->Valid());
      EXPECT_EQ(it->key(), key);
      EXPECT_OK(it->status());
    }
  }
}

TEST_F(BlockTest, RandomBitFlipsNeverCrash) {
  const Entries entries = SortedEntries(200, 9);
  const std::string valid = BuildBlock(entries, 16);

  for (uint64_t seed = 0; seed < 2000; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    Random rnd(seed);
    std::string damaged = valid;
    const uint32_t flips = 1 + rnd.Uniform(8);
    for (uint32_t i = 0; i < flips; ++i) {
      const uint32_t bit = rnd.Uniform(static_cast<uint32_t>(damaged.size() * 8));
      damaged[bit / 8] = static_cast<char>(damaged[bit / 8] ^ (1 << (bit % 8)));
    }

    const auto block = std::make_shared<const Block>(damaged);
    const std::unique_ptr<Iterator> walker = NewBlockIterator(block, CountedCompare);
    static_cast<void>(Scan(walker.get()));

    for (int i = 0; i < 20; ++i) {
      const std::unique_ptr<Iterator> seeker = NewBlockIterator(block, CountedCompare);
      const std::string target = rnd.OneIn(4) ? RandomBinaryString(&rnd, rnd.Skewed(4))
                                              : KeyOf(static_cast<int>(rnd.Uniform(260)));
      ExpectSeekIsSane(seeker.get(), target);
      for (size_t steps = 0; seeker->Valid(); seeker->Next()) {
        ASSERT_LT(++steps, kMaxSteps) << "the iteration does not end";
        ReadCurrent(*seeker);
      }
    }
  }
}

TEST(BlockDeathTest, BuilderAbortsOnMisuse) {
  EXPECT_DEATH(BlockBuilder(0), "restart_interval");

  BlockBuilder builder(16);
  builder.Add("a", "1");
  (void)builder.Finish();
  EXPECT_DEATH(builder.Add("b", "2"), "after Finish");
}

}  // namespace
}  // namespace shale
