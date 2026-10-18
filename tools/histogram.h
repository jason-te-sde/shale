#ifndef SHALE_TOOLS_HISTOGRAM_H_
#define SHALE_TOOLS_HISTOGRAM_H_

// The latency histogram of shale_bench. It is in a library of its own so that the tests reach it
// without running the tool.

#include <cstddef>
#include <vector>

namespace shale {

/// A set of latencies in microseconds that keeps every sample, so that a percentile is one of the
/// samples that were measured and not an estimate from buckets. A run of a million operations holds
/// eight megabytes, which a benchmark can spend.
///
/// It is not thread-safe, and that includes the const methods: Percentile() sorts the samples the
/// first time it is asked, and again after an Add().
class Histogram {
 public:
  /// Adds one sample.
  /// @param micros the latency, in microseconds.
  void Add(double micros);

  /// Returns the nearest-rank percentile: the ceil(p / 100 * n)-th smallest of the n samples. So
  /// Percentile(100) is the largest, and for 100 samples Percentile(99) is the 99th and
  /// Percentile(99.9) the 100th. Aborts if there are no samples, and if `p` is not in (0, 100].
  /// @param p the percentile, above 0 and at most 100.
  double Percentile(double p) const;

  /// Returns the mean of the samples. Aborts if there are none.
  double Average() const;

  /// Returns how many samples have been added.
  size_t Count() const { return samples_.size(); }

 private:
  // Sorted on demand, so that a histogram that is only added to costs nothing for the order.
  mutable std::vector<double> samples_;
  mutable bool sorted_ = true;
};

}  // namespace shale

#endif  // SHALE_TOOLS_HISTOGRAM_H_
