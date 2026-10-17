#include "util/random.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace shale {
namespace {

TEST(RandomTest, SplitMix64ReferenceValues) {
  // The first outputs of the published splitmix64 generator seeded with zero. Every seeded test in
  // the repository replays the same data on every platform only while these hold.
  Random r(0);
  EXPECT_EQ(r.Next(), 0xe220a8397b1dcdafULL);
  EXPECT_EQ(r.Next(), 0x6e789e6aa1b965f4ULL);
  EXPECT_EQ(r.Next(), 0x06c45d188009454fULL);
}

TEST(RandomTest, SameSeedSameSequence) {
  Random a(42);
  Random b(42);
  Random c(43);
  bool differs_from_other_seed = false;
  for (int i = 0; i < 1000; i++) {
    const uint64_t x = a.Next();
    EXPECT_EQ(x, b.Next()) << "draw " << i;
    differs_from_other_seed |= (x != c.Next());
  }
  EXPECT_TRUE(differs_from_other_seed);
}

TEST(RandomTest, UniformIsTheRemainderOfNext) {
  // The formula is part of the contract: a different reduction would change every seeded test
  // built on Uniform, and would do so differently on each platform.
  Random a(7);
  Random b(7);
  for (int i = 0; i < 1000; i++) {
    EXPECT_EQ(a.Uniform(1000), static_cast<uint32_t>(b.Next() % 1000)) << "draw " << i;
  }
}

TEST(RandomTest, UniformStaysInRangeAndHitsEveryValue) {
  Random r(301);
  std::vector<int> seen(10, 0);
  for (int i = 0; i < 100000; i++) {
    const uint32_t v = r.Uniform(10);
    ASSERT_LT(v, 10u) << "draw " << i;
    seen[v]++;
  }
  for (uint32_t v = 0; v < 10; v++) {
    EXPECT_GT(seen[v], 0) << "value " << v << " was never drawn";
  }
}

TEST(RandomTest, UniformOfOneIsAlwaysZero) {
  Random r(5);
  for (int i = 0; i < 100; i++) {
    ASSERT_EQ(r.Uniform(1), 0u);
  }
}

TEST(RandomTest, OneInOneIsAlwaysTrueAndOneInTenIsRoughlyATenth) {
  Random r(11);
  for (int i = 0; i < 100; i++) {
    ASSERT_TRUE(r.OneIn(1));
  }
  int hits = 0;
  for (int i = 0; i < 100000; i++) {
    hits += r.OneIn(10) ? 1 : 0;
  }
  EXPECT_GT(hits, 9000);
  EXPECT_LT(hits, 11000);
}

TEST(RandomTest, SkewedStaysBelowItsBoundAndFavoursSmallValues) {
  Random r(23);
  int small = 0;
  int large = 0;
  for (int i = 0; i < 100000; i++) {
    const uint32_t v = r.Skewed(12);
    ASSERT_LT(v, 1u << 12) << "draw " << i;
    small += (v < 16) ? 1 : 0;
    large += (v >= 2048) ? 1 : 0;
  }
  // Uniform values would put 16/4096 of the draws below 16; the skew puts several draws in ten
  // there, and still reaches the top half of the range.
  EXPECT_GT(small, 25000);
  EXPECT_GT(large, 0);
  for (int i = 0; i < 100; i++) {
    ASSERT_EQ(r.Skewed(0), 0u);
  }
}

TEST(RandomDeathTest, UniformOfZeroFails) {
  Random r(1);
  EXPECT_DEATH(r.Uniform(0), "n > 0");
}

TEST(RandomDeathTest, SkewedWithAnOutOfRangeBoundFails) {
  Random r(1);
  EXPECT_DEATH(r.Skewed(-1), "max_log");
  EXPECT_DEATH(r.Skewed(32), "max_log");
}

}  // namespace
}  // namespace shale
