// The write batch: the unit of atomicity, and the record the write-ahead log stores.
//
// A batch is read back from the log during recovery, so everything that can be wrong with the bytes
// must come back as a Status and never as a crash, and the tests that follow pin each message.
// The tests that build a batch check it by inserting it into a memtable and reading the memtable
// back, which is the only way the batch is ever consumed.

#include "shale/write_batch.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "io/memtable.h"
#include "io/write_batch_internal.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"

namespace shale {
namespace {

// Renders what a batch does to a memtable, in the memtable's order: Put(key, value)@sequence and
// Delete(key)@sequence, concatenated. A batch that cannot be inserted ends with ParseError().
std::string PrintContents(const WriteBatch& batch) {
  const auto mem = std::make_shared<MemTable>(1);
  const Status s = WriteBatchInternal::InsertInto(&batch, mem.get());
  std::string state;
  const std::unique_ptr<Iterator> iter = mem->NewIterator();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey ikey;
    if (!ParseInternalKey(iter->key(), &ikey)) {
      state.append("BadInternalKey()");
      continue;
    }
    switch (ikey.type) {
      case ValueType::kValue:
        state.append("Put(").append(ikey.user_key).append(", ").append(iter->value()).append(")");
        break;
      case ValueType::kDeletion:
        state.append("Delete(").append(ikey.user_key).append(")");
        break;
    }
    state.append("@").append(std::to_string(ikey.sequence));
  }
  if (!s.ok()) {
    state.append("ParseError()");
  }
  return state;
}

// Records every call a batch makes, in order, as Put(key,value) and Delete(key).
class Recorder : public WriteBatch::Handler {
 public:
  void Put(std::string_view key, std::string_view value) override {
    calls.push_back("Put(" + std::string(key) + "," + std::string(value) + ")");
  }
  void Delete(std::string_view key) override {
    calls.push_back("Delete(" + std::string(key) + ")");
  }

