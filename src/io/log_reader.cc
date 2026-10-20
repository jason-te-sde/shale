#include "io/log_reader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "io/log_format.h"
#include "shale/env.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace shale::log {

namespace {

bool AllZeros(std::string_view bytes) {
  return std::all_of(bytes.begin(), bytes.end(), [](char c) { return c == '\0'; });
}

}  // namespace

Reader::Reporter::~Reporter() = default;

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum)
    : file_(file), reporter_(reporter), checksum_(checksum), backing_store_(kBlockSize) {}

uint64_t Reader::LastRecordOffset() const { return last_record_offset_; }

uint64_t Reader::ZeroTailBytes() const { return zero_tail_bytes_; }

bool Reader::ReadRecord(std::string_view* record, std::string* scratch) {
  scratch->clear();
  *record = std::string_view();
  bool in_fragmented_record = false;
  uint64_t prospective_record_offset = 0;  // where the record being assembled began
  // How many bytes of the log, headers included, the fragments in `scratch` came from: what is
  // lost if the record is abandoned.
  size_t assembled_bytes = 0;

  for (;;) {
    RecordType type = kZeroType;
    std::string_view fragment;
    const Physical result = ReadPhysicalRecord(&type, &fragment);

    switch (result) {
      case Physical::kRecord: {
        // The fragment's header sits just before its payload, which ends where buffer_ now begins.
        const uint64_t physical_record_offset =
            end_of_buffer_offset_ - buffer_.size() - fragment.size() - kHeaderSize;
        switch (type) {
          case kFullType:
            if (in_fragmented_record) {
              // Nothing writes this: after a FIRST comes a MIDDLE or a LAST.
              Report(assembled_bytes, Status::Corruption("partial record without end"));
              scratch->clear();
            }
            *record = fragment;
            last_record_offset_ = physical_record_offset;
            return true;

          case kFirstType:
            if (in_fragmented_record) {
              Report(assembled_bytes, Status::Corruption("partial record without end"));
            }
            scratch->assign(fragment);
            in_fragmented_record = true;
            prospective_record_offset = physical_record_offset;
            assembled_bytes = kHeaderSize + fragment.size();
            break;

          case kMiddleType:
            if (in_fragmented_record) {
              scratch->append(fragment);
              assembled_bytes += kHeaderSize + fragment.size();
            } else {
              Report(kHeaderSize + fragment.size(),
                     Status::Corruption("missing start of fragmented record"));
            }
            break;

          case kLastType:
            if (in_fragmented_record) {
              scratch->append(fragment);
              *record = *scratch;
              last_record_offset_ = prospective_record_offset;
              return true;
            }
            Report(kHeaderSize + fragment.size(),
                   Status::Corruption("missing start of fragmented record"));
            break;

          case kZeroType:
            // ReadPhysicalRecord() turns every type outside 1..4 into a defect.
            SHALE_CHECK(false, "ReadPhysicalRecord returned a record of an unknown type");
        }
        break;
      }

      case Physical::kBad:
        // The block is already dropped and reported. What was assembled before it cannot be
        // completed: the fragments that follow in the next block would not be its own.
        if (in_fragmented_record) {
          Report(assembled_bytes, Status::Corruption("partial record without end"));
          in_fragmented_record = false;
          scratch->clear();
        }
        break;

      case Physical::kEof:
        // A FIRST or a MIDDLE without its LAST is where a writer that died leaves the file, and so
        // is one that zeros to the end of the file follow: not a defect, and the half-read record
        // is dropped without a word.
        scratch->clear();
        return false;
    }
  }
}

