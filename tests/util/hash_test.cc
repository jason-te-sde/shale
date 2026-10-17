#include "util/hash.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace shale {
namespace {

TEST(HashTest, KnownValues) {
  // Bloom filters are written to disk with this hash, so these values must never change.
  const uint8_t d1[] = {0x62}, d2[] = {0xc3, 0x97}, d3[] = {0xe2, 0x99, 0xa5},
                d4[] = {0xe1, 0x80, 0xb9, 0x32};
  EXPECT_EQ(Hash(nullptr, 0, 0xbc9f1d34), 0xbc9f1d34u);
  EXPECT_EQ(Hash(reinterpret_cast<const char*>(d1), 1, 0xbc9f1d34), 0xef1345c4u);
  EXPECT_EQ(Hash(reinterpret_cast<const char*>(d2), 2, 0xbc9f1d34), 0x5b663814u);
  EXPECT_EQ(Hash(reinterpret_cast<const char*>(d3), 3, 0xbc9f1d34), 0x323c078fu);
  EXPECT_EQ(Hash(reinterpret_cast<const char*>(d4), 4, 0xbc9f1d34), 0xed21633au);
}

TEST(HashTest, SeedChangesResult) {
  const char data[] = "the same bytes";
  const uint32_t a = Hash(data, sizeof(data) - 1, 1);
  const uint32_t b = Hash(data, sizeof(data) - 1, 2);
  EXPECT_NE(a, b);
  EXPECT_EQ(a, Hash(data, sizeof(data) - 1, 1)) << "the hash is a pure function of its inputs";
}

TEST(HashTest, EveryByteAndTheLengthMatter) {
  // Lengths 0 to 9 cover the four-byte main loop and each of the three tail cases.
  const char data[] = "abcdefghi";
  uint32_t previous = Hash(data, 0, 7);
  for (size_t n = 1; n <= 9; n++) {
    const uint32_t h = Hash(data, n, 7);
    EXPECT_NE(h, previous) << "lengths " << n - 1 << " and " << n << " collide";
    previous = h;

    char changed[10];
    std::memcpy(changed, data, n);
    changed[n - 1] ^= 1;
    EXPECT_NE(Hash(changed, n, 7), h) << "flipping the last of " << n << " bytes changed nothing";
  }
}

TEST(HashTest, DoesNotDependOnAlignment) {
  const char data[] = "alignment must not matter to a value stored on disk";
  const size_t n = sizeof(data) - 1;
  const uint32_t want = Hash(data, n, 0xbc9f1d34);
  std::vector<char> buffer(n + 8);
  for (size_t offset = 0; offset < 8; offset++) {
    std::memcpy(buffer.data() + offset, data, n);
    EXPECT_EQ(Hash(buffer.data() + offset, n, 0xbc9f1d34), want) << "offset " << offset;
  }
}

}  // namespace
}  // namespace shale