  std::vector<std::string> calls;
};

// A batch with the three operations the tests below corrupt in turn.
WriteBatch ThreeOps() {
  WriteBatch b;
  b.Put("foo", "bar");
  b.Delete("box");
  b.Put("baz", "boo");
  WriteBatchInternal::SetSequence(&b, 200);
  return b;
}

// A batch rebuilt from `contents`, which must be at least a header.
WriteBatch FromContents(std::string_view contents) {
  WriteBatch b;
  EXPECT_OK(WriteBatchInternal::SetContents(&b, contents));
  return b;
}

TEST(WriteBatchTest, Empty) {
  WriteBatch b;
  EXPECT_EQ(b.Count(), 0u);
  EXPECT_EQ(WriteBatchInternal::Count(&b), 0u);
  EXPECT_EQ(WriteBatchInternal::ByteSize(&b), 12u);
  EXPECT_EQ(b.ApproximateSize(), 12u);
  EXPECT_EQ(WriteBatchInternal::Sequence(&b), 0u);
  EXPECT_EQ(PrintContents(b), "");
}

TEST(WriteBatchTest, MultipleOps) {
  WriteBatch b;
  b.Put("foo", "bar");
  b.Delete("box");
  b.Put("baz", "boo");
  WriteBatchInternal::SetSequence(&b, 100);
  EXPECT_EQ(WriteBatchInternal::Sequence(&b), 100u);
  EXPECT_EQ(b.Count(), 3u);
  EXPECT_EQ(PrintContents(b), "Put(baz, boo)@102Delete(box)@101Put(foo, bar)@100");
}

TEST(WriteBatchTest, IterateVisitsTheOperationsInTheOrderTheyWereAdded) {
  WriteBatch b = ThreeOps();
  Recorder recorder;
  ASSERT_OK(b.Iterate(&recorder));
  EXPECT_EQ(recorder.calls,
            (std::vector<std::string>{"Put(foo,bar)", "Delete(box)", "Put(baz,boo)"}));
}

TEST(WriteBatchTest, EncodingMatchesTheDocumentedLayout) {
  // The log stores these bytes, so the layout is a file format: sequence as a little-endian
  // fixed64, count as a fixed32, then one record per operation, a tag byte (1 for a put, 0 for a
  // deletion) followed by each string as a varint32 length and its bytes.
  WriteBatch b;
  b.Put("k", "vv");
  b.Delete("d");
  WriteBatchInternal::SetSequence(&b, 0x0102);
  const std::string expected(
      "\x02\x01\0\0\0\0\0\0"  // sequence
      "\x02\0\0\0"            // count
      "\x01\x01k\x02vv"       // Put("k", "vv")
      "\x00\x01"
      "d",  // Delete("d")
      12 + 6 + 3);
  EXPECT_EQ(std::string(WriteBatchInternal::Contents(&b)), expected);
}

TEST(WriteBatchTest, TruncationAtAnyLengthIsAlwaysReported) {
  // Cutting the contents anywhere after the header must never read as a valid batch: either a
  // record is cut, or the records that are left no longer match the count.
  const WriteBatch b = ThreeOps();
  const std::string contents(WriteBatchInternal::Contents(&b));
  for (size_t cut = 1; cut <= contents.size() - 12; cut++) {
    const WriteBatch truncated = FromContents(contents.substr(0, contents.size() - cut));
    Recorder recorder;
    const Status s = truncated.Iterate(&recorder);
    ASSERT_TRUE(s.IsCorruption()) << "cut " << cut << " bytes: " << s.ToString();
    const std::string text = s.ToString();
    EXPECT_TRUE(text == "Corruption: bad WriteBatch Put" ||
                text == "Corruption: bad WriteBatch Delete" ||
                text == "Corruption: WriteBatch has wrong count")
        << "cut " << cut << " bytes: " << text;
  }
}

TEST(WriteBatchTest, CorruptionsAreReported) {
  const WriteBatch b = ThreeOps();
  const std::string contents(WriteBatchInternal::Contents(&b));

  {
    // The last byte is the last byte of the final Put's value.
    const WriteBatch truncated = FromContents(contents.substr(0, contents.size() - 1));
    Recorder recorder;
    const Status s = truncated.Iterate(&recorder);
    EXPECT_EQ(s.ToString(), "Corruption: bad WriteBatch Put");
    // What came before the damage was still delivered, which is why a handler that applies the
    // operations leaves a partial result behind when this happens.
    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"Put(foo,bar)", "Delete(box)"}));
  }
  {
    WriteBatch deletion;
    deletion.Delete("box");
    const std::string d(WriteBatchInternal::Contents(&deletion));
    const WriteBatch truncated = FromContents(d.substr(0, d.size() - 1));
    Recorder recorder;
    EXPECT_EQ(truncated.Iterate(&recorder).ToString(), "Corruption: bad WriteBatch Delete");
    EXPECT_TRUE(recorder.calls.empty());
  }
  {
    WriteBatch too_many = ThreeOps();
    WriteBatchInternal::SetCount(&too_many, 5);
    Recorder recorder;
    EXPECT_EQ(too_many.Iterate(&recorder).ToString(), "Corruption: WriteBatch has wrong count");
    EXPECT_EQ(recorder.calls.size(), 3u);
  }
  {
    WriteBatch too_few = ThreeOps();
    WriteBatchInternal::SetCount(&too_few, 2);
    Recorder recorder;
    EXPECT_EQ(too_few.Iterate(&recorder).ToString(), "Corruption: WriteBatch has wrong count");
  }
  {
    std::string bad_tag = contents;
    bad_tag[12] = '\x07';  // the first record's tag
    const WriteBatch broken = FromContents(bad_tag);
    Recorder recorder;
    EXPECT_EQ(broken.Iterate(&recorder).ToString(), "Corruption: unknown WriteBatch tag");
    EXPECT_TRUE(recorder.calls.empty());
  }
  {
    // A length that claims more bytes than the batch has.
    std::string bad_length = contents;
    bad_length[13] = '\x7f';  // the first key's length
    const WriteBatch broken = FromContents(bad_length);
    Recorder recorder;
    EXPECT_EQ(broken.Iterate(&recorder).ToString(), "Corruption: bad WriteBatch Put");
  }
}

TEST(WriteBatchTest, ACorruptBatchIsRefusedByInsertIntoWithTheSameStatus) {
  WriteBatch b = ThreeOps();
  WriteBatchInternal::SetCount(&b, 5);
  const auto mem = std::make_shared<MemTable>(1);
  const Status s = WriteBatchInternal::InsertInto(&b, mem.get());
  EXPECT_EQ(s.ToString(), "Corruption: WriteBatch has wrong count");
}

