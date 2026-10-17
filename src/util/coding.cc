#include "util/coding.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "util/check.h"

namespace shale {

namespace {

// Decodes a varint64 from [p, limit). Nine bytes carry 63 bits, so a tenth byte may add only the
// top bit and may not say that more follows.
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* v) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    const uint64_t byte = static_cast<uint8_t>(*p);
    p++;
    if ((byte & 0x80) != 0) {
      result |= (byte & 0x7f) << shift;
    } else {
      if (shift == 63 && byte > 1) {
        return nullptr;
      }
      *v = result | (byte << shift);
      return p;
    }
  }
  return nullptr;
}

}  // namespace

void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  EncodeFixed32(buf, v);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  EncodeFixed64(buf, v);
  dst->append(buf, sizeof(buf));
}

void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  size_t n = 0;
  while (v >= 0x80) {
    buf[n++] = static_cast<char>(static_cast<uint8_t>(v) | 0x80);
    v >>= 7;
  }
  buf[n++] = static_cast<char>(v);
  dst->append(buf, n);
}

void PutVarint32(std::string* dst, uint32_t v) { PutVarint64(dst, v); }

void PutLengthPrefixed(std::string* dst, std::string_view value) {
  SHALE_CHECK(static_cast<uint64_t>(value.size()) <= std::numeric_limits<uint32_t>::max(),
              "length-prefixed value is longer than 4 GiB");
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

int VarintLength(uint64_t v) {
  int length = 1;
  while (v >= 0x80) {
    v >>= 7;
    length++;
  }
  return length;
}

bool GetFixed32(std::string_view* input, uint32_t* v) {
  if (input->size() < sizeof(uint32_t)) {
    return false;
  }
  const char* const p = input->data();
  *v = DecodeFixed32(p);
  input->remove_prefix(sizeof(uint32_t));
  return true;
}

bool GetFixed64(std::string_view* input, uint64_t* v) {
  if (input->size() < sizeof(uint64_t)) {
    return false;
  }
  const char* const p = input->data();
  *v = DecodeFixed64(p);
  input->remove_prefix(sizeof(uint64_t));
  return true;
}

namespace internal {

const char* GetVarint32PtrFallback(const char* p, const char* limit, uint32_t* v) {
  uint32_t result = 0;
  // Four bytes carry 28 bits, so a fifth may add only the top four and may not say that more
  // follows; an encoding that gets that far with the high bit still set runs out of loop.
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    const uint32_t byte = static_cast<uint8_t>(*p);
    p++;
    if ((byte & 0x80) != 0) {
      result |= (byte & 0x7f) << shift;
    } else {
      if (shift == 28 && byte > 0x0f) {
        return nullptr;
      }
      *v = result | (byte << shift);
      return p;
    }
  }
  return nullptr;
}

}  // namespace internal

bool GetVarint32(std::string_view* input, uint32_t* v) {
  const char* const p = input->data();
  const char* const limit = p + input->size();
  const char* const end = GetVarint32Ptr(p, limit, v);
  if (end == nullptr) {
    return false;
  }
  *input = std::string_view(end, static_cast<size_t>(limit - end));
  return true;
}

bool GetVarint64(std::string_view* input, uint64_t* v) {
  const char* const p = input->data();
  const char* const limit = p + input->size();
  const char* const end = GetVarint64Ptr(p, limit, v);
  if (end == nullptr) {
    return false;
  }
  *input = std::string_view(end, static_cast<size_t>(limit - end));
  return true;
}

bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  std::string_view rest = *input;
  uint32_t length = 0;
  if (!GetVarint32(&rest, &length) || rest.size() < length) {
    return false;
  }
  *result = rest.substr(0, length);
  rest.remove_prefix(length);
  *input = rest;
  return true;
}

}  // namespace shale
