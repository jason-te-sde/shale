#ifndef SHALE_TOOLS_BENCH_STATS_H_
#define SHALE_TOOLS_BENCH_STATS_H_

// The medians that shale_bench takes over the runs of a benchmark. They are in a library of their
// own so that the tests reach them without running the tool.

#include <string>
#include <vector>

namespace shale {

/// Returns the median of `values`: the middle one when there is an odd number, and the mean of the
/// two in the middle when there is an even number. Aborts if there are none.
/// @param values the values, in any order.
double Median(std::vector<double> values);

/// Returns the median of write amplification figures, which an engine reports as text and which are
/// shown as it wrote them. With an odd number, it is the middle figure, compared as numbers and
/// returned as given. With an even number, it is the mean of the two in the middle, to two places.
/// If any figure is not a number, such as "n/a", it is the first figure as it stands: there is no
/// median of what is not a number. Aborts if there are none.
/// @param figures one figure for each run.
std::string MedianFigure(const std::vector<std::string>& figures);

}  // namespace shale

#endif  // SHALE_TOOLS_BENCH_STATS_H_
