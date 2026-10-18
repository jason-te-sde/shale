#include "bench_stats.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "util/check.h"

namespace shale {

double Median(std::vector<double> values) {
  SHALE_CHECK(!values.empty(), "Median of no values");
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

std::string MedianFigure(const std::vector<std::string>& figures) {
  SHALE_CHECK(!figures.empty(), "MedianFigure of no figures");
  std::vector<std::pair<double, size_t>> numbers;
  numbers.reserve(figures.size());
  for (size_t i = 0; i < figures.size(); i++) {
    // strtod and not from_chars: AppleClang 15's library has no from_chars for a double. The
    // program never changes the locale, so the decimal point is a point.
    char* end = nullptr;
    const double number = std::strtod(figures[i].c_str(), &end);
    if (figures[i].empty() || *end != '\0') {
      return figures.front();
    }
    numbers.emplace_back(number, i);
  }
  std::sort(numbers.begin(), numbers.end());
  const size_t middle = numbers.size() / 2;
  if (numbers.size() % 2 == 1) {
    return figures[numbers[middle].second];
  }
  std::array<char, 64> text{};
  std::snprintf(text.data(), text.size(), "%.2f",
                (numbers[middle - 1].first + numbers[middle].first) / 2);
  return text.data();
}

}  // namespace shale
