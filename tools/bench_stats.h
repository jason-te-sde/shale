#ifndef SHALE_TOOLS_BENCH_STATS_H_
#define SHALE_TOOLS_BENCH_STATS_H_

// What shale_bench decides from numbers and from its own build, apart from its command line: the
// medians over the runs of a benchmark, and whether the build's numbers are worth quoting. They are
// in a library of their own so that the tests reach them without running the tool.

#include <string>
#include <string_view>
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

/// Returns whether the numbers of a build are worth quoting: it is optimized (Release or
/// RelWithDebInfo), has no sanitizer, is not instrumented for coverage, and has no deliberate flaw
/// compiled in. A build that does not say what it is, such as one made without CMake, is not.
/// @param build_type CMake's build type, such as "Release".
/// @param sanitizer the sanitizers the build has, or the empty string.
/// @param coverage whether the build is instrumented for coverage.
/// @param flaw the deliberate flaw compiled in, or the empty string.
bool IsRepresentativeBuild(std::string_view build_type, std::string_view sanitizer, bool coverage,
                           std::string_view flaw);

}  // namespace shale

#endif  // SHALE_TOOLS_BENCH_STATS_H_