TEST(WriteBatchTest, SetContentsTooSmall) {
  WriteBatch b = ThreeOps();
  const std::string before(WriteBatchInternal::Contents(&b));

  const Status s = WriteBatchInternal::SetContents(&b, std::string(11, '\0'));
  EXPECT_TRUE(s.IsCorruption());
  EXPECT_NE(s.ToString().find("too small"), std::string::npos) << s.ToString();
  EXPECT_EQ(s.ToString(), "Corruption: malformed WriteBatch (too small)");
  EXPECT_EQ(std::string(WriteBatchInternal::Contents(&b)), before) << "a refused batch changed";

  EXPECT_TRUE(WriteBatchInternal::SetContents(&b, "").IsCorruption());
  EXPECT_OK(WriteBatchInternal::SetContents(&b, std::string(12, '\0')));
  EXPECT_EQ(b.Count(), 0u);
}

TEST(WriteBatchTest, SetContentsRoundTrip) {
  const WriteBatch b = ThreeOps();
  const WriteBatch copy = FromContents(WriteBatchInternal::Contents(&b));
  EXPECT_EQ(copy.Count(), 3u);
  EXPECT_EQ(WriteBatchInternal::Sequence(&copy), 200u);
  EXPECT_EQ(PrintContents(copy), PrintContents(b));
}

TEST(WriteBatchTest, SetContentsForgetsThePayloadAccounting) {
  // Payload bytes and the oversize flag describe what was added through Put and Delete. Contents
  // that arrive whole, from the log, carry no such history, and the old figures must not linger.
  WriteBatch b;
  b.Put(std::string(kMaxKeySize + 1, 'k'), "v");
  ASSERT_TRUE(WriteBatchInternal::HasOversizedEntry(&b));
  ASSERT_GT(WriteBatchInternal::PayloadBytes(&b), 0u);
  ASSERT_OK(WriteBatchInternal::SetContents(&b, std::string(12, '\0')));
  EXPECT_FALSE(WriteBatchInternal::HasOversizedEntry(&b));
  EXPECT_EQ(WriteBatchInternal::PayloadBytes(&b), 0u);
}

TEST(WriteBatchTest, AppendConcatenates) {
  WriteBatch dst;
  dst.Put("a", "va");
  WriteBatchInternal::SetSequence(&dst, 200);
  WriteBatch src;
  src.Put("b", "vb");
  src.Delete("c");
  WriteBatchInternal::SetSequence(&src, 900);  // Append takes the records, not the sequence

  WriteBatchInternal::Append(&dst, &src);
  EXPECT_EQ(dst.Count(), 3u);
  EXPECT_EQ(WriteBatchInternal::Sequence(&dst), 200u);
  EXPECT_EQ(PrintContents(dst), "Put(a, va)@200Put(b, vb)@201Delete(c)@202");
  EXPECT_EQ(WriteBatchInternal::PayloadBytes(&dst), 3u + 3u + 1u);
  // The source is only read.
  EXPECT_EQ(src.Count(), 2u);
  EXPECT_EQ(PrintContents(src), "Put(b, vb)@900Delete(c)@901");
}

TEST(WriteBatchTest, AppendCarriesTheOversizeFlag) {
  WriteBatch dst;
  dst.Put("a", "va");
  WriteBatch src;
  src.Put(std::string(kMaxKeySize + 1, 'k'), "v");
  ASSERT_FALSE(WriteBatchInternal::HasOversizedEntry(&dst));
  WriteBatchInternal::Append(&dst, &src);
  EXPECT_TRUE(WriteBatchInternal::HasOversizedEntry(&dst));
}

TEST(WriteBatchTest, AppendingAnEmptyBatchChangesNothing) {
  WriteBatch dst = ThreeOps();
  const std::string before(WriteBatchInternal::Contents(&dst));
  const WriteBatch empty;
  WriteBatchInternal::Append(&dst, &empty);
  EXPECT_EQ(std::string(WriteBatchInternal::Contents(&dst)), before);
}

