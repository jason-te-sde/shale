#include "util/key_buffer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace shale {
namespace {

// Whether the bytes of `view` lie inside the object `buffer`, which is to say in place.
bool InPlace(const KeyBuffer& buffer, std::string_view view) {
  const auto begin = reinterpret_cast<uintptr_t>(&buffer);
  const auto end = begin + sizeof(buffer);
  const auto at = reinterpret_cast<uintptr_t>(view.data());
  return at >= begin && at + view.size() <= end;
}

std::string Pattern(size_t n, char first) {
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s.push_back(static_cast<char>(first + static_cast<char>(i % 23)));
  }
  return s;
}

TEST(KeyBufferTest, StartsEmpty) {
  KeyBuffer buffer;
  EXPECT_TRUE(buffer.view().empty());
}

TEST(KeyBufferTest, KeepsUpToSixtyFourBytesInPlaceAndGrowsBeyond) {
  static_assert(KeyBuffer::kInline == 64);
  KeyBuffer buffer;
  const std::string bytes = Pattern(300, 'a');
  size_t have = 0;
  for (const size_t piece :
       {size_t{1}, size_t{30}, size_t{33}, size_t{1}, size_t{35}, size_t{200}}) {
    buffer.Append(bytes.data() + have, piece);
    have += piece;
    EXPECT_EQ(buffer.view(), std::string_view(bytes).substr(0, have));
    EXPECT_EQ(InPlace(buffer, buffer.view()), have <= KeyBuffer::kInline) << have;
  }
  EXPECT_EQ(have, bytes.size());

  // Exactly sixty-four bytes is still in place, and one more is not.
  KeyBuffer exact;
  exact.Append(bytes.data(), 64);
  EXPECT_TRUE(InPlace(exact, exact.view()));
  EXPECT_EQ(exact.view(), std::string_view(bytes).substr(0, 64));
  exact.Append(bytes.data() + 64, 1);
  EXPECT_FALSE(InPlace(exact, exact.view()));
  EXPECT_EQ(exact.view(), std::string_view(bytes).substr(0, 65));
}

TEST(KeyBufferTest, ClearEmptiesTheBufferAndItIsUsableAgain) {
  KeyBuffer buffer;
  const std::string big = Pattern(500, 'A');
  buffer.Append(big.data(), big.size());
  buffer.Clear();
  EXPECT_TRUE(buffer.view().empty());
  buffer.Append("abc", 3);
  EXPECT_EQ(buffer.view(), "abc");
  buffer.Append(big.data(), big.size());
  EXPECT_EQ(buffer.view(), "abc" + big);
  buffer.Append(nullptr, 0);
  EXPECT_EQ(buffer.view(), "abc" + big);
}

TEST(KeyBufferTest, AssignPrefixAcrossInlineAndHeap) {
  const std::string small = Pattern(40, 'a');
  const std::string large = Pattern(400, 'A');
  KeyBuffer from_small;
  from_small.Append(small.data(), small.size());
  KeyBuffer from_large;
  from_large.Append(large.data(), large.size());

  // Into a buffer that holds something else, whatever it is on the heap or not.
  KeyBuffer into;
  into.Append(large.data(), large.size());
  for (const size_t n : {size_t{0}, size_t{1}, size_t{39}, size_t{40}}) {
    into.AssignPrefix(from_small.view(), n);
    EXPECT_EQ(into.view(), std::string_view(small).substr(0, n)) << n;
  }
  for (const size_t n : {size_t{0}, size_t{63}, size_t{64}, size_t{65}, size_t{400}}) {
    into.AssignPrefix(from_large.view(), n);
    EXPECT_EQ(into.view(), std::string_view(large).substr(0, n)) << n;
  }
  // A buffer that has grown keeps what it grew to; a new one keeps a short prefix in place.
  for (const size_t n : {size_t{0}, size_t{63}, size_t{64}, size_t{65}, size_t{400}}) {
    KeyBuffer fresh;
    fresh.AssignPrefix(from_large.view(), n);
    EXPECT_EQ(fresh.view(), std::string_view(large).substr(0, n)) << n;
    EXPECT_EQ(InPlace(fresh, fresh.view()), n <= KeyBuffer::kInline) << n;
  }

  KeyBuffer grown;
  grown.AssignPrefix(from_large.view(), 100);
  EXPECT_EQ(grown.view(), std::string_view(large).substr(0, 100));
  grown.Append(small.data(), small.size());
  EXPECT_EQ(grown.view(), large.substr(0, 100) + small);
}

TEST(KeyBufferTest, AssignPrefixOfItsOwnContents) {
  const std::string large = Pattern(200, 'A');
  KeyBuffer buffer;
  buffer.Append(large.data(), large.size());
  buffer.AssignPrefix(buffer.view(), 150);
  EXPECT_EQ(buffer.view(), std::string_view(large).substr(0, 150));
  buffer.AssignPrefix(buffer.view(), 10);
  EXPECT_EQ(buffer.view(), std::string_view(large).substr(0, 10));
  buffer.Append("z", 1);
  EXPECT_EQ(buffer.view(), large.substr(0, 10) + "z");
}

}  // namespace
}  // namespace shale
