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

LookupKey::LookupKey(std::string_view user_key, SequenceNumber sequence) {
  const uint64_t internal_size = uint64_t{user_key.size()} + kInternalKeyTagSize;
  SHALE_CHECK(internal_size <= std::numeric_limits<uint32_t>::max(),
              "user key is too long to be length-prefixed");
  PutVarint32(&rep_, static_cast<uint32_t>(internal_size));
  internal_key_start_ = rep_.size();
  AppendInternalKey(&rep_, {user_key, sequence, kValueTypeForSeek});
}

}  // namespace shale
