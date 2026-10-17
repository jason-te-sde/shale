// The bloom filter over user keys. Its bytes are written into tables, so two things must hold for
// good: a key that was added is never reported absent, because a table lookup would then miss data
// that is there, and the bytes for a given set of keys never change, because a table written
// today is read by the filter code of next year.

#include "io/bloom.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/coding.h"
#include "util/hash.h"

namespace shale {
namespace {

std::string KeyOf(uint32_t i) {
  std::string key;
  PutFixed32(&key, i);
  return key;
}

std::string BuildFilter(int bits_per_key, const std::vector<std::string>& keys) {
  BloomFilterBuilder builder(bits_per_key);
  for (const std::string& key : keys) {
    builder.AddKey(key);
  }
  return builder.Finish();
}

// The filter the format describes, written out as plainly as the description reads and sharing no
// code with the builder except the hash function, so that the two agree only if both follow it.
std::string ReferenceFilter(int bits_per_key, const std::vector<std::string>& keys) {
  if (bits_per_key == 0) {
    return "";
  }
  const int k = std::clamp(static_cast<int>(bits_per_key * 0.69), 1, 30);
  const size_t bits = std::max<size_t>(keys.size() * static_cast<size_t>(bits_per_key), 64);
  const size_t bytes = (bits + 7) / 8;
  std::string filter(bytes + 1, '\0');
  filter[bytes] = static_cast<char>(k);
  for (const std::string& key : keys) {
    uint32_t h = Hash(key.data(), key.size(), 0xbc9f1d34);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (int j = 0; j < k; ++j) {
      const size_t bit = h % (bytes * 8);
      filter[bit / 8] = static_cast<char>(filter[bit / 8] | (1 << (bit % 8)));
      h += delta;
    }
  }
  return filter;
}

TEST(BloomTest, EmptyFilterMatchesNothing) {
  const std::string filter = BuildFilter(10, {});
  EXPECT_FALSE(BloomFilterMayMatch(filter, "hello"));
  EXPECT_FALSE(BloomFilterMayMatch(filter, ""));
}

TEST(BloomTest, DisabledFilterMatchesEverything) {
  BloomFilterBuilder builder(0);
  builder.AddKey("hello");
  const std::string filter = builder.Finish();
  EXPECT_EQ(filter, "");
  EXPECT_TRUE(BloomFilterMayMatch(filter, "hello"));
  EXPECT_TRUE(BloomFilterMayMatch(filter, "anything"));
}

TEST(BloomTest, MalformedFilterFailsOpen) {
  // A filter that cannot be read answers "maybe" to everything, so a damaged one costs a lookup
  // in the table and never a missing key.
  EXPECT_TRUE(BloomFilterMayMatch("x", "hello")) << "one byte has no bit array";

  std::string too_many_probes(8, '\0');
  too_many_probes.push_back(static_cast<char>(31));
  EXPECT_EQ(too_many_probes.size(), 9U);
  EXPECT_TRUE(BloomFilterMayMatch(too_many_probes, "hello")) << "k above 30 is not one of ours";

  std::string no_probes(8, '\0');
  no_probes.push_back(static_cast<char>(0));
  EXPECT_TRUE(BloomFilterMayMatch(no_probes, "hello")) << "k of 0 tests no bit";

  std::string max_probes(8, '\0');
  max_probes.push_back(static_cast<char>(30));
  EXPECT_FALSE(BloomFilterMayMatch(max_probes, "hello")) << "30 is the most, and is valid";
}

TEST(BloomTest, SmallSet) {
  const std::string filter = BuildFilter(10, {"hello", "world"});
  EXPECT_TRUE(BloomFilterMayMatch(filter, "hello"));
  EXPECT_TRUE(BloomFilterMayMatch(filter, "world"));
  EXPECT_FALSE(BloomFilterMayMatch(filter, "x"));
  EXPECT_FALSE(BloomFilterMayMatch(filter, "foo"));
}

TEST(BloomTest, EmptyKeyIsAKey) {
  const std::string filter = BuildFilter(10, {"", "a"});
  EXPECT_TRUE(BloomFilterMayMatch(filter, ""));
  EXPECT_TRUE(BloomFilterMayMatch(filter, "a"));
}

TEST(BloomTest, ProbeCountFollowsBitsPerKey) {
  // k is int(bits_per_key * 0.69) kept between 1 and 30, and is the last byte of the filter.
  const std::vector<std::pair<int, int>> expected = {{1, 1},   {2, 1},   {10, 6},
                                                     {20, 13}, {44, 30}, {100, 30}};
  for (const auto& [bits_per_key, k] : expected) {
    SCOPED_TRACE("bits per key " + std::to_string(bits_per_key));
    const std::string filter = BuildFilter(bits_per_key, {"a", "b", "c"});
    EXPECT_EQ(static_cast<uint8_t>(filter.back()), k);
  }
}

TEST(BloomTest, BytesFollowTheFormat) {
  // The same bytes as a plain reading of the format gives, for several sizes: below the 64-bit
  // minimum, just above it, and well above, where the size is not a multiple of 8 bits per key.
  for (const int bits_per_key : {1, 7, 10, 13}) {
    for (const uint32_t n : {0U, 1U, 5U, 6U, 7U, 100U, 333U}) {
      SCOPED_TRACE("bits per key " + std::to_string(bits_per_key) + ", " + std::to_string(n) +
                   " keys");
      std::vector<std::string> keys;
      for (uint32_t i = 0; i < n; ++i) {
        keys.push_back(KeyOf(i * 7919));
      }
      EXPECT_EQ(BuildFilter(bits_per_key, keys), ReferenceFilter(bits_per_key, keys));
    }
  }
}

TEST(BloomTest, DuplicateKeysAndFinishingTwice) {
  BloomFilterBuilder builder(10);
  builder.AddKey("a");
  builder.AddKey("a");
  builder.AddKey("b");
  const std::string first = builder.Finish();
  EXPECT_EQ(builder.Finish(), first) << "Finish() does not consume the keys";
  EXPECT_EQ(first, ReferenceFilter(10, {"a", "a", "b"}));
}

TEST(BloomTest, VaryingLengthsHaveNoFalseNegativesAndLowFalsePositives) {
  std::vector<uint32_t> lengths;
  for (uint32_t base : {1U, 10U, 100U, 1000U}) {
    for (uint32_t i = 1; i <= 9; ++i) {
      lengths.push_back(base * i);
    }
  }
  lengths.push_back(10000);

  for (const uint32_t n : lengths) {
    SCOPED_TRACE("n = " + std::to_string(n));
    BloomFilterBuilder builder(10);
    for (uint32_t i = 0; i < n; ++i) {
      builder.AddKey(KeyOf(i));
    }
    const std::string filter = builder.Finish();

    EXPECT_LE(filter.size(), size_t{n} * 10 / 8 + 40) << "the filter is about 10 bits per key";
    for (uint32_t i = 0; i < n; ++i) {
      ASSERT_TRUE(BloomFilterMayMatch(filter, KeyOf(i))) << "false negative for key " << i;
    }

    constexpr uint32_t kProbes = 10000;
    uint32_t false_positives = 0;
    for (uint32_t i = 0; i < kProbes; ++i) {
      if (BloomFilterMayMatch(filter, KeyOf(i + 1000000000U))) {
        ++false_positives;
      }
    }
    const double rate = static_cast<double>(false_positives) / kProbes;
    EXPECT_LE(rate, 0.02) << false_positives << " false positives in " << kProbes;
  }
}

TEST(BloomDeathTest, NegativeBitsPerKeyAborts) {
  EXPECT_DEATH(BloomFilterBuilder(-1), "bits_per_key");
}

}  // namespace
}  // namespace shale
