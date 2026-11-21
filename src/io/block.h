#ifndef SHALE_IO_BLOCK_H_
#define SHALE_IO_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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
  friend class BlockIterator;

  const BlockContents contents_;
  // Where the entries end and the restart array begins. Zero, with no restart points, for a block
  // whose structure is damaged, which then has no entries to read.
  size_t restart_offset_ = 0;
  uint32_t num_restarts_ = 0;
  bool malformed_ = false;
};

/// Reads the entries of data blocks, one block at a time, and can be pointed at another block
/// without being made again: the table's iterator keeps one for the index and one for the data
/// block it is in. NewBlockIterator() makes one behind the Iterator interface.
///
/// `status()` is sticky until the iterator is given a block again: once an entry has been found
/// damaged the iterator stays invalid, so a seek cannot return to entries before the damage and
/// leave a status that says all is well.
///
/// The iterator keeps the key it is at, and the one before it, each whole: it builds a key from the
/// shared prefix of the current one into the other of two buffers, and then switches. The key
/// before is what a reader that checks the order of the keys compares against, without a copy of
/// its own.
class BlockIterator final : public Iterator {
 public:
  /// An iterator over no block: it is invalid, and its status is OK, until Reset() or Continue().
  BlockIterator(KeyCompareFn cmp, size_t min_key_size);
  /// An iterator over `block`; see NewBlockIterator() for `cmp` and `min_key_size`.
  BlockIterator(std::shared_ptr<const Block> block, KeyCompareFn cmp, size_t min_key_size);

  /// Makes the iterator a new one over `block`: invalid, with no previous key, and with the status
  /// of the block and not of what was read before.
  void Reset(std::shared_ptr<const Block> block);
  /// As Reset(), except that the last key the iterator was at is kept, for previous_key() after the
  /// next SeekToFirst(). If the iterator had run off the end of its block, that is the block's
  /// last key. A block with no entries leaves the key that was kept as it is.
  void Continue(std::shared_ptr<const Block> block);

  /// Whether the current entry has a key before it that previous_key() returns: it does after a
  /// Next(), and after the first SeekToFirst() that follows a Continue() that had a key to keep. It
  /// does not after a Seek(), a Reset(), or any other SeekToFirst().
  bool has_previous_key() const { return has_previous_ && Valid(); }
  /// The key before the current one. Requires has_previous_key(). The view is valid until the
  /// iterator moves.
  std::string_view previous_key() const;

  bool Valid() const override { return current_ < entries_end_; }
  void SeekToFirst() override;
  void Seek(std::string_view target) override;
  void Next() override;
  std::string_view key() const override;
  std::string_view value() const override;
  Status status() const override { return status_; }

 private:
  size_t RestartOffset(uint32_t index) const;
  void StartAt(size_t offset);
  bool ReadRestartKey(uint32_t index, std::string_view* key);
  bool ParseNextEntry(bool has_previous);
  void Fail();
  void Load(std::shared_ptr<const Block> block);

  std::shared_ptr<const Block> block_;
  const KeyCompareFn cmp_;
  const size_t min_key_size_;
  const char* data_ = nullptr;
  size_t entries_end_ = 0;
  uint32_t num_restarts_ = 0;
  size_t current_ = 0;  // Offset of the current entry, or entries_end_ when there is none.
  size_t next_ = 0;     // Offset of the entry after it.
  // The key of the current entry is keys_[current_key_], and, when has_previous_, the one before
  // it is the other. `shareable_` is how many bytes of keys_[current_key_] the next entry may take
  // as its shared prefix: all of it while stepping, none at the start of a block.
  std::string keys_[2];
  int current_key_ = 0;
  size_t shareable_ = 0;
  bool has_previous_ = false;
  bool has_last_key_ = false;  // keys_[current_key_] holds the last key the iterator was at
  bool carry_ = false;         // the next SeekToFirst() has the kept key as its previous key
  std::string_view value_;
  Status status_;
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
