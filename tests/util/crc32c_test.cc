#include "util/crc32c.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "util/random.h"

namespace shale {
namespace {

// CRC32C one bit at a time, straight from the definition: reflected polynomial 0x82f63b78, all
// ones in and out. Slow, and unrelated to the table-driven and hardware code it checks.
uint32_t BitwiseCrc32c(uint32_t init_crc, const char* data, size_t n) {
  uint32_t c = ~init_crc;
  for (size_t i = 0; i < n; i++) {
    c ^= static_cast<uint8_t>(data[i]);
    for (int bit = 0; bit < 8; bit++) {
      c = (c & 1) ? (c >> 1) ^ 0x82f63b78u : c >> 1;
    }
  }
  return ~c;
}

TEST(Crc32cTest, StandardVectors) {
  EXPECT_EQ(crc32c::Value("123456789", 9), 0xe3069283u);

  // The test vectors of RFC 3720, appendix B.4.
  char buf[32];
  for (char& b : buf) b = 0;
  EXPECT_EQ(crc32c::Value(buf, sizeof(buf)), 0x8a9136aau);
  for (char& b : buf) b = static_cast<char>(0xff);
  EXPECT_EQ(crc32c::Value(buf, sizeof(buf)), 0x62a8ab43u);
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = static_cast<char>(i);
  EXPECT_EQ(crc32c::Value(buf, sizeof(buf)), 0x46dd794eu);
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = static_cast<char>(31 - i);
  EXPECT_EQ(crc32c::Value(buf, sizeof(buf)), 0x113fdb5cu);
}

TEST(Crc32cTest, ExtendEqualsWhole) {
  EXPECT_EQ(crc32c::Extend(crc32c::Value("hello ", 6), "world", 5),
            crc32c::Value("hello world", 11));
  EXPECT_EQ(crc32c::ExtendPortable(crc32c::Value("hello ", 6), "world", 5),
            crc32c::Value("hello world", 11));
}

TEST(Crc32cTest, EmptyInputLeavesTheCrcUnchanged) {
  EXPECT_EQ(crc32c::Value(nullptr, 0), 0u);
  const uint32_t crc = crc32c::Value("abc", 3);
  EXPECT_EQ(crc32c::Extend(crc, nullptr, 0), crc);
  EXPECT_EQ(crc32c::ExtendPortable(crc, nullptr, 0), crc);
}

TEST(Crc32cTest, MaskRoundTrip) {
  const uint32_t crc = crc32c::Value("foo", 3);
  EXPECT_NE(crc32c::Mask(crc), crc);
  EXPECT_NE(crc32c::Mask(crc32c::Mask(crc)), crc);
  EXPECT_EQ(crc32c::Unmask(crc32c::Mask(crc)), crc);
  EXPECT_EQ(crc32c::Unmask(crc32c::Unmask(crc32c::Mask(crc32c::Mask(crc)))), crc);

  for (uint32_t c : {0u, 1u, 0x80000000u, 0xffffffffu}) {
    EXPECT_EQ(crc32c::Unmask(crc32c::Mask(c)), c) << c;
  }
}

TEST(Crc32cTest, MaskUsesTheOnDiskFormula) {
  // Stored checksums are masked with ((crc >> 15) | (crc << 17)) + 0xa282ead8. A different delta or
  // rotation would still round-trip, and would make every existing file unreadable.
  EXPECT_EQ(crc32c::Mask(0), 0xa282ead8u);
  EXPECT_EQ(crc32c::Mask(0x80000000u), 0xa283ead8u);
  EXPECT_EQ(crc32c::Mask(0xffffffffu), 0xa282ead7u);
  EXPECT_EQ(crc32c::Mask(0xe3069283u), 0xc78ab0e5u);
}

TEST(Crc32cTest, PortableMatchesTheBitwiseDefinition) {
  constexpr uint64_t kSeed = 29;
  SCOPED_TRACE("seed " + std::to_string(kSeed));
  Random rnd(kSeed);
  std::vector<char> buffer(4096 + 8);
  for (int round = 0; round < 300; round++) {
    for (char& b : buffer) b = static_cast<char>(rnd.Next());
    const size_t offset = rnd.Uniform(8);
    const size_t length = rnd.Uniform(1025);
    const uint32_t init = static_cast<uint32_t>(rnd.Next());
    ASSERT_EQ(crc32c::ExtendPortable(init, buffer.data() + offset, length),
              BitwiseCrc32c(init, buffer.data() + offset, length))
        << "round " << round << ", offset " << offset << ", length " << length;
  }
}

TEST(Crc32cTest, HardwareMatchesPortable) {
  if (!crc32c::kHasHardwareSupport) {
    GTEST_SKIP() << "this build has no hardware CRC32C path, so Extend() is ExtendPortable()";
  }
  constexpr uint64_t kSeed = 17;
  SCOPED_TRACE("seed " + std::to_string(kSeed));
  Random rnd(kSeed);
  std::vector<char> buffer(4096 + 8);
  for (int round = 0; round < 1000; round++) {
    for (char& b : buffer) b = static_cast<char>(rnd.Next());
    const size_t offset = rnd.Uniform(8);
    const size_t length = rnd.Uniform(4097);
    const uint32_t init = static_cast<uint32_t>(rnd.Next());
    ASSERT_EQ(crc32c::Extend(init, buffer.data() + offset, length),
              crc32c::ExtendPortable(init, buffer.data() + offset, length))
        << "round " << round << ", offset " << offset << ", length " << length;
  }
}

}  // namespace
}  // namespace shale
