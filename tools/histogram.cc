#include "histogram.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>

#include "util/check.h"

namespace shale {

void Histogram::Add(double micros) {
  samples_.push_back(micros);
  sorted_ = false;
}

double Histogram::Percentile(double p) const {
  SHALE_CHECK(!samples_.empty(), "Histogram::Percentile of a histogram with no samples");
  SHALE_CHECK(p > 0.0 && p <= 100.0, "Histogram::Percentile needs p in (0, 100]");
  if (!sorted_) {
    std::sort(samples_.begin(), samples_.end());
    sorted_ = true;
  }
  // p / 100 * n is a whole number for many round p, and the product of two doubles that stand for
  // 0.99 and 100 may come out a hair above 99 and so select the 100th sample. The factor takes a
  // few units in the last place off before the ceiling; no p that anyone asks for is that close
  // above a whole rank.
  const double count = static_cast<double>(samples_.size());
  const double exact = p * count / 100.0 * (1.0 - 4.0 * DBL_EPSILON);
  const size_t rank = std::clamp(static_cast<size_t>(std::ceil(exact)), size_t{1}, samples_.size());
  return samples_[rank - 1];
}

double Histogram::Average() const {
  SHALE_CHECK(!samples_.empty(), "Histogram::Average of a histogram with no samples");
  return std::accumulate(samples_.begin(), samples_.end(), 0.0) /
         static_cast<double>(samples_.size());
}

}  // namespace shale
