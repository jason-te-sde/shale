#ifndef SHALE_IO_BLOOM_H_
#define SHALE_IO_BLOOM_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shale {

/// Builds a bloom filter over a set of user keys: a small summary of the set that can say a key is
/// definitely not in it, so that a lookup can skip a table without reading it.
///
/// A filter never says a key is absent when it was added, and says a key is present for some that
/// were not, about 1.5% of them at 10 bits per key for four-byte binary keys and about half that
/// for text keys (measured, see the constructor). The filter is
/// stored in tables, so its bytes are part of the file format. The bit array has `max(n *
/// bits_per_key, 64)` bits rounded up to whole bytes, and a last byte holds the number of probes
/// `k`, which is `bits_per_key * 0.69` rounded down and kept between 1 and 30. A key is hashed once
/// with Hash() and seed 0xbc9f1d34 to `h`; its `k` bits are `h % bits`, then `h` plus `delta` and
/// so on, where `delta = (h >> 17) | (h << 15)` and the addition wraps at 32 bits. This is double
/// hashing: one hash value stands in for `k` independent ones.
///
/// It is not thread-safe.
class BloomFilterBuilder {
 public:
  /// Creates a builder for a filter of `bits_per_key` bits for each key added. More bits give
  /// fewer false positives. Measured with 100000 keys, 10 bits per key lets about 1.5% of absent
  /// keys through, and 15 bits about 0.3%. Aborts if `bits_per_key` is negative.
  /// @param bits_per_key bits of filter per key; 0 turns the filter off, and Finish() then returns
  ///   the empty string.
  explicit BloomFilterBuilder(int bits_per_key);

  /// Adds a key to the set. Adding the same key twice is harmless, though it takes space as if it
  /// were two keys.
  /// @param user_key the user key, not an internal key: a lookup asks about a user key, and every
  ///   version of it must be found by the one entry.
  void AddKey(std::string_view user_key);

  /// Returns the filter for the keys added so far: the bit array and then the byte `k`. Returns
  /// the empty string if the filter is off. Does not forget the keys, so more may be added and
  /// the filter built again.
  std::string Finish() const;

 private:
  const int bits_per_key_;
  const int probes_;              // k; 0 when the filter is off.
  std::vector<uint32_t> hashes_;  // One 32-bit hash per key added.
};

/// Returns false if `filter`, as built by BloomFilterBuilder::Finish(), shows that `user_key` was
/// not among the keys added to it, and true if it may have been.
///
/// It says true, whatever the key, for a filter it cannot read: the empty string, which is what a
/// disabled filter is, one with no bit array, and one whose probe count is above 30, which no
/// builder writes. A damaged filter then costs a read of the table and not a missing key.
/// @param filter the bytes of a filter.
/// @param user_key the user key to look for.
bool BloomFilterMayMatch(std::string_view filter, std::string_view user_key);

}  // namespace shale

#endif  // SHALE_IO_BLOOM_H_
