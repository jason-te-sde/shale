#ifndef SHALE_IO_BLOCK_H_
#define SHALE_IO_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "io/table_format.h"
#include "shale/iterator.h"
#include "util/dbformat.h"

namespace shale {

/// A data block as it was read from a table, ready to be iterated. The layout is the one
/// BlockBuilder writes.
///
/// A block comes off the disk, and the decoder must not trust it: whatever the bytes are, reading
/// them stays inside the block, and what is wrong with them comes back as a Corruption status on
/// the iterator, never as a crash. A Block is immutable, so any number of iterators over it may
/// run on different threads.
class Block {
 public:
  /// Takes ownership of the block's bytes. Never fails: a block whose structure is damaged yields
  /// iterators that are invalid with a Corruption status, because the block is cached and read
  /// from many places, and the error belongs to the reads and not to loading it.
  ///
  /// The structure is damaged, and reads fail with "bad block contents", if the block is shorter
  /// than the 4-byte restart count, if the restart array the count describes does not fit in the
  /// block, if there are no restart points, or if the first one is not at the start of the entries.
  /// Damage inside the entries is found as they are read.
  explicit Block(BlockContents contents);

  /// Copies `contents` and makes a block of the copy, as the constructor above does: for a block
  /// that was built in memory, by a test, a fuzz target or a benchmark, rather than read.
  explicit Block(std::string_view contents) : Block(BlockContents::CopyOf(contents)) {}

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  /// Returns the size of the block in bytes, as it was given.
  size_t size() const { return contents_.size(); }

 private:
  friend std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const Block> block,
                                                    KeyCompareFn cmp, size_t min_key_size);
  class Iter;

  const BlockContents contents_;
  // Where the entries end and the restart array begins. Zero, with no restart points, for a block
  // whose structure is damaged, which then has no entries to read.
  size_t restart_offset_ = 0;
  uint32_t num_restarts_ = 0;
  bool malformed_ = false;
};

/// Returns an iterator over the entries of `block`, in the order they were added. The iterator
/// shares ownership of the block, so it stays valid after every other owner is gone. Seek() finds
/// the first entry whose key `cmp` does not order before the target, which assumes `cmp` orders
/// the keys the way the block was built.
///
/// `cmp` is called with keys as they are stored, and what is stored is whatever the file held, so
/// a comparison that cannot take every byte string, CompareInternalKey() among them, needs
/// `min_key_size`: an entry whose whole key is shorter ends the iteration with Corruption before
/// the key reaches `cmp`, whether it is met walking the entries or probing a restart point.
///
/// A block damaged in a way the decoder can see ends the iteration with a Corruption status:
/// the entries before the damage are returned, and the iterator then stays invalid, so that a
/// caller that only watches Valid() is told by status() that it did not reach the end of the
/// data.
/// @param block the block to read.
/// @param cmp the order of the block's keys.
/// @param min_key_size the length in bytes below which a key is damage; 0 accepts any key. A
///   reader of internal keys passes 8, the size of the tag that ends one.
std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const Block> block, KeyCompareFn cmp,
                                           size_t min_key_size = 0);

}  // namespace shale

#endif  // SHALE_IO_BLOCK_H_
