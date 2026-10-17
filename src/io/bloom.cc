#include "io/bloom.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "util/check.h"
#include "util/hash.h"

namespace shale {

namespace {

// Part of the format: the seed the keys are hashed with.
constexpr uint32_t kHashSeed = 0xbc9f1d34;

// The most probes a filter may use. A reader treats a larger count as a filter it does not know.
constexpr int kMaxProbes = 30;

// The smallest bit array. A filter over a few keys would otherwise be so small that nearly every
// key matched.
constexpr size_t kMinBits = 64;

uint32_t HashKey(std::string_view key) { return Hash(key.data(), key.size(), kHashSeed); }

// The step between one probe and the next: the hash rotated left by 15 bits.
uint32_t ProbeDelta(uint32_t h) { return (h >> 17) | (h << 15); }

}  // namespace

BloomFilterBuilder::BloomFilterBuilder(int bits_per_key)
    : bits_per_key_(bits_per_key),
      probes_(bits_per_key == 0
                  ? 0
                  : std::clamp(static_cast<int>(static_cast<double>(bits_per_key) * 0.69), 1,
                               kMaxProbes)) {
  SHALE_CHECK(bits_per_key >= 0, "BloomFilterBuilder needs bits_per_key >= 0");
}

void BloomFilterBuilder::AddKey(std::string_view user_key) {
  if (probes_ == 0) {
    return;  // The filter is off, and nothing will be built from the keys.
  }
  hashes_.push_back(HashKey(user_key));
}

std::string BloomFilterBuilder::Finish() const {
  if (probes_ == 0) {
    return "";
  }
  const size_t bits = std::max(hashes_.size() * static_cast<size_t>(bits_per_key_), kMinBits);
  const size_t bytes = (bits + 7) / 8;
  // The array is sized in whole bytes, and the bits in the last one belong to it.
  const size_t array_bits = bytes * 8;

  std::string filter(bytes + 1, '\0');
  filter[bytes] = static_cast<char>(probes_);
  for (uint32_t h : hashes_) {
    const uint32_t delta = ProbeDelta(h);
    for (int j = 0; j < probes_; ++j) {
      const size_t bit = h % array_bits;
      filter[bit / 8] = static_cast<char>(filter[bit / 8] | (1 << (bit % 8)));
      h += delta;
    }
  }
  return filter;
}

bool BloomFilterMayMatch(std::string_view filter, std::string_view user_key) {
  if (filter.size() < 2) {
    return true;  // No bit array: empty means the filter is off, and one byte is not a filter.
  }
  const size_t array_bits = (filter.size() - 1) * 8;
  const int probes = static_cast<uint8_t>(filter.back());
  if (probes > kMaxProbes) {
    return true;  // Not a count the builder writes; fail open.
  }

  uint32_t h = HashKey(user_key);
  const uint32_t delta = ProbeDelta(h);
  for (int j = 0; j < probes; ++j) {
    const size_t bit = h % array_bits;
    if ((static_cast<uint8_t>(filter[bit / 8]) & (1 << (bit % 8))) == 0) {
      return false;
    }
    h += delta;
  }
  return true;
}

}  // namespace shale
