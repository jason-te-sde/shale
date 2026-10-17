#ifndef SHALE_IO_LOG_FORMAT_H_
#define SHALE_IO_LOG_FORMAT_H_

// The on-disk format of the write-ahead log, shared by the Writer and the Reader.
//
// A log file is a sequence of blocks of kBlockSize bytes; only the last may be shorter. A block
// holds physical records, back to back:
//
//     checksum (4 bytes) | length (2 bytes) | type (1 byte) | payload (`length` bytes)
//
// The checksum is the masked CRC32C of the type byte followed by the payload, and both integers
// are little-endian. A physical record never crosses a block boundary, so a record that does not
// fit in what is left of a block is cut into fragments: FIRST, any number of MIDDLE, and LAST. A
// record that fits is one FULL fragment. The reader finds its way in a damaged file by dropping
// the rest of a block and starting over at the next one, which is why a boundary is a place where
// it can always resynchronise.
//
// When fewer than kHeaderSize bytes are left in a block, no header fits, and they are zero
// padding. When exactly kHeaderSize are left, the Writer fills them with a FIRST or FULL
// fragment of length zero: the next fragment of the record then starts the next block.

#include <cstddef>
#include <cstdint>

namespace shale::log {

/// The kind of a physical record. The numbers are part of the file format.
enum RecordType : uint8_t {
  kZeroType = 0,    ///< Never written. Zero padding and zero-filled file tails read as this.
  kFullType = 1,    ///< A whole logical record.
  kFirstType = 2,   ///< The first fragment of a logical record that spans blocks.
  kMiddleType = 3,  ///< A fragment that is neither the first nor the last.
  kLastType = 4,    ///< The last fragment of a logical record that spans blocks.
};

/// The largest value of RecordType that a file may hold.
inline constexpr int kMaxRecordType = kLastType;

/// The size of a block in bytes.
inline constexpr size_t kBlockSize = 32768;

/// The size of a physical record's header: checksum (4), length (2) and type (1).
inline constexpr size_t kHeaderSize = 4 + 2 + 1;

}  // namespace shale::log

#endif  // SHALE_IO_LOG_FORMAT_H_
