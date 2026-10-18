// The histogram under shale_bench's latency columns. It keeps every sample, so a percentile is a
// sample that was measured and not an estimate, and these tests pin the one rule that decides which
// sample: nearest rank.

#include "histogram.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include "util/random.h"

namespace shale {
namespace {

// A histogram of the samples given, added in that order.
Histogram Of(const std::vector<double>& samples) {
  Histogram histogram;
  for (const double sample : samples) {
    histogram.Add(sample);
  }
  return histogram;
}

// The samples 1, 2, ..., n.
std::vector<double> OneTo(size_t n) {
  std::vector<double> samples(n);
  std::iota(samples.begin(), samples.end(), 1.0);
  return samples;
}

TEST(HistogramTest, NearestRankPercentiles) {
  const Histogram histogram = Of(OneTo(100));
  EXPECT_EQ(histogram.Percentile(50), 50);
  EXPECT_EQ(histogram.Percentile(99), 99);
  EXPECT_EQ(histogram.Percentile(99.9), 100);
  EXPECT_EQ(histogram.Percentile(100), 100);
  EXPECT_DOUBLE_EQ(histogram.Average(), 50.5);
  EXPECT_EQ(histogram.Count(), 100u);
}

TEST(HistogramTest, SingleSample) {
  Histogram histogram;
  histogram.Add(7.25);
  EXPECT_EQ(histogram.Percentile(1), 7.25);
  EXPECT_EQ(histogram.Percentile(50), 7.25);
  EXPECT_EQ(histogram.Percentile(100), 7.25);
  EXPECT_DOUBLE_EQ(histogram.Average(), 7.25);
  EXPECT_EQ(histogram.Count(), 1u);
}

TEST(HistogramTest, TheOrderOfTheSamplesDoesNotMatter) {
  std::vector<double> samples = OneTo(100);
  std::reverse(samples.begin(), samples.end());
  const Histogram descending = Of(samples);
  EXPECT_EQ(descending.Percentile(50), 50);
  EXPECT_EQ(descending.Percentile(99), 99);
  EXPECT_EQ(descending.Percentile(99.9), 100);

  // A fixed shuffle, so that a failure is repeated by running the test again.
  Random random(301);
  for (size_t i = samples.size() - 1; i > 0; i--) {
    std::swap(samples[i], samples[random.Uniform(static_cast<uint32_t>(i + 1))]);
  }
  const Histogram shuffled = Of(samples);
  EXPECT_EQ(shuffled.Percentile(50), 50);
  EXPECT_EQ(shuffled.Percentile(99), 99);
  EXPECT_EQ(shuffled.Percentile(99.9), 100);
}

TEST(HistogramTest, SamplesAddedAfterAPercentileWasAskedForAreCounted) {
  Histogram histogram = Of({10, 20, 30});
  EXPECT_EQ(histogram.Percentile(100), 30);
  histogram.Add(5);
  histogram.Add(40);
  EXPECT_EQ(histogram.Count(), 5u);
  EXPECT_EQ(histogram.Percentile(100), 40);
  EXPECT_EQ(histogram.Percentile(20), 5);  // the ceil(0.2 * 5) = 1st smallest
  EXPECT_DOUBLE_EQ(histogram.Average(), 21.0);
}

TEST(HistogramTest, EqualSamplesAreEachCounted) {
  const Histogram histogram = Of({3, 3, 3, 9});
  EXPECT_EQ(histogram.Percentile(75), 3);  // the 3rd smallest
  EXPECT_EQ(histogram.Percentile(76), 9);  // the 4th
}

// The rank is ceil(p / 100 * n), and floating point must not turn a product that is a whole number
// into one that is a hair above it, which selects the next sample: in doubles, 1.1 / 100 * 3000
// is a little over 33, and 0.9 / 100 * 1000 a little over 9. The expected rank is worked out in
// whole numbers, with p in tenths of a percent, for every such p and every n up to a few thousand,
// which is more samples than any run that is meant to be fast takes of one latency.
TEST(HistogramTest, TheRankIsExactWhereTheProductIsAWholeNumber) {
  Histogram histogram;
  for (size_t n = 1; n <= 3000; n++) {
    histogram.Add(static_cast<double>(n));  // the samples are 1, 2, ..., n
    for (size_t permille = 1; permille <= 1000; permille++) {
      const size_t rank = (permille * n + 999) / 1000;
      ASSERT_EQ(histogram.Percentile(static_cast<double>(permille) / 10.0),
                static_cast<double>(rank))
          << "n=" << n << " p=" << static_cast<double>(permille) / 10.0;
    }
  }
}

using HistogramDeathTest = ::testing::Test;

TEST(HistogramDeathTest, AnEmptyHistogramHasNoPercentile) {
  const Histogram histogram;
  EXPECT_DEATH(static_cast<void>(histogram.Percentile(50)), "Histogram::Percentile");
}

TEST(HistogramDeathTest, APercentileOutsideZeroToOneHundredIsRefused) {
  const Histogram histogram = Of({1, 2, 3});
  EXPECT_DEATH(static_cast<void>(histogram.Percentile(0)), "Histogram::Percentile");
  EXPECT_DEATH(static_cast<void>(histogram.Percentile(100.5)), "Histogram::Percentile");
}

TEST(HistogramDeathTest, AnEmptyHistogramHasNoAverage) {
  const Histogram histogram;
  EXPECT_DEATH(static_cast<void>(histogram.Average()), "Histogram::Average");
}

}  // namespace
}  // namespace shale
