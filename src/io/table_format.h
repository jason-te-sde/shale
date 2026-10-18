#ifndef SHALE_IO_TABLE_FORMAT_H_
#define SHALE_IO_TABLE_FORMAT_H_

// The file format of a table, below the level of its entries: where a block is, how a block is
// stored, and how a reader finds its way in from the end of the file.
//
// A table is its data blocks one after another, then a filter block, then an index block, then the
// footer. Every block is followed by a 5-byte trailer: a type byte, which is 0 for a block stored
// as it is, and the masked CRC32C of the block's contents and that type byte as a fixed32. The
// footer is the one fixed-size part, at the very end, so that a reader can start from it: it names
// the filter block and the index block, and the index block names the data blocks.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "shale/env.h"
#include "shale/status.h"

namespace shale {

/// Where a block is in a file: the offset of its first byte and the length of its contents, which
/// does not include the trailer after it.
struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;

  /// Appends the handle to `*dst` as two varint64s, the offset and then the size.
  void EncodeTo(std::string* dst) const;

  /// Reads a handle from the front of `*input` and removes its bytes from `*input`. Returns
  /// Corruption("bad block handle"), leaving `*input` as it was, if the input ends inside the
  /// handle or a varint does not fit in 64 bits. Values the handle can hold that no file could
  /// have, an offset beyond its end among them, are for the caller that knows the file's size.
  Status DecodeFrom(std::string_view* input);
};

/// The size of the trailer after every block: the type byte and the checksum.
inline constexpr size_t kBlockTrailerSize = 5;

/// The last eight bytes of every table, as a little-endian fixed64, so that the file's last byte is
/// the number's most significant one, 0x73.
inline constexpr uint64_t kTableMagicNumber = 0x7368616c65000001ULL;

/// The version of the layout described here. A reader refuses a table whose footer says another,
/// because it cannot know what the table holds.
inline constexpr uint32_t kTableFormatVersion = 1;

/// The size of the footer in bytes. It never varies, which is what lets a reader find it.
inline constexpr size_t kFooterSize = 48;

/// The end of a table: the handles of the filter block and the index block, and what lets a reader
/// trust them.
///
/// It is 48 bytes, all integers little-endian: the filter's offset and size and the index's offset
/// and size as fixed64s, the format version as a fixed32, the CRC32C of those first 36 bytes as a
/// fixed32, and the magic number as a fixed64. The checksum is stored as it is, and not masked as
/// a block's is: masking is for a checksum that is stored inside the bytes it covers, and this one
/// is stored after them.
struct Footer {
  BlockHandle filter;
  BlockHandle index;

  /// Appends exactly kFooterSize bytes to `*dst`.
  void EncodeTo(std::string* dst) const;

  /// Reads a footer from `input`, which must be all of the last kFooterSize bytes of a table.
  /// The checks are made in the order that tells the reader most: the size, then the magic number,
  /// which says whether this is a table at all, then the checksum, which says whether the footer is
  /// intact, and last the version, which is only meaningful in a footer that is.
  ///
  /// Returns Corruption with one of these messages: "bad footer size" if `input` is not
  /// kFooterSize bytes; "not an sstable (bad magic number)"; "footer checksum mismatch";
  /// "unsupported table format version". The handles are not checked against any file size.
  Status DecodeFrom(std::string_view input);
};

/// The contents of a block, in a buffer of their own.
///
/// ReadBlock() fills one straight from the file, into memory that nothing has written before: a
/// block is read whole on every miss of the block cache, and filling its buffer with zeros first,
/// as a std::string that is resized does, is work the read then throws away. Nothing reads a byte
/// that the file did not supply, since ReadBlock() fails on a short read before it looks at any.
/// It can be moved and not copied, and it is immutable once made.
class BlockContents {
 public:
  /// An empty block.
  BlockContents() = default;

  /// Takes the first `size` bytes of `data`, which must hold at least that many.
  BlockContents(std::unique_ptr<char[]> data, size_t size) : data_(std::move(data)), size_(size) {}

  /// Returns a copy of `bytes`: for a block that was made in memory rather than read, as a test
  /// or a fuzz target makes one.
  static BlockContents CopyOf(std::string_view bytes);

  BlockContents(BlockContents&&) noexcept = default;
  BlockContents& operator=(BlockContents&&) noexcept = default;
  BlockContents(const BlockContents&) = delete;
  BlockContents& operator=(const BlockContents&) = delete;
  ~BlockContents() = default;

  /// Returns the contents, valid for as long as this object lives and is not assigned to.
  std::string_view view() const { return {data_.get(), size_}; }

  /// Returns the size of the contents in bytes.
  size_t size() const { return size_; }

  /// Returns true if the contents are empty.
  bool empty() const { return size_ == 0; }

 private:
  std::unique_ptr<char[]> data_;
  size_t size_ = 0;
};

/// Reads the block at `handle` from `file`, with its trailer, and checks the trailer. On success
/// `*contents` is the block's contents without the trailer; on failure it is empty.
///
/// The handle comes from the file, so nothing about it is trusted: a size that no file could hold
/// is a short read and not an allocation of that size, because the bytes are read in chunks and
/// the buffer grows only as the file supplies them.
///
/// Errors: Corruption("truncated block read") if the file ends before the block and its trailer
/// do; Corruption("block checksum mismatch"), only when `verify_checksum` is set, if the trailer's
/// checksum is not that of the contents and the type byte; Corruption("unknown block type") if the
/// type byte is not 0, whether or not the checksum is checked, because this reader cannot decode
/// what it does not know. Errors from the file are returned as they are.
/// @param file the file the table is in.
/// @param handle where the block is.
/// @param verify_checksum whether to check the checksum; the type is checked either way.
/// @param contents receives the block's contents.
Status ReadBlock(const RandomAccessFile* file, const BlockHandle& handle, bool verify_checksum,
                 BlockContents* contents);

}  // namespace shale

#endif  // SHALE_IO_TABLE_FORMAT_H_
