#include "shale/write_batch.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "io/memtable.h"
#include "io/write_batch_internal.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/dbformat.h"

namespace shale {

namespace {

// The header: the sequence number as a fixed64, then the count as a fixed32.
constexpr size_t kHeaderSize = 12;
constexpr size_t kCountOffset = 8;

// A length-prefixed string cannot be longer than this.
constexpr size_t kMaxEncodedLength = std::numeric_limits<uint32_t>::max();

// Counts one more operation. The count is 32 bits, so a batch of 2^32 operations cannot be
// described by its own header; reaching that takes at least 12 GiB, and it aborts here instead of
// wrapping to a count that the batch would then fail to match.
void CountOneMore(WriteBatch* b) {
  const uint32_t count = WriteBatchInternal::Count(b);
  SHALE_CHECK(count < std::numeric_limits<uint32_t>::max(),
              "WriteBatch cannot hold more than 2^32 - 1 operations");
  WriteBatchInternal::SetCount(b, count + 1);
}

// Adds each entry of a batch to a memtable under consecutive sequence numbers.
class MemTableInserter final : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber sequence, MemTable* mem) : sequence_(sequence), mem_(mem) {}

  void Put(std::string_view key, std::string_view value) override {
    Add(ValueType::kValue, key, value);
  }

  void Delete(std::string_view key) override { Add(ValueType::kDeletion, key, {}); }

  // True if an entry was left out because its sequence number did not fit.
  bool out_of_range() const { return out_of_range_; }

 private:
  void Add(ValueType type, std::string_view key, std::string_view value) {
    // Handled here, and not by the memtable, which would abort: the number came out of the log.
    // Once one entry is past the limit every later one is too, so the counter stops there.
    if (sequence_ > kMaxSequenceNumber) {
      out_of_range_ = true;
      return;
    }
    mem_->Add(sequence_, type, key, value);
    sequence_++;
  }

  SequenceNumber sequence_;
  MemTable* const mem_;
  bool out_of_range_ = false;
};

}  // namespace

WriteBatch::Handler::~Handler() = default;

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.assign(kHeaderSize, '\0');
  payload_bytes_ = 0;
  oversized_ = false;
}

uint32_t WriteBatch::Count() const { return WriteBatchInternal::Count(this); }

size_t WriteBatch::ApproximateSize() const { return rep_.size(); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  payload_bytes_ += key.size() + value.size();
  oversized_ = oversized_ || key.size() > kMaxKeySize || value.size() > kMaxValueSize;
  if (key.size() > kMaxEncodedLength || value.size() > kMaxEncodedLength) {
    return;  // cannot be encoded, and the flag above is what makes the write fail
  }
  CountOneMore(this);
  rep_.push_back(static_cast<char>(ValueType::kValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  payload_bytes_ += key.size();
  oversized_ = oversized_ || key.size() > kMaxKeySize;
  if (key.size() > kMaxEncodedLength) {
    return;
  }
  CountOneMore(this);
  rep_.push_back(static_cast<char>(ValueType::kDeletion));
  PutLengthPrefixed(&rep_, key);
}

Status WriteBatch::Iterate(Handler* handler) const {
  std::string_view input(rep_);
  if (input.size() < kHeaderSize) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  input.remove_prefix(kHeaderSize);

  uint64_t found = 0;
  std::string_view key;
  std::string_view value;
  while (!input.empty()) {
    const auto type = static_cast<ValueType>(static_cast<uint8_t>(input.front()));
    input.remove_prefix(1);
    // No default branch: a new ValueType must be a compile error here, not an unknown tag. A byte
    // that is no ValueType at all falls out of the switch.
    switch (type) {
      case ValueType::kValue:
        if (!GetLengthPrefixed(&input, &key) || !GetLengthPrefixed(&input, &value)) {
          return Status::Corruption("bad WriteBatch Put");
        }
        handler->Put(key, value);
        found++;
        continue;
      case ValueType::kDeletion:
        if (!GetLengthPrefixed(&input, &key)) {
          return Status::Corruption("bad WriteBatch Delete");
        }
        handler->Delete(key);
        found++;
        continue;
    }
    return Status::Corruption("unknown WriteBatch tag");
  }
  if (found != Count()) {
    return Status::Corruption("WriteBatch has wrong count");
  }
  return Status::OK();
}

uint32_t WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + kCountOffset);
}

void WriteBatchInternal::SetCount(WriteBatch* b, uint32_t n) {
  EncodeFixed32(&b->rep_[kCountOffset], n);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return DecodeFixed64(b->rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber s) {
  EncodeFixed64(&b->rep_[0], s);
}

std::string_view WriteBatchInternal::Contents(const WriteBatch* b) { return b->rep_; }

size_t WriteBatchInternal::ByteSize(const WriteBatch* b) { return b->rep_.size(); }

Status WriteBatchInternal::SetContents(WriteBatch* b, std::string_view contents) {
  if (contents.size() < kHeaderSize) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  b->rep_.assign(contents.data(), contents.size());
  b->payload_bytes_ = 0;
  b->oversized_ = false;
  return Status::OK();
}

Status WriteBatchInternal::InsertInto(const WriteBatch* b, MemTable* mem) {
  MemTableInserter inserter(Sequence(b), mem);
  Status s = b->Iterate(&inserter);
  if (s.ok() && inserter.out_of_range()) {
    s = Status::Corruption("WriteBatch sequence number out of range");
  }
  return s;
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  const uint32_t dst_count = Count(dst);
  const uint32_t src_count = Count(src);
  SHALE_CHECK(src_count <= std::numeric_limits<uint32_t>::max() - dst_count,
              "appending would give a WriteBatch more than 2^32 - 1 operations");
  SetCount(dst, dst_count + src_count);
  dst->rep_.append(src->rep_, kHeaderSize, std::string::npos);
  dst->payload_bytes_ += src->payload_bytes_;
  dst->oversized_ = dst->oversized_ || src->oversized_;
}

uint64_t WriteBatchInternal::PayloadBytes(const WriteBatch* b) { return b->payload_bytes_; }

bool WriteBatchInternal::HasOversizedEntry(const WriteBatch* b) { return b->oversized_; }

}  // namespace shale
