#ifndef SHALE_IO_LOG_READER_H_
#define SHALE_IO_LOG_READER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "io/log_format.h"
#include "shale/env.h"
#include "shale/status.h"

namespace shale::log {

/// Reads the records of a write-ahead log, written by a Writer, from front to back.
///
/// Recovery decides from this class whether a log merely ends in a torn record, which a crash
/// leaves behind and which is fine, or is damaged in the middle, which it must not open. The
/// dividing line is exact:
///
/// - **Silent.** The file ends, and nothing is reported, when it stops inside a header, or inside
///   a record whose length says it would have fitted in its block, or after a FIRST or MIDDLE
///   fragment that no LAST follows. These are the shapes in which a writer that died leaves its
///   file. The file ends mid-record whatever the fragment boundaries: reading stops there, and
///   the records before it were all returned.
/// - **Reported.** Everything else is reported to the Reporter, once for each defect, with the
///   number of bytes it makes the reader drop and a Corruption status: a read error (the status
///   the file returned); "checksum mismatch"; "bad record length", for a length that would take
///   the record out of its block; "unknown record type", for a type that is not one of the four
///   (zero included); "missing start of fragmented record", for a MIDDLE or LAST with no FIRST
///   before it; and "partial record without end", for a FIRST that something other than a MIDDLE
///   or LAST follows. A defect in the header or the contents drops the rest of its block, since
///   after one the lengths in it cannot be trusted, and reading goes on at the next block. A
///   fragmented record that a defect interrupts is lost whole, never joined to another one.
///
/// A cut that falls where a defect would have been, as a damaged length field that points
/// beyond the end of a short file but inside its block, cannot be told from a torn tail, and is
/// treated as one.
///
/// A Reader is used by one thread at a time.
class Reader {
 public:
  /// Receives the reports of a Reader.
  class Reporter {
   public:
    /// Does nothing; a Reporter is destroyed through this base class.
    virtual ~Reporter();

    /// Called when the reader meets a defect.
    /// @param bytes how many bytes of the log the defect made the reader drop; approximate for a
    ///        failed read, where it is the size of the block that could not be read.
    /// @param status what was wrong; always an error, a Corruption unless a read failed.
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  /// Creates a Reader that starts at the beginning of `file`.
  /// @param file the log to read; it must outlive the Reader and be positioned at its start.
  /// @param reporter told of every defect; may be null, in which case defects are skipped
  ///        unseen. It must outlive the Reader.
  /// @param checksum whether to verify each record's checksum. Checksums off lets a damaged
  ///        payload through unnoticed; the structure of the log is checked either way.
  Reader(SequentialFile* file, Reporter* reporter, bool checksum);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  /// Reads the next record. Returns false at the end of the log, silently or after reporting a
  /// failed read, and keeps returning false after that.
  ///
  /// On true, `*record` is the record. It lives either in the Reader's block buffer or in
  /// `*scratch`, and is valid until the next call to ReadRecord() or until `*scratch` is changed.
  /// @param record receives the record.
  /// @param scratch storage for a record that spans blocks; it is cleared first.
  bool ReadRecord(std::string_view* record, std::string* scratch);

  /// Returns the offset in the file of the first fragment of the record that ReadRecord() last
  /// returned, and zero before it has returned any.
  uint64_t LastRecordOffset() const;

 private:
  // What ReadPhysicalRecord() found.
  enum class Physical : uint8_t {
    kRecord,  // a fragment, in the out-parameters
    kBad,     // a defect, reported, and the rest of the block dropped
    kEof,     // the end of the log: nothing more can be read
  };

  Physical ReadPhysicalRecord(RecordType* type, std::string_view* payload);

  // Drops the rest of the block, and reports `reason` and how many bytes that was.
  Physical DropRestOfBlock(std::string_view reason);

  // Tells the reporter, if there is one.
  void Report(size_t bytes, const Status& status);

  SequentialFile* const file_;
  Reporter* const reporter_;
  const bool checksum_;
  std::vector<char> backing_store_;    // one block
  std::string_view buffer_;            // the part of the block that is not consumed yet
  size_t block_length_ = 0;            // how many bytes of the block the last read returned
  bool eof_ = false;                   // the last read returned less than a block, or failed
  uint64_t end_of_buffer_offset_ = 0;  // the file offset just past the end of buffer_
  uint64_t last_record_offset_ = 0;
};

}  // namespace shale::log

#endif  // SHALE_IO_LOG_READER_H_
