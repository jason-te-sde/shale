#include "io/block.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "shale/iterator.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/dbformat.h"

namespace shale {

namespace {

constexpr size_t kFixed32Size = 4;

// Decodes the header of the entry that starts at `p`, where `limit` is the end of the entries.
// Returns where the entry's key bytes begin, or nullptr if the header is cut short or the key and
// value it describes would not fit before `limit`. Reads nothing at or after `limit`.
const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared, uint32_t* non_shared,
                        uint32_t* value_len) {
  p = GetVarint32Ptr(p, limit, shared);
  if (p == nullptr) {
    return nullptr;
  }
  p = GetVarint32Ptr(p, limit, non_shared);
  if (p == nullptr) {
    return nullptr;
  }
  p = GetVarint32Ptr(p, limit, value_len);
  if (p == nullptr) {
    return nullptr;
  }
  // The two lengths come from the data and each may be up to 2^32 - 1, so add them in 64 bits.
  if (static_cast<uint64_t>(limit - p) < uint64_t{*non_shared} + *value_len) {
    return nullptr;
  }
  return p;
}

}  // namespace

Block::Block(std::string contents) : data_(std::move(contents)) {
  if (data_.size() < kFixed32Size) {
    malformed_ = true;
    return;
  }
  const uint32_t count = DecodeFixed32(data_.data() + data_.size() - kFixed32Size);
  // The restart array and the count after it take 4 * (count + 1) bytes, which must leave room for
  // nothing or for entries. Written as a bound on `count`, it cannot overflow.
  const size_t room_for_restarts = (data_.size() - kFixed32Size) / kFixed32Size;
  if (count == 0 || count > room_for_restarts) {
    malformed_ = true;
    return;
  }
  const size_t restart_offset = data_.size() - (size_t{count} + 1) * kFixed32Size;
  // The first entry is at the start of the entries. The iterator starts there, so a restart array
  // that says otherwise would have it skip whatever comes before the point the array names.
  if (DecodeFixed32(data_.data() + restart_offset) != 0) {
    malformed_ = true;
    return;
  }
  restart_offset_ = restart_offset;
  num_restarts_ = count;
}

// Reads one block's entries. `status_` is sticky: once an entry has been found damaged the
// iterator stays invalid, so a seek cannot return to entries before the damage and leave a status
// that says all is well.
class Block::Iter final : public Iterator {
 public:
  Iter(std::shared_ptr<const Block> block, KeyCompareFn cmp, size_t min_key_size)
      : block_(std::move(block)),
        cmp_(cmp),
        min_key_size_(min_key_size),
        data_(block_->data_.data()),
        entries_end_(block_->restart_offset_),
        num_restarts_(block_->num_restarts_),
        current_(entries_end_),
        next_(entries_end_) {
    if (block_->malformed_) {
      status_ = Status::Corruption("bad block contents");
    }
  }

  bool Valid() const override { return current_ < entries_end_; }

  void SeekToFirst() override {
    if (!status_.ok()) {
      return;
    }
    StartAt(0);
    ParseNextEntry();
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) {
      return;
    }
    // Binary search for the last restart point whose key is less than the target: every entry
    // before it is less than the target too, and the entry we want is at or after it. The key at a
    // restart point is stored whole, so it can be read without anything before it.
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = left + (right - left + 1) / 2;
      std::string_view mid_key;
      if (!ReadRestartKey(mid, &mid_key)) {
        return;
      }
      if (cmp_(mid_key, target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }

    // Then walk forward from it. The restart point at `left` was read as a probe, unless it is the
    // first, which starts at 0 and is read by the walk itself.
    StartAt(left == 0 ? 0 : RestartOffset(left));
    do {
      if (!ParseNextEntry()) {
        return;
      }
    } while (cmp_(key_, target) < 0);
  }

  void Next() override {
    SHALE_CHECK(Valid(), "Block iterator: Next requires Valid");
    ParseNextEntry();
  }

  std::string_view key() const override {
    SHALE_CHECK(Valid(), "Block iterator: key requires Valid");
    return key_;
  }

  std::string_view value() const override {
    SHALE_CHECK(Valid(), "Block iterator: value requires Valid");
    return value_;
  }

  Status status() const override { return status_; }

 private:
  // The offset of restart point `index`, which must be less than the number of them. Its value
  // is whatever the block says, so it is not known to lie inside the entries.
  size_t RestartOffset(uint32_t index) const {
    return DecodeFixed32(data_ + entries_end_ + size_t{index} * kFixed32Size);
  }

  // Prepares to read the entry at `offset` as the next one, with no previous key.
  void StartAt(size_t offset) {
    key_.clear();
    value_ = {};
    current_ = entries_end_;
    next_ = offset;
  }

  // Reads the key stored whole at restart point `index` into `*key`, a view into the block.
  // Returns false, with the iterator failed, if the offset is outside the entries, or the entry
  // there is damaged or shares bytes with a key before it, or its key is too short to be compared.
  bool ReadRestartKey(uint32_t index, std::string_view* key) {
    const size_t offset = RestartOffset(index);
    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_len = 0;
    const char* p = nullptr;
    if (offset < entries_end_) {
      p = DecodeEntry(data_ + offset, data_ + entries_end_, &shared, &non_shared, &value_len);
    }
    // The key is stored whole here, so `non_shared` is its length. It is checked before the caller
    // can compare it with anything.
    if (p == nullptr || shared != 0 || non_shared < min_key_size_) {
      Fail();
      return false;
    }
    *key = std::string_view(p, non_shared);
    return true;
  }

  // Moves to the entry at next_, rebuilding its key from the previous one. Returns true if there
  // is one. Returns false at the end of the entries, with the iterator invalid and OK, and on a
  // damaged entry, with the iterator failed.
  bool ParseNextEntry() {
    current_ = next_;
    if (current_ >= entries_end_) {
      current_ = entries_end_;
      next_ = entries_end_;
      key_.clear();
      value_ = {};
      return false;
    }
    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_len = 0;
    const char* p =
        DecodeEntry(data_ + current_, data_ + entries_end_, &shared, &non_shared, &value_len);
    if (p == nullptr || key_.size() < shared) {
      Fail();
      return false;
    }
    key_.resize(shared);
    key_.append(p, non_shared);
    if (key_.size() < min_key_size_) {
      Fail();
      return false;
    }
    value_ = std::string_view(p + non_shared, value_len);
    next_ = static_cast<size_t>(value_.data() + value_.size() - data_);
    return true;
  }

  void Fail() {
    status_ = Status::Corruption("bad entry in block");
    current_ = entries_end_;
    next_ = entries_end_;
    key_.clear();
    value_ = {};
  }

  const std::shared_ptr<const Block> block_;
  const KeyCompareFn cmp_;
  const size_t min_key_size_;
  const char* const data_;
  const size_t entries_end_;
  const uint32_t num_restarts_;
  size_t current_;  // Offset of the current entry, or entries_end_ when there is none.
  size_t next_;     // Offset of the entry after it.
  std::string key_;
  std::string_view value_;
  Status status_;
};

std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const Block> block, KeyCompareFn cmp,
                                           size_t min_key_size) {
  SHALE_CHECK(block != nullptr, "NewBlockIterator needs a block");
  SHALE_CHECK(cmp != nullptr, "NewBlockIterator needs a comparison function");
  return std::make_unique<Block::Iter>(std::move(block), cmp, min_key_size);
}

}  // namespace shale
