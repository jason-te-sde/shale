#ifndef SHALE_WRITE_BATCH_H_
#define SHALE_WRITE_BATCH_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "shale/status.h"

namespace shale {

/// The longest key the database accepts, in bytes. A key above it makes a write fail with
/// InvalidArgument.
inline constexpr size_t kMaxKeySize = 65536;

/// The longest value the database accepts, in bytes. A value above it makes a write fail with
/// InvalidArgument.
inline constexpr size_t kMaxValueSize = 67108864;

/// A group of updates that are applied together or not at all.
///
/// A batch is a list of puts and deletions. Writing it to the database gives every operation in it
/// the next sequence numbers in order, and logs it as one record, so after a crash either all of
/// it is there or none of it. The same bytes are what the write-ahead log stores, which is why
/// the batch keeps its operations in their encoded form instead of in a vector.
///
/// The limits on key and value size are not checked when an operation is added, since adding
/// cannot fail: the batch notes the violation, and the write that is handed the batch returns
/// InvalidArgument and applies none of it. A WriteBatch is not thread-safe.
class WriteBatch {
 public:
  /// Creates an empty batch.
  WriteBatch();

  /// Adds an operation that sets `key` to `value`. The bytes are copied. Neither may exceed the
  /// limits above, but if one does the entry is accepted here and the write is refused. An entry
  /// too large to encode at all, 4 GiB or more in the key or the value, is not stored, and is not
  /// counted by Count(); it makes the write fail the same way.
  ///
  /// Aborts if the batch already holds 2^32 - 1 operations, which is all its 32-bit count can
  /// say. That is unlike an oversized entry, which returns InvalidArgument when written: an
  /// oversized entry is data a user can send, while a batch of 2^32 operations is at least 8 GiB
  /// and no real batch reaches it, so reaching it is a bug in the caller.
  /// @param key the key to set; may be empty.
  /// @param value the new value; may be empty.
  void Put(std::string_view key, std::string_view value);

  /// Adds an operation that deletes `key`. The key is copied. The limit on its size applies as it
  /// does for Put(), and so does the abort when the batch already holds 2^32 - 1 operations.
  /// @param key the key to delete; may be empty.
  void Delete(std::string_view key);

  /// Removes every operation and the sequence number, leaving the batch as a new one is.
  void Clear();

  /// Returns the number of operations in the batch.
  uint32_t Count() const;

  /// Returns the size of the batch in bytes as it is encoded, header included. It grows with every
  /// operation, which makes it the figure a caller compares with a limit on how big a batch, or a
  /// group of batches, may get.
  size_t ApproximateSize() const;

  /// What Iterate() calls for each operation in a batch.
  class Handler {
   public:
    /// Does nothing; a Handler is destroyed through this base class.
    virtual ~Handler();

    /// Called for an operation that sets `key` to `value`. Both views are valid only during the
    /// call.
    virtual void Put(std::string_view key, std::string_view value) = 0;

    /// Called for an operation that deletes `key`. The view is valid only during the call.
    virtual void Delete(std::string_view key) = 0;
  };

  /// Calls `handler` for every operation in the batch, in the order they were added.
  ///
  /// A batch that was built through Put() and Delete() cannot fail here. One that was rebuilt from
  /// bytes, read from the log, can be damaged, and then the result is a Corruption status naming
  /// what is wrong. The handler has been called for every operation before the damage by then, so
  /// a handler that applies the operations leaves a partial result behind.
  /// @param handler receives the operations; must not be null.
  /// @return OK, or Corruption if the batch is malformed.
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  std::string rep_;
  uint64_t payload_bytes_ = 0;
  bool oversized_ = false;
};

}  // namespace shale

#endif  // SHALE_WRITE_BATCH_H_