Reader::Physical Reader::ReadPhysicalRecord(RecordType* type, std::string_view* payload) {
  for (;;) {
    // What a look past zeros read, and found not to be their end, is reported first, one block at
    // a time, as reading the blocks one by one would have reported it. See ZerosRunToTheEnd().
    if (zero_blocks_to_report_ > 0) {
      zero_blocks_to_report_--;
      Report(kBlockSize, Status::Corruption("unknown record type"));
      return Physical::kBad;
    }
    if (!read_error_.ok()) {
      Report(kBlockSize, read_error_);
      read_error_ = Status::OK();
      return Physical::kEof;
    }

    if (buffer_.size() < kHeaderSize) {
      if (eof_) {
        // What is left cannot hold a header: the file ended inside one, or inside the padding
        // of a block. Either way it is the end, and it is not reported.
        buffer_ = std::string_view();
        return Physical::kEof;
      }
      // The last read filled a block, so what is left of it is its trailer, which is padding
      // (or nothing), and the next block starts at the next read.
      const Status s = ReadBlock();
      if (!s.ok()) {
        Report(kBlockSize, s);
        return Physical::kEof;
      }
      continue;
    }

    const char* header = buffer_.data();
    const size_t length = static_cast<size_t>(static_cast<uint8_t>(header[4])) |
                          (static_cast<size_t>(static_cast<uint8_t>(header[5])) << 8);
    const uint8_t raw_type = static_cast<uint8_t>(header[6]);
    const size_t position_in_block = block_length_ - buffer_.size();

    // Zeros where a header starts, and in every byte from there to the end of the file, are what a
    // filesystem leaves where it kept the length of a file and not its data (D6). They end the log
    // without a report, as a record cut short by the end of the file does.
    //
    // Everywhere else a zero type is a defect, and a defect is never taken for the end of the log
    // by where it falls: the reader drops the rest of a block at one, and the records it drops can
    // be ones that were acknowledged, whether or not anything valid follows. What makes these
    // zeros different is that every byte from here to the end of the file is read and found to be
    // zero, so no record, whole or damaged, comes after them to be swallowed, and the only bytes
    // dropped are the zeros. Under D1's premise a synced byte is never zeroed, so none of them was
    // acknowledged as synced. A zero header followed by any byte that is not zero is reported
    // below, exactly as before, and so are zeros that start inside a record, whose header is not
    // zero.
    if (raw_type == kZeroType && AllZeros(buffer_)) {
      const size_t in_this_block = buffer_.size();
      uint64_t after = 0;
      if (ZerosRunToTheEnd(&after)) {
        zero_tail_bytes_ = in_this_block + after;
        return Physical::kEof;
      }
      // Something that is not zero follows. This header is a zero type, reported with the rest of
      // its block, as any is.
      Report(in_this_block, Status::Corruption("unknown record type"));
      return Physical::kBad;
    }

    // The header's own fields are judged before the file's end and before the checksum, so that a
    // header that cannot be right is reported whatever follows it.
    if (raw_type == kZeroType || raw_type > kMaxRecordType) {
      return DropRestOfBlock("unknown record type");
    }
    // A fragment never leaves its block. A longer one is damaged, even if the file ends in this
    // block, because a torn write only cuts a fragment short and never makes it longer.
    if (length > kBlockSize - position_in_block - kHeaderSize) {
      return DropRestOfBlock("bad record length");
    }
    if (kHeaderSize + length > buffer_.size()) {
      // The fragment fits in its block but the file ends before it does: the writer died while
      // writing it. A block that was read in full cannot end here, so this is the last block.
      SHALE_CHECK(eof_, "a full block ended inside a fragment that fits in it");
      // Or the length is damaged, and the fragment is whole. A crash leaves a prefix of the
      // fragment, and a prefix is no fragment with the checksum in the header, short of a
      // collision of 32 bits (about one in 2^32 for each byte that is left, of a file that a
      // crash really cut). A whole fragment is. So where the bytes that are here are a fragment
      // that the stored checksum vouches for, the end of the file is not what this is.
      if (checksum_ && HoldsAWholeFragmentShorterThan(header, length)) {
        return DropRestOfBlock("bad record length");
      }
      buffer_ = std::string_view();
      return Physical::kEof;
    }

    if (checksum_) {
      const uint32_t expected = crc32c::Unmask(DecodeFixed32(header));
      // The type byte and the payload are adjacent, and the checksum covers both.
      const uint32_t actual = crc32c::Value(header + 6, 1 + length);
      if (actual != expected) {
        // The length may be what is wrong, so nothing after this header can be trusted.
        return DropRestOfBlock("checksum mismatch");
      }
    }

    *type = static_cast<RecordType>(raw_type);
    *payload = std::string_view(header + kHeaderSize, length);
    buffer_.remove_prefix(kHeaderSize + length);
    return Physical::kRecord;
  }
}

bool Reader::HoldsAWholeFragmentShorterThan(const char* header, size_t length) const {
  // What is left after the header, which is fewer bytes than `length` claims. The fragment that was
  // written may have been any length up to that, the last of them ending with the file.
  const size_t available = buffer_.size() - kHeaderSize;
  SHALE_CHECK(available < length, "the fragment is not claimed to run past the end of the file");
  const uint32_t expected = crc32c::Unmask(DecodeFixed32(header));
  // The checksum covers the type byte and then the payload, so it is taken over the one and
  // extended a byte at a time, and compared at every length: one pass over what is left.
  uint32_t crc = crc32c::Value(header + 6, 1);
  const char* payload = header + kHeaderSize;
  for (size_t candidate = 0;; candidate++) {
    if (crc == expected) {
      return true;
    }
    if (candidate == available) {
      return false;
    }
    crc = crc32c::Extend(crc, payload + candidate, 1);
  }
}

Status Reader::ReadBlock() {
  buffer_ = std::string_view();
  std::string_view block;
  Status s = file_->Read(kBlockSize, &block, backing_store_.data());
  if (!s.ok()) {
    eof_ = true;
    block_length_ = 0;
    return s;
  }
  end_of_buffer_offset_ += block.size();
  buffer_ = block;
  block_length_ = block.size();
  if (block.size() < kBlockSize) {
    eof_ = true;
  }
  return Status::OK();
}

bool Reader::ZerosRunToTheEnd(uint64_t* bytes) {
  // Every block after this one is read, and every byte of it checked, until the end of the file or
  // a byte that is not zero. The blocks of zeros on the way are not kept, only counted: they are
  // either the end of the log or one report each.
  uint64_t zeros = 0;
  size_t blocks = 0;
  while (!eof_) {
    const Status s = ReadBlock();
    if (!s.ok()) {
      // What the rest of the file holds is unknown, so the zeros are not taken for its end: they
      // are reported, and then the failed read, as reading on block by block would have.
      zero_blocks_to_report_ = blocks;
      read_error_ = s;
      return false;
    }
    if (!AllZeros(buffer_)) {
      // The blocks of zeros before this one were each a whole block, since a block that is not
      // whole ends the file, and are reported. This one is read from its start, as it would be
      // after them.
      zero_blocks_to_report_ = blocks;
      return false;
    }
    zeros += buffer_.size();
    blocks++;
  }
  buffer_ = std::string_view();
  *bytes = zeros;
  return true;
}

Reader::Physical Reader::DropRestOfBlock(std::string_view reason) {
  const size_t dropped = buffer_.size();
  buffer_ = std::string_view();
  Report(dropped, Status::Corruption(reason));
  return Physical::kBad;
}

void Reader::Report(size_t bytes, const Status& status) {
  if (reporter_ != nullptr) {
    reporter_->Corruption(bytes, status);
  }
}

}  // namespace shale::log
