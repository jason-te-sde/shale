#include "io/table_format.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "shale/env.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace shale {

namespace {

// Where the fields of the footer are. The bytes before the checksum are what it covers.
constexpr size_t kFooterVersionOffset = 32;
constexpr size_t kFooterChecksumOffset = 36;
constexpr size_t kFooterMagicOffset = 40;
static_assert(kFooterMagicOffset + 8 == kFooterSize, "the footer ends with the magic number");

// The type byte of a block stored as it is. It is the only type there is.
constexpr char kBlockTypeRaw = 0;

// The most a read asks the file for at once. A block of this size or less is read with one call.
// Bigger ones are read in pieces, so that the buffer is sized by what the file holds and not by
// what a handle says.
constexpr size_t kReadChunkSize = size_t{1} << 20;

}  // namespace

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset);
  PutVarint64(dst, size);
}

Status BlockHandle::DecodeFrom(std::string_view* input) {
  std::string_view rest = *input;
  uint64_t decoded_offset = 0;
  uint64_t decoded_size = 0;
  if (!GetVarint64(&rest, &decoded_offset) || !GetVarint64(&rest, &decoded_size)) {
    return Status::Corruption("bad block handle");
  }
  offset = decoded_offset;
  size = decoded_size;
  *input = rest;
  return Status::OK();
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t start = dst->size();
  PutFixed64(dst, filter.offset);
  PutFixed64(dst, filter.size);
  PutFixed64(dst, index.offset);
  PutFixed64(dst, index.size);
  PutFixed32(dst, kTableFormatVersion);
  SHALE_CHECK(dst->size() - start == kFooterChecksumOffset, "the footer layout has changed");
  PutFixed32(dst, crc32c::Value(dst->data() + start, kFooterChecksumOffset));
  PutFixed64(dst, kTableMagicNumber);
  SHALE_CHECK(dst->size() - start == kFooterSize, "the footer is not kFooterSize bytes");
}

Status Footer::DecodeFrom(std::string_view input) {
  if (input.size() != kFooterSize) {
    return Status::Corruption("bad footer size");
  }
  const char* const p = input.data();
  if (DecodeFixed64(p + kFooterMagicOffset) != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }
  if (DecodeFixed32(p + kFooterChecksumOffset) != crc32c::Value(p, kFooterChecksumOffset)) {
    return Status::Corruption("footer checksum mismatch");
  }
  if (DecodeFixed32(p + kFooterVersionOffset) != kTableFormatVersion) {
    return Status::Corruption("unsupported table format version");
  }
  filter.offset = DecodeFixed64(p);
  filter.size = DecodeFixed64(p + 8);
  index.offset = DecodeFixed64(p + 16);
  index.size = DecodeFixed64(p + 24);
  return Status::OK();
}

Status ReadBlock(const RandomAccessFile* file, const BlockHandle& handle, bool verify_checksum,
                 std::string* contents) {
  contents->clear();
  // A block and its trailer that cannot be counted in bytes, or that would end beyond what a file
  // offset can name, are in no file.
  constexpr uint64_t kMaxBlockSize =
      uint64_t{std::numeric_limits<size_t>::max()} - kBlockTrailerSize;
  if (handle.size > kMaxBlockSize) {
    return Status::Corruption("truncated block read");
  }
  const size_t total = static_cast<size_t>(handle.size) + kBlockTrailerSize;
  if (handle.offset > std::numeric_limits<uint64_t>::max() - total) {
    return Status::Corruption("truncated block read");
  }

  // Read in chunks. Each is requested only after the one before it came back whole, so the buffer
  // is never larger than the file's bytes plus one chunk, however large the handle says the block
  // is.
  std::string stored;
  size_t have = 0;
  while (have < total) {
    const size_t want = std::min(total - have, kReadChunkSize);
    stored.resize(have + want);
    std::string_view got;
    char* const scratch = stored.data() + have;
    Status s = file->Read(handle.offset + have, want, &got, scratch);
    if (!s.ok()) {
      return s;
    }
    SHALE_CHECK(got.size() <= want, "a RandomAccessFile returned more bytes than were asked for");
    if (!got.empty() && got.data() != scratch) {
      // The contract puts the bytes in `scratch`, but a file that points into its own storage
      // instead is not misreading anything, and the bytes are the same.
      std::memmove(scratch, got.data(), got.size());
    }
    have += got.size();
    if (got.size() < want) {
      return Status::Corruption("truncated block read");
    }
  }

  const size_t size = static_cast<size_t>(handle.size);
  const char type = stored[size];
  if (verify_checksum) {
    const uint32_t expected = crc32c::Unmask(DecodeFixed32(stored.data() + size + 1));
    // The checksum covers the contents and the type byte, which are contiguous.
    if (crc32c::Value(stored.data(), size + 1) != expected) {
      return Status::Corruption("block checksum mismatch");
    }
  }
  if (type != kBlockTypeRaw) {
    return Status::Corruption("unknown block type");
  }
  stored.resize(size);
  *contents = std::move(stored);
  return Status::OK();
}

}  // namespace shale
