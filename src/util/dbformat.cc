#include "util/dbformat.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "util/check.h"
#include "util/coding.h"

namespace shale {

uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t) {
  SHALE_CHECK(seq <= kMaxSequenceNumber, "sequence number does not fit in 56 bits");
  return (seq << 8) | static_cast<uint8_t>(t);
}

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result) {
  if (internal_key.size() < kInternalKeyTagSize) {
    return false;
  }
  const uint64_t tag =
      DecodeFixed64(internal_key.data() + internal_key.size() - kInternalKeyTagSize);
  const uint8_t type = static_cast<uint8_t>(tag & 0xff);
  if (type > static_cast<uint8_t>(kValueTypeForSeek)) {
    return false;
  }
  result->user_key = internal_key.substr(0, internal_key.size() - kInternalKeyTagSize);
  result->sequence = tag >> 8;
  result->type = static_cast<ValueType>(type);
  return true;
}

std::string_view ExtractUserKey(std::string_view internal_key) {
  SHALE_CHECK(internal_key.size() >= kInternalKeyTagSize,
              "internal key is shorter than its 8-byte tag");
  return internal_key.substr(0, internal_key.size() - kInternalKeyTagSize);
}

int BytewiseCompare(std::string_view a, std::string_view b) {
  // string_view::compare orders by unsigned bytes, since char_traits<char> does. Its result is
  // only specified by sign, and callers rely on -1, 0 or 1.
  const int r = a.compare(b);
  return (r > 0) - (r < 0);
}

int CompareInternalKey(std::string_view a, std::string_view b) {
  const int r = BytewiseCompare(ExtractUserKey(a), ExtractUserKey(b));
  if (r != 0) {
    return r;
  }
  // Same user key: the larger tag, which is the newer entry, sorts first.
  const uint64_t a_tag = DecodeFixed64(a.data() + a.size() - kInternalKeyTagSize);
  const uint64_t b_tag = DecodeFixed64(b.data() + b.size() - kInternalKeyTagSize);
  if (a_tag > b_tag) {
    return -1;
  }
  if (a_tag < b_tag) {
    return 1;
  }
  return 0;
}

LookupKey::LookupKey(std::string_view user_key, SequenceNumber sequence) {
  const uint64_t internal_size = uint64_t{user_key.size()} + kInternalKeyTagSize;
  SHALE_CHECK(internal_size <= std::numeric_limits<uint32_t>::max(),
              "user key is too long to be length-prefixed");
  PutVarint32(&rep_, static_cast<uint32_t>(internal_size));
  internal_key_start_ = rep_.size();
  AppendInternalKey(&rep_, {user_key, sequence, kValueTypeForSeek});
}

}  // namespace shale