TEST(WriteBatchTest, PayloadBytesCountsKeysAndValues) {
  WriteBatch b;
  b.Put("ab", "cde");
  b.Delete("f");
  EXPECT_EQ(WriteBatchInternal::PayloadBytes(&b), 6u);
}

TEST(WriteBatchTest, OversizeFlagAtTheLimits) {
  // One buffer, viewed at the limit and one byte past it, so that the test does not hold several
  // copies of a value that is 64 MiB.
  const std::string big_key(kMaxKeySize + 1, 'k');
  const std::string big_value(kMaxValueSize + 1, 'v');
  const std::string_view key_at_limit = std::string_view(big_key).substr(0, kMaxKeySize);
  const std::string_view value_at_limit = std::string_view(big_value).substr(0, kMaxValueSize);
  {
    WriteBatch b;
    b.Put(key_at_limit, "v");
    EXPECT_FALSE(WriteBatchInternal::HasOversizedEntry(&b)) << "key of " << kMaxKeySize;
  }
  {
    WriteBatch b;
    b.Put(big_key, "v");
    EXPECT_TRUE(WriteBatchInternal::HasOversizedEntry(&b)) << "key of " << kMaxKeySize + 1;
  }
  {
    WriteBatch b;
    b.Delete(key_at_limit);
    EXPECT_FALSE(WriteBatchInternal::HasOversizedEntry(&b)) << "deletion of " << kMaxKeySize;
  }
  {
    WriteBatch b;
    b.Delete(big_key);
    EXPECT_TRUE(WriteBatchInternal::HasOversizedEntry(&b)) << "a deletion's key is limited too";
  }
  {
    WriteBatch b;
    b.Put("k", value_at_limit);
    EXPECT_FALSE(WriteBatchInternal::HasOversizedEntry(&b)) << "value of " << kMaxValueSize;
  }
  {
    WriteBatch b;
    b.Put("k", big_value);
    EXPECT_TRUE(WriteBatchInternal::HasOversizedEntry(&b)) << "value of " << kMaxValueSize + 1;
    // The batch accepts the entry; refusing it is the write path's job, and it needs the flag.
    EXPECT_EQ(b.Count(), 1u);
  }
}

TEST(WriteBatchTest, OneOversizedEntryFlagsTheWholeBatch) {
  WriteBatch b;
  b.Put("a", "va");
  b.Put(std::string(kMaxKeySize + 1, 'k'), "v");
  b.Put("b", "vb");
  EXPECT_TRUE(WriteBatchInternal::HasOversizedEntry(&b));
}

TEST(WriteBatchTest, AnEntryTooLargeToEncodeIsFlaggedAndNotStored) {
  // A length-prefixed string cannot be longer than 4 GiB, so such an entry cannot be written to the
  // batch at all. It must still come back as an oversized batch, which the write path rejects with
  // InvalidArgument, and not abort the caller. The view is never read: it claims more bytes than
  // exist, and the batch looks only at its length.
  const char byte = 'x';
  const size_t huge = (size_t{1} << 32) + 1;
  WriteBatch b;
  b.Put("k", std::string_view(&byte, huge));
  EXPECT_TRUE(WriteBatchInternal::HasOversizedEntry(&b));
  EXPECT_EQ(WriteBatchInternal::PayloadBytes(&b), 1u + huge);
  EXPECT_EQ(b.Count(), 0u);
  EXPECT_EQ(b.ApproximateSize(), 12u);

  // The same for a deletion. Again the view lies about its length and is never read.
  WriteBatch d;
  d.Delete(std::string_view(&byte, huge));
  EXPECT_TRUE(WriteBatchInternal::HasOversizedEntry(&d));
  EXPECT_EQ(d.Count(), 0u);
}

// The count in the header is 32 bits. A batch with 2^32 operations is at least 8 GiB, so none is
// ever built; these tests set the count by hand and add one operation to it.
constexpr uint32_t kMaxCount = std::numeric_limits<uint32_t>::max();

TEST(WriteBatchTest, TheCountMayReachItsLimit) {
  WriteBatch b;
  WriteBatchInternal::SetCount(&b, kMaxCount - 1);
  b.Put("k", "v");
  EXPECT_EQ(b.Count(), kMaxCount) << "the last operation the header can count must be accepted";

  WriteBatch d;
  WriteBatchInternal::SetCount(&d, kMaxCount - 1);
  d.Delete("k");
  EXPECT_EQ(d.Count(), kMaxCount);
}

