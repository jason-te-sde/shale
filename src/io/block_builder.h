#ifndef SHALE_IO_BLOCK_BUILDER_H_
#define SHALE_IO_BLOCK_BUILDER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shale {

/// Builds a data block: a run of key-value entries in the order the caller adds them, with each
/// key stored as the difference from the one before it.
///
/// Consecutive keys in a sorted table usually start the same way, so an entry holds only the bytes
/// of its key that differ from the previous key's. That makes a reader that has landed in the
/// middle of the block unable to rebuild a key without reading from the start, so every
/// `restart_interval`-th entry holds its whole key and its offset goes in a restart array at the
/// end of the block. A reader searches the restart points, whose keys it can read as they are, and
/// then walks forward from the one it found.
///
/// The block is laid out as the entries, then the restart offsets as fixed32 each, then the number
/// of restart points as a fixed32. An entry is three varint32s, the number of key bytes shared with
/// the previous key, the number it holds itself and the length of its value, followed by those key
/// bytes and the value. The first entry, and every entry at a restart point, shares nothing.
///
/// The builder does not know how the keys are ordered, so it cannot check that they are: adding
/// them in the order the reader will compare them in is the caller's job. It is not thread-safe.
class BlockBuilder {
 public:
  /// Creates an empty builder. Aborts if `restart_interval` is less than 1.
  /// @param restart_interval how many entries go between two restart points; 1 stores every key
  ///   whole and shares nothing.
  explicit BlockBuilder(int restart_interval);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  /// Discards everything added and starts a new block.
  void Reset();

  /// Appends an entry. Aborts after Finish() and before Reset(), and if the block would grow past
  /// what a 32-bit offset can address.
  /// @param key must sort after the key added before it, in the order the block will be read in.
  /// @param value any bytes, the empty string included.
  void Add(std::string_view key, std::string_view value);

  /// Ends the block by appending the restart array and returns the finished block. The view is
  /// valid until the next Reset() or until the builder is destroyed, and nothing can be added to
  /// the block after this.
  std::string_view Finish();

  /// Returns the size of the block Finish() would return for what has been added so far.
  size_t CurrentSizeEstimate() const;

  /// Returns true if nothing has been added since the builder was created or last reset.
  bool empty() const { return !has_entries_; }

 private:
  const int restart_interval_;
  std::string buffer_;              // The entries so far, and after Finish() the whole block.
  std::vector<uint32_t> restarts_;  // Offsets in buffer_ of the entries that hold whole keys.
  int interval_entries_ = 0;        // Entries in the current restart interval, its first included.
  bool has_entries_ = false;
  bool finished_ = false;
  std::string last_key_;  // The key added last, which the next one is compared with.
};

}  // namespace shale

#endif  // SHALE_IO_BLOCK_BUILDER_H_
