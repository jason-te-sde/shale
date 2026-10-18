// The medians over the runs of a benchmark, which are the figures that shale_bench prints and that
// a later comparison quotes. A median that is wrong by one position is a wrong number that looks
// like any other.

#include "bench_stats.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace shale {
namespace {

TEST(MedianTest, OneValueIsItsOwnMedian) { EXPECT_EQ(Median({42.5}), 42.5); }

TEST(MedianTest, OddCountTakesTheMiddleOneWhateverTheOrder) {
  EXPECT_EQ(Median({3, 1, 2}), 2);
  EXPECT_EQ(Median({9, 1, 5, 3, 7}), 5);
  EXPECT_EQ(Median({1, 2, 100}), 2) << "an outlier does not move it";
}

TEST(MedianTest, EvenCountTakesTheMeanOfTheTwoInTheMiddle) {
  EXPECT_EQ(Median({4, 1}), 2.5);
  EXPECT_EQ(Median({10, 40, 20, 30}), 25);
}

TEST(MedianTest, EqualValues) {
  EXPECT_EQ(Median({7, 7, 7}), 7);
  EXPECT_EQ(Median({7, 7}), 7);
}

TEST(MedianFigureTest, OneFigureIsShownAsTheEngineWroteIt) {
  EXPECT_EQ(MedianFigure({"2.10"}), "2.10");
}

TEST(MedianFigureTest, OddCountShowsTheMiddleFigureVerbatim) {
  EXPECT_EQ(MedianFigure({"3.50", "1.20", "2.10"}), "2.10");
  EXPECT_EQ(MedianFigure({"10.00", "9.5", "9.75"}), "9.75") << "compared as numbers, not as text";
}

TEST(MedianFigureTest, EvenCountShowsTheMeanOfTheMiddleTwoToTwoPlaces) {
  EXPECT_EQ(MedianFigure({"1.00", "2.00"}), "1.50");
  EXPECT_EQ(MedianFigure({"4.00", "1.00", "2.00", "3.00"}), "2.50");
  EXPECT_EQ(MedianFigure({"1.00", "1.02"}), "1.01");
  EXPECT_EQ(MedianFigure({"1.0", "2.333"}), "1.67") << "rounded to two places, not cut";
}

TEST(MedianFigureTest, AFigureThatIsNotANumberMakesItTheFirstRunsAsItStands) {
  EXPECT_EQ(MedianFigure({"n/a", "n/a", "n/a"}), "n/a");
  EXPECT_EQ(MedianFigure({"1.50", "n/a"}), "1.50");
  EXPECT_EQ(MedianFigure({"n/a", "1.50"}), "n/a");
  EXPECT_EQ(MedianFigure({""}), "");
  EXPECT_EQ(MedianFigure({"2.0x", "1.0"}), "2.0x") << "a figure with text after the number";
}

}  // namespace
}  // namespace shale