TEST(WriteBatchTest, AppendMayReachTheCountLimit) {
  WriteBatch dst;
  WriteBatchInternal::SetCount(&dst, kMaxCount - 1);
  WriteBatch src;
  src.Put("k", "v");
  WriteBatchInternal::Append(&dst, &src);
  EXPECT_EQ(dst.Count(), kMaxCount);
}

TEST(WriteBatchDeathTest, APutPastTheCountLimitAborts) {
  EXPECT_DEATH(
      {
        WriteBatch b;
        WriteBatchInternal::SetCount(&b, kMaxCount);
        b.Put("k", "v");
      },
      "WriteBatch cannot hold more than");
}

TEST(WriteBatchDeathTest, ADeletePastTheCountLimitAborts) {
  EXPECT_DEATH(
      {
        WriteBatch b;
        WriteBatchInternal::SetCount(&b, kMaxCount);
        b.Delete("k");
      },
      "WriteBatch cannot hold more than");
}

TEST(WriteBatchDeathTest, AnAppendPastTheCountLimitAborts) {
  EXPECT_DEATH(
      {
        WriteBatch dst;
        WriteBatchInternal::SetCount(&dst, kMaxCount);
        WriteBatch src;
        src.Put("k", "v");
        WriteBatchInternal::Append(&dst, &src);
      },
      "appending would give a WriteBatch more than");
}

TEST(WriteBatchDeathTest, AnAppendOneOperationPastTheLimitAborts) {
  // The other side of the boundary AppendMayReachTheCountLimit finds: counts that add up to one
  // more than the header can hold.
  EXPECT_DEATH(
      {
        WriteBatch dst;
        WriteBatchInternal::SetCount(&dst, kMaxCount - 1);
        WriteBatch src;
        src.Put("a", "1");
        src.Put("b", "2");
        WriteBatchInternal::Append(&dst, &src);
      },
      "appending would give a WriteBatch more than");
}

TEST(WriteBatchTest, ClearResets) {
  WriteBatch b = ThreeOps();
  b.Put(std::string(kMaxKeySize + 1, 'k'), "v");
  b.Clear();
  EXPECT_EQ(b.Count(), 0u);
  EXPECT_EQ(WriteBatchInternal::ByteSize(&b), 12u);
  EXPECT_EQ(WriteBatchInternal::Sequence(&b), 0u);
  EXPECT_EQ(WriteBatchInternal::PayloadBytes(&b), 0u);
  EXPECT_FALSE(WriteBatchInternal::HasOversizedEntry(&b));
  EXPECT_EQ(PrintContents(b), "");

  b.Put("x", "y");
  EXPECT_EQ(b.Count(), 1u);
  EXPECT_EQ(PrintContents(b), "Put(x, y)@0");
}

// A batch holding the three operations of ThreeOps() plus an oversized key, so that every field a
// move has to reset is non-zero.
WriteBatch BatchWithEveryFieldSet() {
  WriteBatch b = ThreeOps();
  b.Put(std::string(kMaxKeySize + 1, 'k'), "v");
  return b;
}

// Checks that `b` is what a new batch is: a bare header, no operations, nothing to iterate.
void ExpectValidEmptyBatch(const WriteBatch& b) {
  EXPECT_EQ(b.Count(), 0u);
  EXPECT_EQ(b.ApproximateSize(), 12u);
  EXPECT_EQ(WriteBatchInternal::ByteSize(&b), 12u);
  EXPECT_EQ(WriteBatchInternal::Sequence(&b), 0u);
  EXPECT_EQ(WriteBatchInternal::PayloadBytes(&b), 0u);
  EXPECT_FALSE(WriteBatchInternal::HasOversizedEntry(&b));
  Recorder recorder;
  EXPECT_TRUE(b.Iterate(&recorder).ok());
  EXPECT_TRUE(recorder.calls.empty());
  EXPECT_EQ(PrintContents(b), "");
}

