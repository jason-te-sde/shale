#include "util/coding.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "util/random.h"

namespace shale {
namespace {

TEST(CodingTest, Fixed32IsLittleEndian) {
  char b[4];
  EncodeFixed32(b, 0x04030201u);
  EXPECT_EQ(std::string(b, 4), std::string("\x01\x02\x03\x04", 4));
}

TEST(CodingTest, Fixed64IsLittleEndian) {
  char b[8];
  EncodeFixed64(b, 0x0807060504030201ULL);
  EXPECT_EQ(std::string(b, 8), std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8));
}

TEST(CodingTest, FixedRoundTrip) {
  constexpr uint64_t kSeed = 301;
  SCOPED_TRACE("seed " + std::to_string(kSeed));
  std::vector<uint64_t> values = {0,
                                  1,
                                  0xff,
                                  0x100,
                                  std::numeric_limits<uint32_t>::max(),
                                  std::numeric_limits<uint64_t>::max()};
  Random rnd(kSeed);
  for (int i = 0; i < 1000; i++) {
    values.push_back(rnd.Next());
  }

  std::string encoded;
  for (uint64_t v : values) {
    const size_t before = encoded.size();
    PutFixed32(&encoded, static_cast<uint32_t>(v));
    ASSERT_EQ(encoded.size(), before + 4);
    PutFixed64(&encoded, v);
    ASSERT_EQ(encoded.size(), before + 12);
  }

  std::string_view input(encoded);
  for (uint64_t v : values) {
    ASSERT_EQ(DecodeFixed32(input.data()), static_cast<uint32_t>(v));
    uint32_t v32;
    ASSERT_TRUE(GetFixed32(&input, &v32));
    ASSERT_EQ(v32, static_cast<uint32_t>(v));
    ASSERT_EQ(DecodeFixed64(input.data()), v);
    uint64_t v64;
    ASSERT_TRUE(GetFixed64(&input, &v64));
    ASSERT_EQ(v64, v);
  }
  EXPECT_TRUE(input.empty()) << input.size() << " bytes were left over";
}

TEST(CodingTest, PutAppendsInsteadOfOverwriting) {
  std::string s = "ab";
  PutFixed32(&s, 1);
  PutFixed64(&s, 2);
  PutVarint32(&s, 3);
  PutVarint64(&s, 4);
  PutLengthPrefixed(&s, "xyz");
  EXPECT_EQ(s.substr(0, 2), "ab");
  EXPECT_EQ(s.size(), 2u + 4 + 8 + 1 + 1 + 4);
}

TEST(CodingTest, FixedTruncationFailsAndConsumesNothing) {
  const std::string full("\x01\x02\x03\x04\x05\x06\x07\x08", 8);
  for (size_t n = 0; n < 4; n++) {
    std::string_view input(full.data(), n);
    uint32_t v = 99;
    EXPECT_FALSE(GetFixed32(&input, &v)) << n << " bytes";
    EXPECT_EQ(input.size(), n);
    EXPECT_EQ(v, 99u);
  }
  for (size_t n = 0; n < 8; n++) {
    std::string_view input(full.data(), n);
    uint64_t v = 99;
    EXPECT_FALSE(GetFixed64(&input, &v)) << n << " bytes";
    EXPECT_EQ(input.size(), n);
    EXPECT_EQ(v, 99u);
  }
}

TEST(CodingTest, Varint32KnownEncoding) {
  std::string s;
  PutVarint32(&s, 300);
  EXPECT_EQ(s, std::string("\xac\x02", 2));

  const struct {
    uint32_t value;
    std::string encoding;
  } cases[] = {
      {0, std::string("\x00", 1)},
      {127, std::string("\x7f", 1)},
      {128, std::string("\x80\x01", 2)},
      {16384, std::string("\x80\x80\x01", 3)},
      {0xffffffffu, std::string("\xff\xff\xff\xff\x0f", 5)},
  };
  for (const auto& c : cases) {
    std::string out;
    PutVarint32(&out, c.value);
    EXPECT_EQ(out, c.encoding) << c.value;
    std::string out64;
    PutVarint64(&out64, c.value);
    EXPECT_EQ(out64, c.encoding) << "as 64 bits: " << c.value;
  }
}

TEST(CodingTest, VarintRoundTripAndLength) {
  std::vector<uint64_t> values;
  for (int shift = 0; shift < 64; shift++) {
    const uint64_t power = uint64_t{1} << shift;
    values.push_back(power - 1);
    values.push_back(power);
    values.push_back(power + 1);
  }
  values.push_back(std::numeric_limits<uint64_t>::max());

  for (uint64_t v : values) {
    std::string s;
    PutVarint64(&s, v);
    ASSERT_EQ(static_cast<size_t>(VarintLength(v)), s.size()) << v;
    std::string_view input(s);
    uint64_t decoded = 0;
    ASSERT_TRUE(GetVarint64(&input, &decoded)) << v;
    ASSERT_EQ(decoded, v);
    ASSERT_TRUE(input.empty()) << v;

    if (v <= std::numeric_limits<uint32_t>::max()) {
      std::string s32;
      PutVarint32(&s32, static_cast<uint32_t>(v));
      ASSERT_EQ(s32, s) << "32 and 64 bits must agree on " << v;
      std::string_view input32(s32);
      uint32_t decoded32 = 0;
      ASSERT_TRUE(GetVarint32(&input32, &decoded32)) << v;
      ASSERT_EQ(decoded32, static_cast<uint32_t>(v));
      ASSERT_TRUE(input32.empty()) << v;
    }
  }

  std::string max;
  PutVarint64(&max, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(max.size(), 10u);
  EXPECT_EQ(VarintLength(0), 1);
  EXPECT_EQ(VarintLength(127), 1);
  EXPECT_EQ(VarintLength(128), 2);
}

TEST(CodingTest, EncodeVarint32WritesWhatPutVarint32Appends) {
  // The raw-pointer form exists for the memtable, which writes an entry straight into the arena. It
  // must produce the same bytes as the string form, and return where the next field starts.
  std::vector<uint32_t> values = {0,
                                  1,
                                  127,
                                  128,
                                  16383,
                                  16384,
                                  2097151,
                                  2097152,
                                  268435455,
                                  268435456,
                                  std::numeric_limits<uint32_t>::max()};
  Random rnd(307);
  for (int i = 0; i < 500; i++) {
    values.push_back(static_cast<uint32_t>(rnd.Next()) >> rnd.Uniform(32));
  }
  for (const uint32_t v : values) {
    std::string expected;
    PutVarint32(&expected, v);

    // Fill the buffer with a pattern first, so that a byte written past the end shows up.
    char buf[8];
    std::fill(std::begin(buf), std::end(buf), static_cast<char>(0x5a));
    char* const end = EncodeVarint32(buf, v);
    ASSERT_EQ(static_cast<size_t>(end - buf), expected.size()) << v;
    ASSERT_EQ(std::string(buf, expected.size()), expected) << v;
    for (size_t i = expected.size(); i < sizeof(buf); i++) {
      ASSERT_EQ(buf[i], static_cast<char>(0x5a)) << "wrote past the end for " << v;
    }
    uint32_t decoded = 0;
    ASSERT_EQ(GetVarint32Ptr(buf, end, &decoded), end) << v;
    ASSERT_EQ(decoded, v);
  }
}

TEST(CodingTest, VarintsConcatenate) {
  constexpr uint64_t kSeed = 301;
  SCOPED_TRACE("seed " + std::to_string(kSeed));
  Random rnd(kSeed);
  std::vector<uint64_t> values;
  std::string s;
  for (int i = 0; i < 1000; i++) {
    const uint64_t v = rnd.Next() >> rnd.Uniform(64);  // every length from 1 to 10 bytes
    values.push_back(v);
    PutVarint64(&s, v);
  }
  std::string_view input(s);
  for (size_t i = 0; i < values.size(); i++) {
    uint64_t decoded = 0;
    ASSERT_TRUE(GetVarint64(&input, &decoded)) << "value " << i;
    ASSERT_EQ(decoded, values[i]) << "value " << i;
  }
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, VarintTruncationFails) {
  std::string max32;
  PutVarint32(&max32, std::numeric_limits<uint32_t>::max());
  std::string max64;
  PutVarint64(&max64, std::numeric_limits<uint64_t>::max());

  for (size_t n = 0; n < max32.size(); n++) {
    std::string_view input(max32.data(), n);
    uint32_t v = 99;
    EXPECT_FALSE(GetVarint32(&input, &v)) << "prefix of " << n << " bytes";
    EXPECT_EQ(input.size(), n) << "a failed read must consume nothing";
    EXPECT_EQ(v, 99u) << "a failed read must not write its result";
  }
  for (size_t n = 0; n < max64.size(); n++) {
    std::string_view input(max64.data(), n);
    uint64_t v = 99;
    EXPECT_FALSE(GetVarint64(&input, &v)) << "prefix of " << n << " bytes";
    EXPECT_EQ(input.size(), n) << "a failed read must consume nothing";
    EXPECT_EQ(v, 99u) << "a failed read must not write its result";
  }
}

TEST(CodingTest, Varint32Overflow) {
  std::string_view bad("\xff\xff\xff\xff\x7f", 5);
  std::string_view ok("\xff\xff\xff\xff\x0f", 5);
  uint32_t v;
  EXPECT_FALSE(GetVarint32(&bad, &v));
  EXPECT_TRUE(GetVarint32(&ok, &v));
  EXPECT_EQ(v, 0xffffffffu);
}

TEST(CodingTest, Varint32RejectsASixthByte) {
  // The fifth byte still says "more follows", so the value cannot fit in 32 bits.
  std::string_view input("\x80\x80\x80\x80\x80\x01", 6);
  uint32_t v = 99;
  EXPECT_FALSE(GetVarint32(&input, &v));
  EXPECT_EQ(input.size(), 6u);
}

TEST(CodingTest, Varint64OverflowElevenBytes) {
  std::string eleven(11, '\x80');
  eleven.push_back('\x00');
  std::string_view input(eleven);
  uint64_t v = 99;
  EXPECT_FALSE(GetVarint64(&input, &v));
  EXPECT_EQ(input.size(), eleven.size());
}

TEST(CodingTest, Varint64RejectsATenthByteAboveOne) {
  // Nine bytes carry 63 bits, so the tenth may add only the top one.
  std::string_view over("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x02", 10);
  std::string_view ok("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", 10);
  uint64_t v = 0;
  EXPECT_FALSE(GetVarint64(&over, &v));
  EXPECT_TRUE(GetVarint64(&ok, &v));
  EXPECT_EQ(v, std::numeric_limits<uint64_t>::max());
}

TEST(CodingTest, GetVarint32PtrStopsAtTheLimit) {
  std::string encoded;
  PutVarint32(&encoded, 1u << 28);  // five bytes
  ASSERT_EQ(encoded.size(), 5u);

  uint32_t v = 0;
  const char* end = encoded.data() + encoded.size();
  EXPECT_EQ(GetVarint32Ptr(encoded.data(), end, &v), end);
  EXPECT_EQ(v, 1u << 28);

  // A buffer of exactly the prefix's size, so that a decoder that reads past the limit is caught
  // by AddressSanitizer.
  for (size_t n = 0; n < encoded.size(); n++) {
    auto exact = std::make_unique<char[]>(n + 1);
    for (size_t i = 0; i < n; i++) {
      exact[i] = encoded[i];
    }
    uint32_t untouched = 99;
    EXPECT_EQ(GetVarint32Ptr(exact.get(), exact.get() + n, &untouched), nullptr) << n << " bytes";
    EXPECT_EQ(untouched, 99u);
  }
}

TEST(CodingTest, GetVarint32PtrReturnsThePositionAfterTheVarint) {
  std::string s;
  PutVarint32(&s, 5);
  PutVarint32(&s, 300);
  PutVarint32(&s, 70000);
  const char* p = s.data();
  const char* limit = s.data() + s.size();
  uint32_t v = 0;
  p = GetVarint32Ptr(p, limit, &v);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(v, 5u);
  EXPECT_EQ(p, s.data() + 1);
  p = GetVarint32Ptr(p, limit, &v);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(v, 300u);
  EXPECT_EQ(p, s.data() + 3);
  p = GetVarint32Ptr(p, limit, &v);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(v, 70000u);
  EXPECT_EQ(p, limit);
}

TEST(CodingTest, GetVarint32PtrRejectsOverflow) {
  const std::string bad("\xff\xff\xff\xff\x7f", 5);
  uint32_t v = 99;
  EXPECT_EQ(GetVarint32Ptr(bad.data(), bad.data() + bad.size(), &v), nullptr);
  EXPECT_EQ(v, 99u);
}

TEST(CodingTest, LengthPrefixedRoundTrip) {
  const std::string values[] = {"", "a", std::string("\0\xff", 2), std::string(200, 'x')};
  std::string encoded;
  for (const std::string& v : values) {
    PutLengthPrefixed(&encoded, v);
  }
  std::string_view input(encoded);
  for (const std::string& v : values) {
    std::string_view result;
    ASSERT_TRUE(GetLengthPrefixed(&input, &result));
    ASSERT_EQ(result, v);
  }
  EXPECT_TRUE(input.empty());

  // Every truncation of the last value, from the whole length prefix being missing to one byte of
  // the value being missing, fails and leaves the input where it was.
  std::string last;
  PutLengthPrefixed(&last, values[3]);
  for (size_t n = 0; n < last.size(); n++) {
    std::string_view truncated(last.data(), n);
    std::string_view result = "untouched";
    EXPECT_FALSE(GetLengthPrefixed(&truncated, &result)) << n << " bytes";
    EXPECT_EQ(truncated.size(), n);
    EXPECT_EQ(result, "untouched");
  }
}

TEST(CodingDeathTest, LengthPrefixedValueOverFourGiBFails) {
  if (sizeof(size_t) < 8) {
    GTEST_SKIP() << "a 32-bit size_t cannot express a length over 4 GiB";
  }
  // The view is never read: the check refuses before the first byte is touched.
  static const char byte = 0;
  const std::string_view huge(&byte, (size_t{1} << 32));
  std::string out;
  EXPECT_DEATH(PutLengthPrefixed(&out, huge), "4 GiB");
}

}  // namespace
}  // namespace shale
