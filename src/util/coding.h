#ifndef SHALE_UTIL_CODING_H_
#define SHALE_UTIL_CODING_H_

// Byte-level encodings of integers and strings. Everything is little-endian whatever the host,
// because these bytes are written to files and read back by another machine.
//
// A fixed-width integer takes 4 or 8 bytes. A varint takes 7 bits of the value per byte, low bits
// first, with the high bit of each byte set when another byte follows: 1 to 5 bytes for 32 bits,
// 1 to 10 for 64. A length-prefixed string is a varint32 length followed by that many bytes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace shale {

/// Writes `v` to `dst[0..3]`, least significant byte first.
inline void EncodeFixed32(char* dst, uint32_t v) {
  auto* p = reinterpret_cast<uint8_t*>(dst);
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

/// Writes `v` to `dst[0..7]`, least significant byte first.
inline void EncodeFixed64(char* dst, uint64_t v) {
  EncodeFixed32(dst, static_cast<uint32_t>(v));
  EncodeFixed32(dst + 4, static_cast<uint32_t>(v >> 32));
}

/// Reads the 4 bytes at `p` as a little-endian integer. `p` need not be aligned.
inline uint32_t DecodeFixed32(const char* p) {
  const auto* b = reinterpret_cast<const uint8_t*>(p);
  return uint32_t{b[0]} | (uint32_t{b[1]} << 8) | (uint32_t{b[2]} << 16) | (uint32_t{b[3]} << 24);
}

/// Reads the 8 bytes at `p` as a little-endian integer. `p` need not be aligned.
inline uint64_t DecodeFixed64(const char* p) {
  return uint64_t{DecodeFixed32(p)} | (uint64_t{DecodeFixed32(p + 4)} << 32);
}

/// Appends `v` to `*dst` as 4 little-endian bytes.
void PutFixed32(std::string* dst, uint32_t v);

/// Appends `v` to `*dst` as 8 little-endian bytes.
void PutFixed64(std::string* dst, uint64_t v);

/// Appends `v` to `*dst` as a varint of 1 to 5 bytes.
void PutVarint32(std::string* dst, uint32_t v);

/// Appends `v` to `*dst` as a varint of 1 to 10 bytes.
void PutVarint64(std::string* dst, uint64_t v);

/// Appends `value` to `*dst` as a varint32 length followed by the bytes. Aborts if the length does
/// not fit in 32 bits, which would otherwise write a prefix that disagrees with the data.
void PutLengthPrefixed(std::string* dst, std::string_view value);

/// Returns the number of bytes PutVarint64() writes for `v`, from 1 to 10.
int VarintLength(uint64_t v);

/// Reads a 4-byte integer from the front of `*input` into `*v` and removes those bytes from
/// `*input`. Returns false if fewer than 4 bytes remain; `*input` and `*v` are then unchanged.
bool GetFixed32(std::string_view* input, uint32_t* v);

/// Like GetFixed32(), for 8 bytes.
bool GetFixed64(std::string_view* input, uint64_t* v);

/// Reads a varint from the front of `*input` into `*v` and removes its bytes from `*input`.
/// Returns false, leaving `*input` and `*v` unchanged, if the input ends inside the varint or if
/// the value does not fit in 32 bits. Encodings with redundant zero bytes are accepted.
bool GetVarint32(std::string_view* input, uint32_t* v);

/// Like GetVarint32(), for values that must fit in 64 bits.
bool GetVarint64(std::string_view* input, uint64_t* v);

/// Reads a length-prefixed string from the front of `*input`. `*result` then views the string's
/// bytes inside the input, and both the prefix and the bytes are removed from `*input`. Returns
/// false, changing nothing, if the prefix is malformed or the input is shorter than it says.
bool GetLengthPrefixed(std::string_view* input, std::string_view* result);

namespace internal {

/// The multi-byte case of GetVarint32Ptr(), out of line to keep the one-byte case small.
const char* GetVarint32PtrFallback(const char* p, const char* limit, uint32_t* v);

}  // namespace internal

/// Decodes a varint32 from [p, limit) into `*v` and returns the position just past it, or nullptr,
/// leaving `*v` unchanged, if the range ends inside the varint or the value does not fit in 32
/// bits. Never reads at or beyond `limit`. This is the form for hot paths that already hold a raw
/// pointer, such as the block decoder.
inline const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* v) {
  if (p < limit) {
    const uint32_t first = static_cast<uint8_t>(*p);
    if ((first & 0x80) == 0) {
      *v = first;
      return p + 1;
    }
  }
  return internal::GetVarint32PtrFallback(p, limit, v);
}

}  // namespace shale

#endif  // SHALE_UTIL_CODING_H_