// The moves must not throw: a std::vector of batches relocates them with a move only if it cannot.
static_assert(std::is_nothrow_move_constructible_v<WriteBatch>);
static_assert(std::is_nothrow_move_assignable_v<WriteBatch>);

TEST(WriteBatchTest, MovedFromBatchIsAValidEmptyBatch) {
  WriteBatch from = BatchWithEveryFieldSet();
  const WriteBatch to(std::move(from));
  static_cast<void>(to);
  ExpectValidEmptyBatch(from);
}

TEST(WriteBatchTest, MovedFromBatchCanBeReused) {
  WriteBatch from = BatchWithEveryFieldSet();
  const WriteBatch to(std::move(from));
  static_cast<void>(to);
  from.Put("x", "y");
  EXPECT_EQ(from.Count(), 1u);
  EXPECT_EQ(PrintContents(from), "Put(x, y)@0");
}

TEST(WriteBatchTest, MoveConstructedBatchHoldsTheOriginalContents) {
  WriteBatch from = BatchWithEveryFieldSet();
  const std::string bytes(WriteBatchInternal::Contents(&from));
  const uint64_t payload = WriteBatchInternal::PayloadBytes(&from);
  ASSERT_GT(payload, 0u);
  const std::string printed = PrintContents(from);

  const WriteBatch to(std::move(from));
  EXPECT_EQ(WriteBatchInternal::Contents(&to), bytes);
  EXPECT_EQ(to.Count(), 4u);
  EXPECT_EQ(WriteBatchInternal::Sequence(&to), 200u);
  EXPECT_EQ(WriteBatchInternal::PayloadBytes(&to), payload);
  EXPECT_TRUE(WriteBatchInternal::HasOversizedEntry(&to));
  EXPECT_EQ(PrintContents(to), printed);
}

TEST(WriteBatchTest, MoveAssignedFromBatchIsAValidEmptyBatch) {
  WriteBatch from = BatchWithEveryFieldSet();
  WriteBatch to;
  to.Put("old", "contents");
  to = std::move(from);
  ExpectValidEmptyBatch(from);
}

TEST(WriteBatchTest, MoveAssignedFromBatchCanBeReused) {
  WriteBatch from = BatchWithEveryFieldSet();
  WriteBatch to;
  to = std::move(from);
  from.Put("x", "y");
  EXPECT_EQ(from.Count(), 1u);
  EXPECT_EQ(PrintContents(from), "Put(x, y)@0");
}

TEST(WriteBatchTest, MoveAssignedBatchReplacesWhatItHeldWithTheOriginalContents) {
  WriteBatch from = BatchWithEveryFieldSet();
  const std::string bytes(WriteBatchInternal::Contents(&from));
  const uint64_t payload = WriteBatchInternal::PayloadBytes(&from);
  const std::string printed = PrintContents(from);

  WriteBatch to;
  to.Put("old", "contents");
  to = std::move(from);
  EXPECT_EQ(WriteBatchInternal::Contents(&to), bytes);
  EXPECT_EQ(to.Count(), 4u);
  EXPECT_EQ(WriteBatchInternal::Sequence(&to), 200u);
  EXPECT_EQ(WriteBatchInternal::PayloadBytes(&to), payload);
  EXPECT_TRUE(WriteBatchInternal::HasOversizedEntry(&to));
  EXPECT_EQ(PrintContents(to), printed);
}

TEST(WriteBatchTest, CopiesStayIndependent) {
  WriteBatch original = ThreeOps();
  WriteBatch copy(original);
  copy.Put("extra", "op");
  EXPECT_EQ(original.Count(), 3u);
  EXPECT_EQ(copy.Count(), 4u);

  WriteBatch assigned;
  assigned = original;
  assigned.Delete("extra");
  EXPECT_EQ(original.Count(), 3u);
  EXPECT_EQ(assigned.Count(), 4u);
}

TEST(WriteBatchTest, ApproximateSizeGrowsWithEveryOperation) {
  WriteBatch b;
  size_t previous = b.ApproximateSize();
  b.Put("key", "value");
  EXPECT_GT(b.ApproximateSize(), previous);
  previous = b.ApproximateSize();
  b.Delete("key");
  EXPECT_GT(b.ApproximateSize(), previous);
  EXPECT_EQ(b.ApproximateSize(), WriteBatchInternal::ByteSize(&b));
}

