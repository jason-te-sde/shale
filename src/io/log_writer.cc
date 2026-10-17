#include "io/log_writer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "io/log_format.h"
#include "shale/env.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace shale::log {

Writer::Writer(WritableFile* dest) : Writer(dest, 0) {}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(static_cast<size_t>(dest_length % kBlockSize)), type_crc_() {
  for (size_t type = 0; type < type_crc_.size(); type++) {
    const char type_byte = static_cast<char>(type);
    type_crc_[type] = crc32c::Value(&type_byte, 1);
  }
}

Status Writer::AddRecord(std::string_view record) {
  if (!status_.ok()) {
    return status_;
  }
  status_ = AppendFragments(record);
  return status_;
}

Status Writer::AppendFragments(std::string_view record) {
  const char* ptr = record.data();
  size_t left = record.size();
  bool begin = true;
  // Runs at least once, so that an empty record is written as a FULL fragment of length zero.
  do {
    const size_t leftover = kBlockSize - block_offset_;
    if (leftover < kHeaderSize) {
      // No header fits in what is left of the block. Zeros mark it as padding for the reader,
      // which skips it without looking, and the record goes to the next block.
      if (leftover > 0) {
        static constexpr std::array<char, kHeaderSize> kZeros{};
        const Status s = dest_->Append(std::string_view(kZeros.data(), leftover));
        if (!s.ok()) {
          return s;
        }
      }
      block_offset_ = 0;
    }

    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    const size_t fragment_length = std::min(left, avail);
    const bool end = left == fragment_length;
    RecordType type = kMiddleType;
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    }
    const Status s = EmitPhysicalRecord(type, ptr, fragment_length);
    if (!s.ok()) {
      return s;
    }
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (left > 0);

  // One flush for the whole record, after its last fragment: the caller is told "written" only
  // once every fragment has reached the operating system.
  return dest_->Flush();
}

Status Writer::EmitPhysicalRecord(RecordType type, const char* data, size_t length) {
  SHALE_CHECK(length <= 0xffff && block_offset_ + kHeaderSize + length <= kBlockSize,
              "a log fragment must fit in the rest of its block");

  std::array<char, kHeaderSize> header;
  header[4] = static_cast<char>(length & 0xff);
  header[5] = static_cast<char>(length >> 8);
  header[6] = static_cast<char>(type);
  const uint32_t crc = crc32c::Extend(type_crc_[static_cast<size_t>(type)], data, length);
  EncodeFixed32(header.data(), crc32c::Mask(crc));

  Status s = dest_->Append(std::string_view(header.data(), header.size()));
  if (s.ok() && length > 0) {
    s = dest_->Append(std::string_view(data, length));
  }
  block_offset_ += kHeaderSize + length;
  return s;
}

}  // namespace shale::log
