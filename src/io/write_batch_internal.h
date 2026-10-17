#ifndef SHALE_IO_WRITE_BATCH_INTERNAL_H_
#define SHALE_IO_WRITE_BATCH_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "shale/status.h"
#include "shale/write_batch.h"
#include "util/dbformat.h"

namespace shale {

class MemTable;

/// What the engine does with a WriteBatch that its users must not be able to: set its sequence
/// number, take its bytes to the log and rebuild it from them, and apply it to a memtable.
///
/// The encoding is a 12-byte header followed by the records. The header is the sequence number of
/// the first operation as a little-endian fixed64, then the number of operations as a fixed32.
/// A record is a tag byte, 1 for a put and 0 for a deletion, then the key and, for a put, the
/// value, each as a varint32 length and its bytes. The log stores exactly these bytes, so changing
/// them changes the file format.
class WriteBatchInternal {
 public:
  /// Returns the number of operations the header says the batch holds.
  static uint32_t Count(const WriteBatch* b);

  /// Sets the operation count in the header. Put() and Delete() keep it up to date; this is for
  /// tests that need a batch whose header disagrees with its records.
  static void SetCount(WriteBatch* b, uint32_t n);

  /// Returns the sequence number of the batch's first operation. The operation at position i has
  /// sequence number Sequence() + i.
  static SequenceNumber Sequence(const WriteBatch* b);

  /// Sets the sequence number of the batch's first operation.
  static void SetSequence(WriteBatch* b, SequenceNumber s);

  /// Returns the encoded batch, header included, which is what the log stores. The view is valid
  /// until the batch is next changed.
  static std::string_view Contents(const WriteBatch* b);

  /// Returns the length of Contents().
  static size_t ByteSize(const WriteBatch* b);

  /// Replaces the batch with `contents`, bytes the log gave back. Returns Corruption, and leaves
  /// the batch as it was, if there are fewer bytes than a header. Nothing else is checked here:
  /// whether the records are well formed is found out when the batch is iterated. The payload
  /// figures that describe what was added through Put() and Delete() start again from nothing,
  /// since bytes from the log carry no such history.
  static Status SetContents(WriteBatch* b, std::string_view contents);

  /// Applies the batch to `*mem`: the operation at position i is added with sequence number
  /// Sequence(b) + i. Returns the Corruption that Iterate() finds in a damaged batch, or
  /// Corruption if the sequence numbers would run past kMaxSequenceNumber. On an error the
  /// operations before the damage have already been added.
  /// @param b the batch to apply.
  /// @param mem the memtable to add to; must not be null.
  static Status InsertInto(const WriteBatch* b, MemTable* mem);

  /// Adds the operations of `src` to the end of `*dst`, in order. The sequence number of `dst` is
  /// kept, and `src` is not changed. This is how a group of batches becomes one log record.
  /// Aborts if the two counts add up to more than 2^32 - 1, which is all the header can say. As
  /// for WriteBatch::Put(), that is a caller bug and not an error to return: a batch with that
  /// many operations is at least 8 GiB.
  static void Append(WriteBatch* dst, const WriteBatch* src);

  /// Returns the sum of the key sizes and value sizes of what was added through Put() and
  /// Delete(), or through Append() of batches built that way. The write path sums it to account
  /// for how much a write will cost.
  static uint64_t PayloadBytes(const WriteBatch* b);

  /// Returns true if any key added through Put() or Delete() was longer than kMaxKeySize, or any
  /// value longer than kMaxValueSize. The write path refuses such a batch with InvalidArgument.
  static bool HasOversizedEntry(const WriteBatch* b);
};

}  // namespace shale

#endif  // SHALE_IO_WRITE_BATCH_INTERNAL_H_
