#ifndef SHALE_UTIL_RANDOM_H_
#define SHALE_UTIL_RANDOM_H_

#include <cstdint>

#include "util/check.h"

namespace shale {

/// A seeded pseudo-random number generator (splitmix64).
///
/// The same seed yields the same sequence on every platform and every standard library, which the
/// standard `<random>` distributions do not promise. All randomness in the repository comes from
/// here, so a seed printed by a failing test reproduces the failure anywhere. It is not
/// cryptographically secure, and it is not thread-safe: give each thread its own.
class Random {
 public:
  /// Creates a generator whose sequence is fully determined by `seed`.
  explicit Random(uint64_t seed) : state_(seed) {}

  /// Returns the next 64-bit value of the sequence.
  uint64_t Next() {
    state_ += 0x9e3779b97f4a7c15ULL;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  /// Returns a value in [0, n). `n` must be greater than zero. The reduction is a plain remainder,
  /// so the result is slightly biased toward small values when `n` does not divide 2^64; for test
  /// data that does not matter, and the exact formula keeps seeded sequences identical everywhere.
  uint32_t Uniform(uint32_t n) {
    SHALE_CHECK(n > 0, "Random::Uniform needs n > 0");
    return static_cast<uint32_t>(Next() % n);
  }

  /// Returns true about once in `n` calls. `n` must be greater than zero.
  bool OneIn(uint32_t n) { return Uniform(n) == 0; }

  /// Returns a value in [0, 2^max_log) that favours small values: it picks a bit width uniformly
  /// from [0, max_log] and then a value of at most that width. `max_log` must be in [0, 31].
  uint32_t Skewed(int max_log) {
    SHALE_CHECK(max_log >= 0 && max_log <= 31, "Random::Skewed needs max_log in [0, 31]");
    return Uniform(uint32_t{1} << Uniform(static_cast<uint32_t>(max_log) + 1));
  }

 private:
  uint64_t state_;
};

}  // namespace shale

#endif  // SHALE_UTIL_RANDOM_H_
