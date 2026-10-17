#ifndef SHALE_IO_LOG_WRITER_H_
#define SHALE_IO_LOG_WRITER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "io/log_format.h"
#include "shale/env.h"
#include "shale/status.h"

namespace shale::log {

/// Appends records to a write-ahead log in the format of log_format.h.
///
/// A Writer is used by one thread at a time, and the file it writes to is written by nothing else
/// for as long as the Writer lives. It does not own the file, and never closes or syncs it.
class Writer {
 public:
  /// Creates a Writer for a new, empty file.
  /// @param dest the file to append to; it must outlive the Writer.
  explicit Writer(WritableFile* dest);

  /// Creates a Writer that continues a file that is already `dest_length` bytes long: the next
  /// record is placed as the format says given that length.
  ///
  /// The file must end exactly on a record boundary written by a Writer. A file that ends in a
  /// torn record is not one: the torn record's header goes on claiming bytes, now the first of
  /// the new ones, so the next read reports a defect and drops the rest of that block, valid new
  /// records included. A caller that finds a torn tail starts a new file instead of appending.
  /// @param dest the file to append to; it must outlive the Writer.
  /// @param dest_length how many bytes `dest` holds.
  Writer(WritableFile* dest, uint64_t dest_length);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  /// Appends one record, cut into fragments as the block boundaries require, then flushes the
  /// file once. When this returns OK the whole record has been handed to the operating system,
  /// which is what an acknowledged write promises; making it durable is a Sync() the caller does.
  ///
  /// The first failure is final. After it the file may hold a part of the record, the Env allows
  /// nothing but Close(), and the Writer no longer knows where in its block the file ends. Every
  /// later call returns that same error without touching the file.
  /// @param record the bytes to append; may be empty.
  Status AddRecord(std::string_view record);

 private:
  Status AppendFragments(std::string_view record);
  Status EmitPhysicalRecord(RecordType type, const char* data, size_t length);

  WritableFile* const dest_;
  size_t block_offset_;  // where in its block the next byte goes
  // The CRC32C of each type byte alone. A fragment's checksum extends one of these over the
  // payload, so that the type byte is not hashed again for every fragment.
  std::array<uint32_t, kMaxRecordType + 1> type_crc_;
  Status status_;  // the first failure, or OK
};

}  // namespace shale::log

#endif  // SHALE_IO_LOG_WRITER_H_
