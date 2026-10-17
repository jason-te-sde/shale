#include "util/dbformat.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "util/coding.h"

namespace shale {
namespace {

std::string InternalKey(std::string_view user_key, SequenceNumber seq, ValueType type) {
  std::string result;
  AppendInternalKey(&result, {user_key, seq, type});
  return result;
}

TEST(DbFormatTest, InternalKeyRoundTrip) {
  const std::string user_keys[] = {"", "k", std::string("\0\xff", 2), std::string(300, 'x')};
  const SequenceNumber sequences[] = {0, 1, 100, kMaxSequenceNumber};
  const ValueType types[] = {ValueType::kValue, ValueType::kDeletion};
  for (const std::string& user_key : user_keys) {
    for (SequenceNumber seq : sequences) {
      for (ValueType type : types) {
        const std::string encoded = InternalKey(user_key, seq, type);
        ASSERT_EQ(encoded.size(), user_key.size() + 8);
        ParsedInternalKey parsed;
        ASSERT_TRUE(ParseInternalKey(encoded, &parsed))
            << "user key of " << user_key.size() << " bytes, sequence " << seq;
        EXPECT_EQ(parsed.user_key, user_key);
        EXPECT_EQ(parsed.sequence, seq);
        EXPECT_EQ(parsed.type, type);
        EXPECT_EQ(ExtractUserKey(encoded), user_key);
      }
    }
  }
}

TEST(DbFormatTest, AppendLayout) {
  std::string s;
  AppendInternalKey(&s, {"foo", 100, ValueType::kValue});
  std::string want = "foo";
  PutFixed64(&want, (100u << 8) | 1);
  EXPECT_EQ(s, want);
}

TEST(DbFormatTest, AppendKeepsWhatIsAlreadyThere) {
  std::string s = "prefix";
  AppendInternalKey(&s, {"k", 1, ValueType::kDeletion});
  EXPECT_EQ(s.substr(0, 6), "prefix");
  EXPECT_EQ(s.size(), 6u + 1 + 8);
}

TEST(DbFormatTest, PackPutsTheSequenceAboveTheType) {
  EXPECT_EQ(PackSequenceAndType(0, ValueType::kDeletion), 0u);
  EXPECT_EQ(PackSequenceAndType(0, ValueType::kValue), 1u);
  EXPECT_EQ(PackSequenceAndType(5, ValueType::kValue), (uint64_t{5} << 8) | 1);
  EXPECT_EQ(PackSequenceAndType(kMaxSequenceNumber, ValueType::kValue), 0xffffffffffffff01ULL);
  EXPECT_EQ(PackSequenceAndType(kMaxSequenceNumber, ValueType::kDeletion), 0xffffffffffffff00ULL);
  EXPECT_EQ(kMaxSequenceNumber, (uint64_t{1} << 56) - 1);
}

TEST(DbFormatTest, ParseRejectsShortAndBadType) {
  ParsedInternalKey parsed;
  EXPECT_FALSE(ParseInternalKey("", &parsed));
  EXPECT_FALSE(ParseInternalKey(std::string_view("1234567", 7), &parsed));

  std::string bad_type = "k";
  PutFixed64(&bad_type, (7u << 8) | 2);
  EXPECT_FALSE(ParseInternalKey(bad_type, &parsed));

  std::string worse_type = "k";
  PutFixed64(&worse_type, (7u << 8) | 0xff);
  EXPECT_FALSE(ParseInternalKey(worse_type, &parsed));

  // Exactly the eight tag bytes: an empty user key is valid.
  const std::string empty_user_key = InternalKey("", 3, ValueType::kValue);
  EXPECT_EQ(empty_user_key.size(), 8u);
  EXPECT_TRUE(ParseInternalKey(empty_user_key, &parsed));
}

TEST(DbFormatTest, ParseLeavesTheResultAloneOnFailure) {
  ParsedInternalKey parsed{"sentinel", 42, ValueType::kDeletion};
  EXPECT_FALSE(ParseInternalKey("short", &parsed));
  EXPECT_EQ(parsed.user_key, "sentinel");
  EXPECT_EQ(parsed.sequence, 42u);
  EXPECT_EQ(parsed.type, ValueType::kDeletion);
}

TEST(DbFormatTest, NewerSequenceSortsFirst) {
  const auto cmp = [](std::string_view a_key, SequenceNumber a_seq, ValueType a_type,
                      std::string_view b_key, SequenceNumber b_seq, ValueType b_type) {
    return CompareInternalKey(InternalKey(a_key, a_seq, a_type), InternalKey(b_key, b_seq, b_type));
  };
  EXPECT_LT(cmp("a", 5, ValueType::kValue, "a", 4, ValueType::kValue), 0);
  EXPECT_GT(cmp("a", 4, ValueType::kValue, "a", 5, ValueType::kValue), 0);
  EXPECT_LT(cmp("a", 1, ValueType::kValue, "b", 100, ValueType::kValue), 0);
  EXPECT_GT(cmp("b", 100, ValueType::kValue, "a", 1, ValueType::kValue), 0);
  // At one sequence number a value sorts before a deletion, so a seek for a sequence finds the
  // value first.
  EXPECT_LT(cmp("a", 7, ValueType::kValue, "a", 7, ValueType::kDeletion), 0);
  EXPECT_GT(cmp("a", 7, ValueType::kDeletion, "a", 7, ValueType::kValue), 0);
  EXPECT_EQ(cmp("a", 7, ValueType::kValue, "a", 7, ValueType::kValue), 0);
  EXPECT_EQ(cmp("", 0, ValueType::kDeletion, "", 0, ValueType::kDeletion), 0);
}

TEST(DbFormatTest, UserKeyOrdersBeforeSequenceEvenWhenOneKeyPrefixesTheOther) {
  // "a" with the newest possible tag must still sort before "a\0" with the oldest.
  const std::string shorter = InternalKey("a", kMaxSequenceNumber, ValueType::kValue);
  const std::string longer = InternalKey(std::string_view("a\0", 2), 0, ValueType::kDeletion);
  EXPECT_LT(CompareInternalKey(shorter, longer), 0);
  EXPECT_GT(CompareInternalKey(longer, shorter), 0);
}

TEST(DbFormatTest, CompareReturnsExactlyMinusOneZeroOrOne) {
  const std::string a = InternalKey("a", 9, ValueType::kValue);
  const std::string b = InternalKey("zzzz", 1, ValueType::kValue);
  EXPECT_EQ(CompareInternalKey(a, b), -1);
  EXPECT_EQ(CompareInternalKey(b, a), 1);
  EXPECT_EQ(CompareInternalKey(a, a), 0);
  EXPECT_EQ(BytewiseCompare("a", "zzzz"), -1);
  EXPECT_EQ(BytewiseCompare("zzzz", "a"), 1);
  EXPECT_EQ(BytewiseCompare("a", "a"), 0);
}

TEST(DbFormatTest, BytewiseOrderIsUnsigned) {
  EXPECT_LT(BytewiseCompare("\x7f", "\x80"), 0);
  EXPECT_LT(BytewiseCompare("", std::string_view("\0", 1)), 0);
  EXPECT_LT(BytewiseCompare("a", std::string_view("a\0", 2)), 0);
  EXPECT_EQ(BytewiseCompare("ab", "ab"), 0);
  EXPECT_GT(BytewiseCompare("\xff", "\x01"), 0);
  EXPECT_EQ(BytewiseCompare("", ""), 0);
  EXPECT_EQ(BytewiseCompare(std::string_view(), std::string_view()), 0);
}

TEST(DbFormatTest, ComparatorsFitTheKeyCompareFnType) {
  KeyCompareFn fn = &BytewiseCompare;
  EXPECT_LT(fn("a", "b"), 0);
  fn = &CompareInternalKey;
  EXPECT_LT(fn(InternalKey("a", 2, ValueType::kValue), InternalKey("a", 1, ValueType::kValue)), 0);
}

TEST(DbFormatTest, LookupKeyLayout) {
  LookupKey k("k", 7);
  std::string ik;
  AppendInternalKey(&ik, {"k", 7, kValueTypeForSeek});
  EXPECT_EQ(k.internal_key(), ik);
  EXPECT_EQ(k.user_key(), "k");
  EXPECT_EQ(k.memtable_key().substr(1), ik);
  EXPECT_EQ(static_cast<uint8_t>(k.memtable_key()[0]), 9);
}

TEST(DbFormatTest, LookupKeyWithALongUserKeyUsesAMultiByteLength) {
  const std::string user_key(300, 'u');
  LookupKey k(user_key, kMaxSequenceNumber);
  std::string_view memtable_key = k.memtable_key();
  uint32_t length = 0;
  ASSERT_TRUE(GetVarint32(&memtable_key, &length));
  EXPECT_EQ(length, 308u);
  EXPECT_EQ(memtable_key, k.internal_key());
  EXPECT_EQ(k.user_key(), user_key);
  EXPECT_EQ(k.internal_key().size(), 308u);
}

TEST(DbFormatTest, LookupKeySurvivesCopyAndMove) {
  const std::string user_key(100, 'u');  // long enough to leave the string's inline buffer
  LookupKey original(user_key, 12);
  const std::string want_internal(original.internal_key());
  LookupKey copy = original;
  LookupKey moved = std::move(original);
  for (const LookupKey* k : {&copy, &moved}) {
    EXPECT_EQ(k->internal_key(), want_internal);
    EXPECT_EQ(k->user_key(), user_key);
    EXPECT_EQ(k->memtable_key().substr(1), want_internal);
  }
}

TEST(DbFormatTest, LookupKeySortsBeforeEveryEntryItCouldFind) {
  // A lookup at sequence 7 must land at or before every entry for the key with sequence <= 7, and
  // after every entry with a newer sequence.
  LookupKey k("a", 7);
  EXPECT_LE(CompareInternalKey(k.internal_key(), InternalKey("a", 7, ValueType::kValue)), 0);
  EXPECT_LE(CompareInternalKey(k.internal_key(), InternalKey("a", 7, ValueType::kDeletion)), 0);
  EXPECT_LT(CompareInternalKey(k.internal_key(), InternalKey("a", 6, ValueType::kValue)), 0);
  EXPECT_GT(CompareInternalKey(k.internal_key(), InternalKey("a", 8, ValueType::kValue)), 0);
}

TEST(DbFormatTest, TagSizeIsWhatAnInternalKeyAddsToItsUserKey) {
  static_assert(kInternalKeyTagSize == sizeof(uint64_t), "the tag is a fixed64");
  for (const std::string& user_key : {std::string(), std::string("k"), std::string(300, 'x')}) {
    EXPECT_EQ(InternalKey(user_key, 5, ValueType::kValue).size(),
              user_key.size() + kInternalKeyTagSize);
    const LookupKey lookup(user_key, 5);
    EXPECT_EQ(lookup.internal_key().size(), user_key.size() + kInternalKeyTagSize);
    EXPECT_EQ(lookup.user_key(), user_key);
  }
  // A key of exactly the tag's size is the shortest there is, and has the empty user key.
  const std::string shortest = InternalKey("", 1, ValueType::kValue);
  ASSERT_EQ(shortest.size(), kInternalKeyTagSize);
  EXPECT_EQ(ExtractUserKey(shortest), "");
}

TEST(DbFormatDeathTest, SequenceAboveTheMaximumFails) {
  EXPECT_DEATH(PackSequenceAndType(kMaxSequenceNumber + 1, ValueType::kValue), "sequence");
  std::string s;
  EXPECT_DEATH(AppendInternalKey(&s, {"k", kMaxSequenceNumber + 1, ValueType::kValue}), "sequence");
  EXPECT_DEATH(LookupKey("k", kMaxSequenceNumber + 1), "sequence");
}

TEST(DbFormatDeathTest, ExtractUserKeyOfAShortKeyFails) {
  EXPECT_DEATH(ExtractUserKey("short"), "shorter than its 8-byte tag");
}

}  // namespace
}  // namespace shale
