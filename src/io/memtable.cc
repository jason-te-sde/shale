#include "io/memtable.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
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

// The size of the tag that ends an internal key.
constexpr size_t kTagSize = 8;

// The most bytes a varint32 takes.
constexpr size_t kMaxVarint32Size = 5;

// Marks the memtable as being added to for as long as it lives, and aborts if it already was. The
// release on exit and the acquire on entry hand the arena's and the skip list's plain state from
// one writer thread to the next, which is allowed as long as they do not overlap.
class AddScope {
 public:
  explicit AddScope(std::atomic<bool>* flag) : flag_(flag) {
    SHALE_CHECK(!flag->exchange(true, std::memory_order_acquire), "concurrent MemTable::Add");
  }

  AddScope(const AddScope&) = delete;
  AddScope& operator=(const AddScope&) = delete;

  ~AddScope() { flag_->store(false, std::memory_order_release); }

 private:
  std::atomic<bool>* const flag_;
};

// Decodes the length-prefixed internal key at the start of an entry, or of a lookup key's
// memtable_key(). Only the bytes of the length and the key are read, so the pointer may be to
// anything that begins like one.
std::string_view EntryKey(const char* entry) {
  uint32_t length = 0;
  const char* const key = GetVarint32Ptr(entry, entry + kMaxVarint32Size, &length);
  SHALE_CHECK(key != nullptr, "memtable entry has a malformed key length");
  return {key, length};
}

// Decodes the value of the entry whose internal key is `internal_key`, a view into the entry.
std::string_view EntryValue(std::string_view internal_key) {
  const char* const start = internal_key.data() + internal_key.size();
  uint32_t length = 0;
  const char* const value = GetVarint32Ptr(start, start + kMaxVarint32Size, &length);
  SHALE_CHECK(value != nullptr, "memtable entry has a malformed value length");
  return {value, length};
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return CompareInternalKey(EntryKey(a), EntryKey(b));
}

// An iterator over the skip list that shows entries as the internal key and value they hold. It
// owns a share of the memtable, so the arena the entries live in outlives it.
class MemTable::Cursor final : public Iterator {
 public:
  explicit Cursor(std::shared_ptr<const MemTable> mem)
      : mem_(std::move(mem)), iter_(&mem_->table_) {}

  bool Valid() const override { return iter_.Valid(); }

  void SeekToFirst() override { iter_.SeekToFirst(); }

  void Seek(std::string_view target) override {
    // Checked here, and not left to the comparator, which only runs if the memtable has an entry.
    SHALE_CHECK(target.size() >= kTagSize,
                "MemTable iterator: Seek takes an internal key, which is at least 8 bytes");
    // The skip list compares entries, whose keys carry a length in front, so the target gets one.
    SHALE_CHECK(target.size() <= std::numeric_limits<uint32_t>::max(),
                "MemTable iterator: Seek target is too long to be length-prefixed");
    scratch_.clear();
    PutVarint32(&scratch_, static_cast<uint32_t>(target.size()));
    scratch_.append(target);
    iter_.Seek(scratch_.data());
  }

  void Next() override { iter_.Next(); }

  std::string_view key() const override { return EntryKey(iter_.key()); }

  std::string_view value() const override { return EntryValue(key()); }

  Status status() const override { return Status::OK(); }

 private:
  const std::shared_ptr<const MemTable> mem_;
  Table::Iterator iter_;
  std::string scratch_;
};

MemTable::MemTable(uint64_t seed) : table_(KeyComparator(), &arena_, seed) {}

size_t MemTable::ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  // shared_from_this() would throw for a memtable that nothing owns, and nothing here throws.
  std::shared_ptr<const MemTable> self = weak_from_this().lock();
  SHALE_CHECK(self != nullptr,
              "MemTable must be owned by a shared_ptr: create it with make_shared");
  return std::make_unique<Cursor>(std::move(self));
}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value) {
  // The guard covers the allocation as well as the insert: the arena is the part that is not
  // safe for two writers, and the skip list guards itself.
  const AddScope scope(&adding_);
  SHALE_CHECK(type == ValueType::kValue || value.empty(), "a deletion carries no value");

  constexpr size_t kMaxLength = std::numeric_limits<uint32_t>::max();
  SHALE_CHECK(key.size() <= kMaxLength - kTagSize,
              "MemTable::Add: key is too long to be length-prefixed");
  SHALE_CHECK(value.size() <= kMaxLength, "MemTable::Add: value is too long to be length-prefixed");
  const size_t internal_size = key.size() + kTagSize;
  const size_t encoded_size = static_cast<size_t>(VarintLength(internal_size)) + internal_size +
                              static_cast<size_t>(VarintLength(value.size())) + value.size();

  char* const entry = arena_.Allocate(encoded_size);
  char* p = EncodeVarint32(entry, static_cast<uint32_t>(internal_size));
  p = std::copy(key.begin(), key.end(), p);
  EncodeFixed64(p, PackSequenceAndType(seq, type));
  p += kTagSize;
  p = EncodeVarint32(p, static_cast<uint32_t>(value.size()));
  p = std::copy(value.begin(), value.end(), p);
  SHALE_CHECK(p == entry + encoded_size, "MemTable::Add wrote a different size than it allocated");
  table_.Insert(entry);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) const {
  Table::Iterator iter(&table_);
  iter.Seek(key.memtable_key().data());
  if (!iter.Valid()) {
    return false;
  }
  // The seek lands on the first entry at or after the lookup key. If it is for the same user key
  // it is the newest one at or before the snapshot; if not, there is no entry for the key at all.
  const std::string_view internal_key = EntryKey(iter.key());
  if (BytewiseCompare(ExtractUserKey(internal_key), key.user_key()) != 0) {
    return false;
  }
  ParsedInternalKey parsed;
  SHALE_CHECK(ParseInternalKey(internal_key, &parsed),
              "memtable entry has an invalid internal key");
  // No default branch: a new ValueType must be a compile error here.
  switch (parsed.type) {
    case ValueType::kValue:
      value->assign(EntryValue(internal_key));
      *s = Status::OK();
      return true;
    case ValueType::kDeletion:
      *s = Status::NotFound("key deleted");
      return true;
  }
  SHALE_CHECK(false, "memtable entry has an unknown type");
}

}  // namespace shale
