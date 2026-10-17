#include "util/crc32c.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "util/coding.h"

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace shale::crc32c {

namespace {

// The reflected form of the Castagnoli polynomial 0x1edc6f41.
constexpr uint32_t kPolynomial = 0x82f63b78;

constexpr uint32_t kMaskDelta = 0xa282ead8;

using Tables = std::array<std::array<uint32_t, 256>, 8>;

// Slicing-by-8: table 0 is the classic byte-at-a-time table, and table k is table k-1 advanced by
// one more zero byte. Together they let the loop below fold eight input bytes per step. Built at
// compile time, so there is no mutable static state and no initialisation order to worry about.
constexpr Tables MakeTables() {
  Tables tables{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 1) != 0 ? (crc >> 1) ^ kPolynomial : crc >> 1;
    }
    tables[0][i] = crc;
  }
  for (size_t k = 1; k < tables.size(); k++) {
    for (uint32_t i = 0; i < 256; i++) {
      const uint32_t previous = tables[k - 1][i];
      tables[k][i] = (previous >> 8) ^ tables[0][previous & 0xff];
    }
  }
  return tables;
}

constexpr Tables kTables = MakeTables();

}  // namespace

uint32_t ExtendPortable(uint32_t init_crc, const char* data, size_t n) {
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  uint32_t crc = ~init_crc;
  while (n >= 8) {
    const uint32_t low = DecodeFixed32(reinterpret_cast<const char*>(p)) ^ crc;
    const uint32_t high = DecodeFixed32(reinterpret_cast<const char*>(p + 4));
    crc = kTables[7][low & 0xff] ^ kTables[6][(low >> 8) & 0xff] ^ kTables[5][(low >> 16) & 0xff] ^
          kTables[4][low >> 24] ^ kTables[3][high & 0xff] ^ kTables[2][(high >> 8) & 0xff] ^
          kTables[1][(high >> 16) & 0xff] ^ kTables[0][high >> 24];
    p += 8;
    n -= 8;
  }
  while (n > 0) {
    crc = kTables[0][(crc ^ *p) & 0xff] ^ (crc >> 8);
    p++;
    n--;
  }
  return ~crc;
}

#if defined(__ARM_FEATURE_CRC32)

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  const char* p = data;
  uint32_t crc = ~init_crc;
  // DecodeFixed64 reads the eight bytes in memory order whatever the host's endianness, which is
  // the order the instruction consumes them in, and compiles to one unaligned load.
  while (n >= 8) {
    crc = __crc32cd(crc, DecodeFixed64(p));
    p += 8;
    n -= 8;
  }
  while (n > 0) {
    crc = __crc32cb(crc, static_cast<uint8_t>(*p));
    p++;
    n--;
  }
  return ~crc;
}

#else

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  return ExtendPortable(init_crc, data, n);
}

#endif

uint32_t Mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + kMaskDelta; }

uint32_t Unmask(uint32_t masked) {
  const uint32_t rotated = masked - kMaskDelta;
  return (rotated >> 17) | (rotated << 15);
}

}  // namespace shale::crc32c
