#include "util/hash.h"

#include <cstddef>
#include <cstdint>

#include "util/coding.h"

namespace shale {

uint32_t Hash(const char* data, size_t n, uint32_t seed) {
  const uint32_t m = 0xc6a4a793;
  const uint32_t r = 24;
  const char* limit = data + n;
  uint32_t h = seed ^ static_cast<uint32_t>(n * m);

  while (limit - data >= 4) {
    h += DecodeFixed32(data);
    data += 4;
    h *= m;
    h ^= (h >> 16);
  }

  switch (limit - data) {
    case 3:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16;
      [[fallthrough]];
    case 2:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8;
      [[fallthrough]];
    case 1:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[0]));
      h *= m;
      h ^= (h >> r);
      break;
    default:
      break;  // no bytes left over
  }
  return h;
}

}  // namespace shale
