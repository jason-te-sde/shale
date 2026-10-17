#ifndef SHALE_UTIL_HASH_H_
#define SHALE_UTIL_HASH_H_

#include <cstddef>
#include <cstdint>

namespace shale {

/// Returns a 32-bit hash of the `n` bytes at `data`, mixed with `seed`.
///
/// Bloom filters are written to disk with this function, so it must return the same value for the
/// same input in every build and on every platform, forever. It is not a cryptographic hash. The
/// bytes are read as little-endian words and need not be aligned. `data` may be null when `n` is
/// zero, in which case the result is `seed`.
uint32_t Hash(const char* data, size_t n, uint32_t seed);

}  // namespace shale

#endif  // SHALE_UTIL_HASH_H_
