#include "io/readahead.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

#include "io/table_format.h"
#include "shale/env.h"
#include "shale/status.h"
#include "util/check.h"

namespace shale {

void Readahead::Reset() {
  steps_ = 0;
  ahead_ = kInitialBytes;
}

void Readahead::NoteStep() {
  if (steps_ < kSequentialBlocks) {
    ++steps_;
  }
}

bool Readahead::Find(const BlockHandle& handle, std::string_view* stored) const {
  // The sizes come from a file, so the end is computed without overflow.
  if (handle.offset < offset_ || handle.size > size_) {
    return false;
  }
  const uint64_t start = handle.offset - offset_;
  const uint64_t total = handle.size + kBlockTrailerSize;
  if (start > size_ || total > size_ - start) {
    return false;
  }
  *stored = std::string_view(buffer_.get() + start, static_cast<size_t>(total));
  return true;
}

bool Readahead::ShouldFill(const BlockHandle& handle) const {
  return steps_ >= kSequentialBlocks && handle.size <= kMaxBytes - kBlockTrailerSize;
}

Status Readahead::Fill(const RandomAccessFile& file, const BlockHandle& handle, uint64_t limit) {
  Discard();
  if (handle.size > kMaxBytes - kBlockTrailerSize) {
    return Status::OK();  // ShouldFill() says no to it; a caller that asks anyway gets no buffer
  }
  const uint64_t wanted = handle.size + kBlockTrailerSize + ahead_;
  const uint64_t room = handle.offset < limit ? limit - handle.offset : 0;
  const size_t n = static_cast<size_t>(std::min(wanted, room));
  ahead_ = std::min(2 * ahead_, kMaxBytes);
  if (n == 0) {
    return Status::OK();
  }
  if (n > capacity_) {
    buffer_ = std::make_unique_for_overwrite<char[]>(n);
    capacity_ = n;
  }
  std::string_view got;
  Status s = file.Read(handle.offset, n, &got, buffer_.get());
  if (!s.ok()) {
    return s;
  }
  SHALE_CHECK(got.size() <= n, "a RandomAccessFile returned more bytes than were asked for");
  if (!got.empty() && got.data() != buffer_.get()) {
    std::memmove(buffer_.get(), got.data(), got.size());
  }
  offset_ = handle.offset;
  size_ = got.size();
  return Status::OK();
}

void Readahead::Discard() { size_ = 0; }

}  // namespace shale
