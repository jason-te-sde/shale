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
// count somewhere a function can reach it. Each test thread has its own.
thread_local int compare_calls = 0;

int CountingCompare(std::string_view a, std::string_view b) {
  ++compare_calls;
  return BytewiseCompare(a, b);
}

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

std::unique_ptr<Iterator> NewIter(std::string contents) {
  return NewBlockIterator(std::make_shared<const Block>(std::move(contents)), BytewiseCompare);
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

TEST(BlockTest, EmptyBlock) {
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

TEST(BlockTest, RoundTripWithPrefixSharing) {
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

TEST(BlockTest, RestartIntervalExtremes) {
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

TEST(BlockTest, SeekSearchesRestartPointsInsteadOfScanningEverything) {
  constexpr int kEntries = 10000;
  constexpr int kInterval = 16;
  const auto block =
      std::make_shared<const Block>(BuildBlock(SortedEntries(kEntries, 10), kInterval));
  const std::unique_ptr<Iterator> it = NewBlockIterator(block, CountingCompare);

  // 625 restart points take at most 10 probes to search, and the walk after them covers one
  // interval. A scan from the start would take thousands.
  constexpr int kMostComparisons = 10 + kInterval;
  for (const int target : {0, 1, 17, 4999, 5000, 9983, 9999}) {
    compare_calls = 0;
    it->Seek(KeyOf(target));
    ASSERT_TRUE(it->Valid()) << target;
    EXPECT_EQ(it->key(), KeyOf(target));
    EXPECT_LE(compare_calls, kMostComparisons) << "seeking " << target;
  }
}

TEST(BlockTest, BinaryKeysAndEmptyValues) {
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

TEST(BlockTest, PrefixCompressionSavesSpace) {
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

TEST(BlockTest, BuilderResetAndEstimate) {
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

TEST(BlockTest, IteratorKeepsTheBlockAlive) {
  auto block = std::make_shared<const Block>(BuildBlock(SortedEntries(50, 5), 16));
  const std::weak_ptr<const Block> watcher = block;
  std::unique_ptr<Iterator> it = NewBlockIterator(std::move(block), BytewiseCompare);
  EXPECT_FALSE(watcher.expired());

  EXPECT_EQ(Scan(it.get()).size(), 50U);
  it.reset();
  EXPECT_TRUE(watcher.expired());
}

TEST(BlockTest, TooShortIsCorruption) {
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

TEST(BlockTest, RestartCountTooLargeIsCorruption) {
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

TEST(BlockTest, FirstRestartPointMustBeTheStartOfTheEntries) {
  std::string damaged = BuildBlock(SortedEntries(10, 7), 4);
  const uint32_t count = DecodeFixed32(damaged.data() + damaged.size() - 4);
  EncodeFixed32(damaged.data() + damaged.size() - 4 * (size_t{count} + 1), 3);

  ExpectCorruption(*NewIter(damaged), "bad block contents");
}

TEST(BlockTest, EntryRunningPastTheRestartArrayIsCorruption) {
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

TEST(BlockTest, LengthsThatWrapAroundAreCorruption) {
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

TEST(BlockTest, EntryHeaderCutShortIsCorruption) {
  // A varint that is still going when the entries end, then a header with only two of its three.
  for (const std::string& entry : {std::string("\x80"), std::string("\x00\x01", 2)}) {
    const std::unique_ptr<Iterator> it = NewIter(RawBlock(entry));
    it->SeekToFirst();
    ExpectCorruption(*it, "bad entry in block");
  }
}

TEST(BlockTest, SharedBytesBeyondThePreviousKeyAreCorruption) {
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

TEST(BlockTest, CorruptionIsStickyUntilTheIteratorIsDropped) {
  const std::unique_ptr<Iterator> it =
      NewIter(RawBlock(RawEntry(0, "a", "1") + RawEntry(9, "x", "2")));
  it->SeekToFirst();
  it->Next();
  ExpectCorruption(*it, "bad entry in block");

  it->SeekToFirst();
  EXPECT_FALSE(it->Valid()) << "a failed iterator does not start again and report a clean status";
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(BlockTest, RestartPointOutsideTheEntriesIsCorruptionOnSeek) {
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

TEST(BlockTest, RestartPointInTheMiddleOfAnEntryNeverCrashes) {
  // A restart point that lands inside an entry's key bytes, so the "entry" decoded there is made of
  // key characters. Whatever the seek concludes, it stays inside the block.
  std::string block = RawEntry(0, "abcdefgh", "12345678") + RawEntry(0, "z", "9");
  PutFixed32(&block, 0);
  PutFixed32(&block, 5);
  PutFixed32(&block, 2);
  const std::unique_ptr<Iterator> it = NewIter(block);
  for (const std::string_view target : {"", "a", "abcdefgh", "b", "z", "zz"}) {
    ExpectSeekIsSane(it.get(), target);
  }
}

TEST(BlockTest, TruncatedEntriesEndWithCorruption) {
  Entries entries = SortedEntries(100, 8);
  entries.back().second = "last value";
  const std::string full = BuildBlock(entries, 16);

  const uint32_t count = DecodeFixed32(full.data() + full.size() - 4);
  const size_t entries_end = full.size() - 4 * (size_t{count} + 1);
  std::vector<uint32_t> restarts;
  for (uint32_t i = 0; i < count; ++i) {
    restarts.push_back(DecodeFixed32(full.data() + entries_end + 4 * size_t{i}));
  }

  int corrupted = 0;
  for (size_t cut = 1; cut < entries_end; ++cut) {
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

    const std::unique_ptr<Iterator> it = NewIter(damaged);
    const Entries scanned = Scan(it.get());
    ASSERT_LE(scanned.size(), entries.size()) << "cut at " << cut;
    EXPECT_TRUE(std::equal(scanned.begin(), scanned.end(), entries.begin()))
        << "cut at " << cut << ": what comes back is the entries before the damage";
    if (!it->status().ok()) {
      ++corrupted;
      ExpectCorruption(*it, "bad entry in block");
    }

    // The restart points are used by Seek() too, and must keep it inside the block.
    for (size_t i = 0; i < entries.size(); i += 7) {
      const std::unique_ptr<Iterator> seeker = NewIter(damaged);
      ExpectSeekIsSane(seeker.get(), entries[i].first);
    }
  }
  EXPECT_GT(corrupted, 0) << "a cut in the middle of an entry is not a clean end";

  // The last entry short by one byte of its value is the sharpest case, and must not be dropped
  // silently as though the block had ended there.
  std::string one_short = full.substr(0, entries_end - 1);
  uint32_t kept = 0;
  for (const uint32_t restart : restarts) {
    if (restart < entries_end - 1) {
      PutFixed32(&one_short, restart);
      ++kept;
    }
  }
  PutFixed32(&one_short, kept);
  const std::unique_ptr<Iterator> it = NewIter(one_short);
  EXPECT_EQ(Scan(it.get()).size(), entries.size() - 1);
  ExpectCorruption(*it, "bad entry in block");
}

TEST(BlockTest, RandomBitFlipsNeverCrash) {
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
    const std::unique_ptr<Iterator> walker = NewBlockIterator(block, BytewiseCompare);
    static_cast<void>(Scan(walker.get()));

    for (int i = 0; i < 20; ++i) {
      const std::unique_ptr<Iterator> seeker = NewBlockIterator(block, BytewiseCompare);
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
