#include "io/block_builder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "util/check.h"
#include "util/coding.h"

namespace shale {

namespace {

// A varint32 takes at most 5 bytes, and an entry starts with three of them.
constexpr uint64_t kMaxEntryHeaderSize = uint64_t{3} * 5;

// The restart offsets and the count are fixed32s.
constexpr size_t kFixed32Size = 4;

}  // namespace

BlockBuilder::BlockBuilder(int restart_interval) : restart_interval_(restart_interval) {
  SHALE_CHECK(restart_interval >= 1, "BlockBuilder needs restart_interval >= 1");
  restarts_.push_back(0);  // The first entry is always a restart point.
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.assign(1, 0);
  interval_entries_ = 0;
  has_entries_ = false;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  if (finished_) {
    return buffer_.size();  // The restart array is already part of the buffer.
  }
  return buffer_.size() + restarts_.size() * kFixed32Size + kFixed32Size;
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  SHALE_CHECK(!finished_, "BlockBuilder::Add after Finish; call Reset first");
  // The reader finds restart points by 32-bit offsets, so a block that outgrew them would be read
  // back wrong without any error.
  const uint64_t size_after = uint64_t{buffer_.size()} + key.size() + value.size() +
                              kMaxEntryHeaderSize +
                              (uint64_t{restarts_.size()} + 1) * kFixed32Size + kFixed32Size;
  SHALE_CHECK(size_after <= std::numeric_limits<uint32_t>::max(),
              "BlockBuilder::Add: the block would be larger than 4 GiB");

  size_t shared = 0;
  if (interval_entries_ < restart_interval_) {
    const size_t limit = std::min(last_key_.size(), key.size());
    while (shared < limit && last_key_[shared] == key[shared]) {
      ++shared;
    }
  } else {
    // This entry starts a new restart interval: it holds its whole key, which is shared = 0.
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    interval_entries_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.substr(shared));
  buffer_.append(value);

  last_key_.resize(shared);
  last_key_.append(key.substr(shared));
  ++interval_entries_;
  has_entries_ = true;
}

std::string_view BlockBuilder::Finish() {
  if (!finished_) {
    for (const uint32_t restart : restarts_) {
      PutFixed32(&buffer_, restart);
    }
    PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
    finished_ = true;
  }
  return buffer_;
}

}  // namespace shale
