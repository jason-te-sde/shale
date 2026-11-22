#ifndef SHALE_IO_READAHEAD_H_
#define SHALE_IO_READAHEAD_H_

// The bytes a table iterator reads ahead of the block it is in, so that a scan reads a file in a
// few large reads and not in one small read per block.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "io/table_format.h"
#include "shale/env.h"
#include "shale/status.h"

namespace shale {

/// A buffer of the bytes of a table that follow the block an iterator has just read, kept by that
/// one iterator. Nothing in it has been checked: it is bytes as the file returned them, and a block
/// found in it is copied out and checked as if it had been read alone. The buffer is not shared, so
/// what it holds is never handed to another reader.
///
/// An iterator starts reading ahead once it has moved to the next block twice in a row without a
/// seek, which is what a scan does and a series of seeks does not. The first fill reads
/// kInitialBytes beyond the block, and each fill after it twice as many, up to kMaxBytes, as
/// RocksDB's automatic readahead does. A seek starts the sequence again, but what is buffered stays
/// usable, since the iterator may land in it.
///
/// Not thread-safe: one iterator uses it.
class Readahead {
 public:
  /// The bytes beyond the block that the first fill after a seek reads.
  static constexpr size_t kInitialBytes = size_t{8} << 10;
  /// The most bytes beyond the block that a fill reads, and the largest block, with its trailer,
  /// that is read ahead at all.
  static constexpr size_t kMaxBytes = size_t{256} << 10;
  /// The blocks an iterator reaches by stepping, after a seek, before it fills.
  static constexpr int kSequentialBlocks = 2;

  /// A seek: the iterator is no longer known to be reading in order, and the next fill reads
  /// kInitialBytes beyond its block. The buffered bytes stay usable.
  void Reset();

  /// The iterator moved on to the next index entry.
  void NoteStep();

  /// Looks for the block at `handle` in the buffer. Returns true, with `*stored` the block's
  /// contents and its trailer, only if all of them are buffered.
  bool Find(const BlockHandle& handle, std::string_view* stored) const;

  /// Returns true if the iterator has stepped kSequentialBlocks blocks since its last seek and the
  /// block at `handle`, with its trailer, is at most kMaxBytes.
  bool ShouldFill(const BlockHandle& handle) const;

  /// Replaces the buffer with one read from `file`: the block at `handle`, with its trailer, and
  /// the bytes that follow, in one Read() of as many as the amount to read ahead says, and not past
  /// `limit`. Then doubles that amount, up to kMaxBytes. The bytes that came back are what is
  /// buffered, however few. On an error from the file nothing is buffered and the error is
  /// returned.
  /// @param file the file the table is in.
  /// @param handle where the block is.
  /// @param limit the offset the read must not go past, which a caller sets to the end of the
  ///   table's data blocks so that a fill never reads the filter, the index or the footer.
  Status Fill(const RandomAccessFile& file, const BlockHandle& handle, uint64_t limit);

  /// Forgets what is buffered, and keeps the memory.
  void Discard();

  /// Returns the bytes allocated for the buffer.
  size_t capacity() const { return capacity_; }

 private:
  std::unique_ptr<char[]> buffer_;
  size_t capacity_ = 0;
  uint64_t offset_ = 0;  // where in the file the buffer starts
  size_t size_ = 0;      // the bytes of it that the file supplied
  int steps_ = 0;
  size_t ahead_ = kInitialBytes;
};

}  // namespace shale

#endif  // SHALE_IO_READAHEAD_H_