TEST(WriteBatchTest, EmptyKeysAndValuesAreOrdinaryEntries) {
  WriteBatch b;
  b.Put("", "");
  b.Put("k", "");
  b.Delete("");
  WriteBatchInternal::SetSequence(&b, 1);
  Recorder recorder;
  ASSERT_OK(b.Iterate(&recorder));
  EXPECT_EQ(recorder.calls, (std::vector<std::string>{"Put(,)", "Put(k,)", "Delete()"}));
}

TEST(WriteBatchTest, BinaryKeysAndValuesSurvive) {
  const std::string key("a\0b\xff", 4);
  const std::string value("\0\x80\n", 3);
  WriteBatch b;
  b.Put(key, value);
  const WriteBatch copy = FromContents(WriteBatchInternal::Contents(&b));
  Recorder recorder;
  ASSERT_OK(copy.Iterate(&recorder));
  EXPECT_EQ(recorder.calls, (std::vector<std::string>{"Put(" + key + "," + value + ")"}));
}

TEST(WriteBatchTest, SequencesRunConsecutivelyFromTheBatchsOwn) {
  WriteBatch b;
  for (int i = 0; i < 5; i++) {
    b.Put("k" + std::to_string(i), "v");
  }
  WriteBatchInternal::SetSequence(&b, kMaxSequenceNumber - 4);
  EXPECT_EQ(PrintContents(b), "Put(k0, v)@" + std::to_string(kMaxSequenceNumber - 4) +
                                  "Put(k1, v)@" + std::to_string(kMaxSequenceNumber - 3) +
                                  "Put(k2, v)@" + std::to_string(kMaxSequenceNumber - 2) +
                                  "Put(k3, v)@" + std::to_string(kMaxSequenceNumber - 1) +
                                  "Put(k4, v)@" + std::to_string(kMaxSequenceNumber));
}

TEST(WriteBatchTest, SequenceNumbersBeyondTheLimitAreReportedNotFatal) {
  // A sequence number comes out of the log during recovery. One that does not fit in 56 bits is
  // damage, and has to come back as a Status: the memtable would abort on it.
  WriteBatch b;
  b.Put("a", "1");
  b.Put("b", "2");
  b.Put("c", "3");
  WriteBatchInternal::SetSequence(&b, kMaxSequenceNumber - 1);
  const auto mem = std::make_shared<MemTable>(1);
  const Status s = WriteBatchInternal::InsertInto(&b, mem.get());
  EXPECT_EQ(s.ToString(), "Corruption: WriteBatch sequence number out of range");
  EXPECT_EQ(PrintContents(b), "Put(a, 1)@" + std::to_string(kMaxSequenceNumber - 1) + "Put(b, 2)@" +
                                  std::to_string(kMaxSequenceNumber) + "ParseError()");

  WriteBatchInternal::SetSequence(&b, std::numeric_limits<SequenceNumber>::max());
  const auto mem2 = std::make_shared<MemTable>(1);
  EXPECT_TRUE(WriteBatchInternal::InsertInto(&b, mem2.get()).IsCorruption());
}

TEST(WriteBatchTest, InsertIntoAddsToWhatIsThere) {
  const auto mem = std::make_shared<MemTable>(1);
  WriteBatch first;
  first.Put("a", "1");
  WriteBatchInternal::SetSequence(&first, 1);
  WriteBatch second;
  second.Put("a", "2");
  second.Delete("b");
  WriteBatchInternal::SetSequence(&second, 2);
  ASSERT_OK(WriteBatchInternal::InsertInto(&first, mem.get()));
  ASSERT_OK(WriteBatchInternal::InsertInto(&second, mem.get()));

  std::string value;
  Status s;
  ASSERT_TRUE(mem->Get(LookupKey("a", 10), &value, &s));
  EXPECT_EQ(value, "2");
  ASSERT_TRUE(mem->Get(LookupKey("a", 1), &value, &s));
  EXPECT_EQ(value, "1");
  ASSERT_TRUE(mem->Get(LookupKey("b", 10), &value, &s));
  EXPECT_TRUE(s.IsNotFound());
}

}  // namespace
}  // namespace shale
