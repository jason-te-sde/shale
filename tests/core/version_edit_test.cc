#include "core/version_edit.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shale/status.h"
#include "util/coding.h"
#include "util/dbformat.h"

namespace shale {
namespace {

std::string Key(std::string_view user_key, SequenceNumber seq = 1,
                ValueType type = ValueType::kValue) {
  std::string result;
  AppendInternalKey(&result, {user_key, seq, type});
  return result;
}

FileMetaData File(uint64_t number, uint64_t size, std::string_view smallest,
                  std::string_view largest) {
  FileMetaData f;
  f.number = number;
  f.file_size = size;
  f.smallest = Key(smallest, 100);
  f.largest = Key(largest, 1);
  return f;
}

// An edit with every field set. The fields are in the order the encoding writes them, which
// EveryTruncationIsCorruption relies on.
VersionEdit FullEdit() {
  VersionEdit e;
  e.log_number = 7;
  e.next_file_number = uint64_t{1} << 40;
  e.last_sequence = kMaxSequenceNumber;
  e.compact_pointers = {{1, Key("m", 50)}, {4, Key("zz", 5, ValueType::kDeletion)}};
  e.deleted_files = {{1, 7}, {2, 9}};
  e.new_files = {{0, File(12, 4096, "a", "k")},
                 {3, File(13, uint64_t{1} << 33, std::string("b\0c", 3), "q")},
                 {6, File(14, 0, "", "zzzz")}};
  return e;
}

std::string Encode(const VersionEdit& e) {
  std::string s;
  e.EncodeTo(&s);
  return s;
}

std::string ErrorOf(std::string_view bytes) {
  VersionEdit e;
  const Status s = e.DecodeFrom(bytes);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  return s.ToString();
}

TEST(VersionEditTest, RoundTripAllFields) {
  const VersionEdit original = FullEdit();
  const std::string encoded = Encode(original);

  // Decoding replaces whatever the edit held, so start from something different.
  VersionEdit decoded;
  decoded.log_number = 99;
  decoded.deleted_files = {{5, 5}};
  decoded.new_files = {{1, File(1, 1, "x", "y")}};
  const Status s = decoded.DecodeFrom(encoded);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(decoded, original);
}

TEST(VersionEditTest, EncodeAppendsToTheString) {
  std::string s = "prefix";
  FullEdit().EncodeTo(&s);
  EXPECT_EQ(s, "prefix" + Encode(FullEdit()));
}

TEST(VersionEditTest, EmptyEditEncodesToNothing) {
  EXPECT_EQ(Encode(VersionEdit{}), "");

  VersionEdit decoded = FullEdit();
  const Status s = decoded.DecodeFrom("");
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(decoded, VersionEdit{});
}

TEST(VersionEditTest, UnknownTag) {
  std::string bytes;
  PutVarint32(&bytes, 99);
  EXPECT_EQ(ErrorOf(bytes), "Corruption: VersionEdit: unknown tag 99");

  // Tag 0 is not one of ours either, and neither is a number too big for 32 bits.
  EXPECT_EQ(ErrorOf(std::string(1, '\0')), "Corruption: VersionEdit: unknown tag 0");
  bytes.clear();
  PutVarint64(&bytes, uint64_t{1} << 32);
  EXPECT_EQ(ErrorOf(bytes), "Corruption: VersionEdit: unknown tag 4294967296");
}

TEST(VersionEditTest, UnknownTagAfterValidRecords) {
  std::string bytes = Encode(FullEdit());
  PutVarint32(&bytes, 7);
  EXPECT_EQ(ErrorOf(bytes), "Corruption: VersionEdit: unknown tag 7");
}

TEST(VersionEditTest, LevelOutOfRange) {
  // Each of the three tags that carry a level, with the level cut off right after it: the level
  // is refused before anything that follows it is read.
  for (const uint32_t tag : {4u, 5u, 6u}) {
    std::string bytes;
    PutVarint32(&bytes, tag);
    PutVarint32(&bytes, 7);
    EXPECT_EQ(ErrorOf(bytes), "Corruption: VersionEdit: level 7 out of range") << "tag " << tag;
  }

  // A level wider than 32 bits is out of range too, not a truncation.
  std::string wide;
  PutVarint32(&wide, 5);
  PutVarint64(&wide, uint64_t{1} << 40);
  PutVarint64(&wide, 3);
  EXPECT_EQ(ErrorOf(wide), "Corruption: VersionEdit: level 1099511627776 out of range");
}

TEST(VersionEditTest, LastLevelIsInRange) {
  VersionEdit e;
  e.deleted_files = {{kNumLevels - 1, 3}};
  VersionEdit decoded;
  const Status s = decoded.DecodeFrom(Encode(e));
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(decoded, e);
}

TEST(VersionEditTest, EveryTruncationIsCorruption) {
  // Build the encoding one record at a time, so that the offsets where a record ends are known:
  // a prefix that ends there is a complete encoding of fewer fields and decodes.
  const VersionEdit full = FullEdit();
  std::vector<VersionEdit> records;
  {
    VersionEdit e;
    e.log_number = full.log_number;
    records.push_back(e);
  }
  {
    VersionEdit e;
    e.next_file_number = full.next_file_number;
    records.push_back(e);
  }
  {
    VersionEdit e;
    e.last_sequence = full.last_sequence;
    records.push_back(e);
  }
  for (const auto& cp : full.compact_pointers) {
    VersionEdit e;
    e.compact_pointers = {cp};
    records.push_back(e);
  }
  for (const auto& d : full.deleted_files) {
    VersionEdit e;
    e.deleted_files = {d};
    records.push_back(e);
  }
  for (const auto& f : full.new_files) {
    VersionEdit e;
    e.new_files = {f};
    records.push_back(e);
  }

  std::string joined;
  std::set<size_t> record_ends;
  for (const VersionEdit& r : records) {
    r.EncodeTo(&joined);
    record_ends.insert(joined.size());
  }
  const std::string encoded = Encode(full);
  ASSERT_EQ(joined, encoded) << "the encoding does not write the fields in the order FullEdit has";

  size_t corrupt = 0;
  for (size_t length = 1; length < encoded.size(); length++) {
    const std::string_view prefix(encoded.data(), length);
    VersionEdit e;
    const Status s = e.DecodeFrom(prefix);
    if (record_ends.count(length) != 0) {
      EXPECT_TRUE(s.ok()) << "prefix of " << length << " bytes ends a record: " << s.ToString();
    } else {
      EXPECT_TRUE(s.IsCorruption()) << "prefix of " << length << " bytes: " << s.ToString();
      EXPECT_EQ(s.ToString().find("Corruption: VersionEdit: truncated "), 0u) << s.ToString();
      corrupt++;
    }
  }
  EXPECT_GT(corrupt, 100u) << "the full edit is too small to make the test mean anything";
}

TEST(VersionEditTest, TruncationNamesTheField) {
  const auto truncated = [](const VersionEdit& e) {
    std::string bytes = Encode(e);
    bytes.pop_back();
    return ErrorOf(bytes);
  };
  VersionEdit e;
  e.log_number = 300;
  EXPECT_EQ(truncated(e), "Corruption: VersionEdit: truncated log_number");
  e = VersionEdit{};
  e.next_file_number = 300;
  EXPECT_EQ(truncated(e), "Corruption: VersionEdit: truncated next_file_number");
  e = VersionEdit{};
  e.last_sequence = 300;
  EXPECT_EQ(truncated(e), "Corruption: VersionEdit: truncated last_sequence");
  e = VersionEdit{};
  e.compact_pointers = {{1, Key("m")}};
  EXPECT_EQ(truncated(e), "Corruption: VersionEdit: truncated compact_pointer");
  e = VersionEdit{};
  e.deleted_files = {{1, 300}};
  EXPECT_EQ(truncated(e), "Corruption: VersionEdit: truncated deleted_file");
  e = VersionEdit{};
  e.new_files = {{1, File(3, 4, "a", "b")}};
  EXPECT_EQ(truncated(e), "Corruption: VersionEdit: truncated new_file");

  // A tag that is itself cut off: a continuation bit with nothing after it.
  EXPECT_EQ(ErrorOf(std::string(1, '\x80')), "Corruption: VersionEdit: truncated tag");
}

TEST(VersionEditTest, FailedDecodeLeavesTheEditAlone) {
  VersionEdit e = FullEdit();
  std::string bytes = Encode(FullEdit());
  bytes.pop_back();
  const Status s = e.DecodeFrom(bytes);
  EXPECT_TRUE(s.IsCorruption());
  EXPECT_EQ(e, FullEdit());
}

TEST(VersionEditDeathTest, LevelOutsideTheRangeIsNotEncoded) {
  VersionEdit below;
  below.deleted_files = {{-1, 3}};
  std::string out;
  EXPECT_DEATH(below.EncodeTo(&out), "level");

  VersionEdit above;
  above.new_files = {{kNumLevels, File(3, 4, "a", "b")}};
  EXPECT_DEATH(above.EncodeTo(&out), "level");

  VersionEdit pointer;
  pointer.compact_pointers = {{kNumLevels, Key("a")}};
  EXPECT_DEATH(pointer.EncodeTo(&out), "level");
}

TEST(VersionEditTest, DebugStringNamesFields) {
  VersionEdit e;
  e.log_number = 5;
  e.next_file_number = 20;
  e.last_sequence = 1234;
  e.compact_pointers = {{2, Key("mid", 9)}};
  e.deleted_files = {{1, 7}};
  e.new_files = {{3, File(12, 4096, "a", "k")}};
  const std::string s = e.DebugString();
  for (const char* want : {"log_number: 5", "next_file_number: 20", "last_sequence: 1234",
                           "add L3 #12", "delete L1 #7", "compact_pointer L2"}) {
    EXPECT_NE(s.find(want), std::string::npos) << want << " is missing from:\n" << s;
  }
  // A field that is not set is not mentioned.
  const std::string empty = VersionEdit{}.DebugString();
  EXPECT_EQ(empty.find("log_number"), std::string::npos) << empty;
}

}  // namespace
}  // namespace shale
