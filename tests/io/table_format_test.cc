// The on-disk pieces every table is made of: the block handle, the footer and the block trailer.
//
// These are read from files that may be damaged or hostile, so most of what is tested here is what
// the decoders refuse. The bytes the encoders write are part of the file format, and are checked
// against the layout the format states and not only against the decoders.

#include "io/table_format.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "io/env_util.h"
#include "shale/env.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace shale {
namespace {

// Where the fields of the footer are. The format says 48 bytes: four fixed64s for the two handles,
// then the version, the checksum of everything before it, and the magic number.
constexpr size_t kVersionOffset = 32;
constexpr size_t kChecksumOffset = 36;
constexpr size_t kMagicOffset = 40;

std::string EncodedFooter() {
  Footer footer;
  footer.filter = BlockHandle{1000, 20};
  footer.index = BlockHandle{1025, 300};
  std::string encoded;
  footer.EncodeTo(&encoded);
  return encoded;
}

// Stores the checksum the footer's first 36 bytes call for, so that a test that changes a field
// gets a footer that is wrong in that field only.
void RecomputeFooterChecksum(std::string* footer) {
  EncodeFixed32(footer->data() + kChecksumOffset, crc32c::Value(footer->data(), kChecksumOffset));
}

std::string EncodedHandle(const BlockHandle& handle) {
  std::string encoded;
  handle.EncodeTo(&encoded);
  return encoded;
}

TEST(TableFormatTest, BlockHandleRoundTripAndTruncation) {
  for (const BlockHandle& handle : {BlockHandle{0, 0}, BlockHandle{uint64_t{1} << 40, 4096},
                                    BlockHandle{std::numeric_limits<uint64_t>::max(), 1}}) {
    const std::string encoded = EncodedHandle(handle);
    std::string with_tail = encoded + "tail";
    std::string_view input = with_tail;
    BlockHandle decoded;
    ASSERT_OK(decoded.DecodeFrom(&input));
    EXPECT_EQ(decoded.offset, handle.offset);
    EXPECT_EQ(decoded.size, handle.size);
    EXPECT_EQ(input, "tail") << "decoding takes the handle and leaves what follows";

    for (size_t len = 0; len < encoded.size(); ++len) {
      std::string_view prefix(encoded.data(), len);
      const Status s = BlockHandle().DecodeFrom(&prefix);
      EXPECT_EQ(s.ToString(), "Corruption: bad block handle")
          << "a prefix of " << len << " of " << encoded.size() << " bytes";
      EXPECT_EQ(prefix.size(), len) << "a failed decode takes nothing from the input";
    }
  }
}

TEST(TableFormatTest, BlockHandleRejectsVarintsThatDoNotFit) {
  // Eleven bytes with the continuation bit set never end a 64-bit varint.
  const std::string endless(11, '\x80');
  std::string_view input = endless;
  EXPECT_EQ(BlockHandle().DecodeFrom(&input).ToString(), "Corruption: bad block handle");
  // A first field that is fine does not rescue a second that is cut short.
  std::string first_only;
  PutVarint64(&first_only, 7);
  std::string_view cut = first_only;
  EXPECT_EQ(BlockHandle().DecodeFrom(&cut).ToString(), "Corruption: bad block handle");
}

TEST(TableFormatTest, FooterRoundTripIsExactly48Bytes) {
  Footer footer;
  footer.filter = BlockHandle{0x0102030405060708ULL, 0x1112131415161718ULL};
  footer.index = BlockHandle{0x2122232425262728ULL, 0x3132333435363738ULL};
  std::string encoded = "prefix";
  footer.EncodeTo(&encoded);
  ASSERT_EQ(encoded.size(), std::string("prefix").size() + kFooterSize)
      << "EncodeTo appends, and appends 48 bytes";
  const std::string bytes = encoded.substr(6);
  ASSERT_EQ(bytes.size(), size_t{48});

  // The layout the format states, field by field.
  EXPECT_EQ(DecodeFixed64(bytes.data()), 0x0102030405060708ULL) << "filter offset";
  EXPECT_EQ(DecodeFixed64(bytes.data() + 8), 0x1112131415161718ULL) << "filter size";
  EXPECT_EQ(DecodeFixed64(bytes.data() + 16), 0x2122232425262728ULL) << "index offset";
  EXPECT_EQ(DecodeFixed64(bytes.data() + 24), 0x3132333435363738ULL) << "index size";
  EXPECT_EQ(DecodeFixed32(bytes.data() + kVersionOffset), kTableFormatVersion);
  EXPECT_EQ(DecodeFixed32(bytes.data() + kChecksumOffset), crc32c::Value(bytes.data(), 36))
      << "the checksum covers the first 36 bytes";
  EXPECT_EQ(DecodeFixed64(bytes.data() + kMagicOffset), uint64_t{0x7368616c65000001ULL});
  EXPECT_EQ(bytes[kMagicOffset], '\x01') << "little-endian: the low byte of the magic comes first";

  Footer decoded;
  ASSERT_OK(decoded.DecodeFrom(bytes));
  EXPECT_EQ(decoded.filter.offset, footer.filter.offset);
  EXPECT_EQ(decoded.filter.size, footer.filter.size);
  EXPECT_EQ(decoded.index.offset, footer.index.offset);
  EXPECT_EQ(decoded.index.size, footer.index.size);
}

TEST(TableFormatTest, FooterRejectsBadMagic) {
  std::string footer = EncodedFooter();
  footer[footer.size() - 1] = static_cast<char>(footer[footer.size() - 1] ^ 0x01);
  EXPECT_EQ(Footer().DecodeFrom(footer).ToString(),
            "Corruption: not an sstable (bad magic number)");
}

TEST(TableFormatTest, FooterRejectsUnknownVersion) {
  for (const uint32_t version : {uint32_t{0}, uint32_t{2}, uint32_t{0xffffffffU}}) {
    std::string footer = EncodedFooter();
    EncodeFixed32(footer.data() + kVersionOffset, version);
    RecomputeFooterChecksum(&footer);
    EXPECT_EQ(Footer().DecodeFrom(footer).ToString(),
              "Corruption: unsupported table format version")
        << "version " << version << " with a checksum that matches it";
  }
}

TEST(TableFormatTest, FooterRejectsBadChecksum) {
  // Every byte the checksum covers, and the checksum itself: a flipped bit anywhere before the
  // magic number is caught, whichever field it is in.
  const std::string original = EncodedFooter();
  for (size_t i = 0; i < kMagicOffset; ++i) {
    std::string footer = original;
    footer[i] = static_cast<char>(footer[i] ^ 0x10);
    EXPECT_EQ(Footer().DecodeFrom(footer).ToString(), "Corruption: footer checksum mismatch")
        << "byte " << i << " flipped";
  }
}

TEST(TableFormatTest, FooterRejectsWrongSize) {
  const std::string footer = EncodedFooter();
  ASSERT_EQ(footer.size(), kFooterSize);
  for (const size_t size : {size_t{0}, size_t{1}, kFooterSize - 1}) {
    EXPECT_EQ(Footer().DecodeFrom(std::string_view(footer.data(), size)).ToString(),
              "Corruption: bad footer size")
        << size << " bytes";
  }
  EXPECT_EQ(Footer().DecodeFrom(footer + "x").ToString(), "Corruption: bad footer size");
}

TEST(TableFormatTest, FooterChecksTheMagicBeforeAnythingElse) {
  // A file that is not an sstable at all is told so, and not that its checksum is wrong.
  const std::string not_a_footer(kFooterSize, '\x5a');
  EXPECT_EQ(Footer().DecodeFrom(not_a_footer).ToString(),
            "Corruption: not an sstable (bad magic number)");
}

// The bytes of a block as a table stores them: the contents, the type byte and the masked CRC32C
// of the two.
std::string WithTrailer(std::string_view contents, char type) {
  std::string block(contents);
  block.push_back(type);
  PutFixed32(&block, crc32c::Mask(crc32c::Value(block.data(), block.size())));
  return block;
}

class ReadBlockTest : public testing::Test {
 protected:
  // Puts `bytes` in a file and opens it.
  void Store(const std::string& bytes) {
    ASSERT_OK(WriteStringToFile(env_.get(), bytes, "/blocks", false));
    ASSERT_OK(env_->NewRandomAccessFile("/blocks", &file_));
  }

  std::unique_ptr<Env> env_ = NewMemEnv();
  std::unique_ptr<RandomAccessFile> file_;
};

TEST_F(ReadBlockTest, VerifiesTrailer) {
  const std::string contents = "the contents of a block";
  const std::string prefix(13, '#');  // so that the block is not at offset 0
  const BlockHandle handle{prefix.size(), contents.size()};

  Store(prefix + WithTrailer(contents, 0));
  std::string read;
  ASSERT_OK(ReadBlock(file_.get(), handle, true, &read));
  EXPECT_EQ(read, contents) << "the contents, without the trailer";

  // One flipped bit in the contents.
  std::string damaged = prefix + WithTrailer(contents, 0);
  damaged[prefix.size() + 4] = static_cast<char>(damaged[prefix.size() + 4] ^ 0x04);
  Store(damaged);
  read = "stale";
  EXPECT_EQ(ReadBlock(file_.get(), handle, true, &read).ToString(),
            "Corruption: block checksum mismatch");
  EXPECT_TRUE(read.empty()) << "a failed read leaves no contents behind to be mistaken for good";
  // Without verification the bytes come back as they are.
  ASSERT_OK(ReadBlock(file_.get(), handle, false, &read));
  EXPECT_EQ(read, damaged.substr(prefix.size(), contents.size()));

  // The stored checksum itself damaged.
  std::string bad_crc = prefix + WithTrailer(contents, 0);
  bad_crc.back() = static_cast<char>(bad_crc.back() ^ 0x80);
  Store(bad_crc);
  EXPECT_EQ(ReadBlock(file_.get(), handle, true, &read).ToString(),
            "Corruption: block checksum mismatch");

  // A type byte this reader does not know, with a checksum that matches it: the block is intact
  // and written by something that compresses, which is not an error to be skipped. The type is
  // checked whether or not the checksum is.
  Store(prefix + WithTrailer(contents, 1));
  EXPECT_EQ(ReadBlock(file_.get(), handle, true, &read).ToString(),
            "Corruption: unknown block type");
  EXPECT_EQ(ReadBlock(file_.get(), handle, false, &read).ToString(),
            "Corruption: unknown block type");

  // The type byte damaged with the checksum left alone is a checksum failure: the checksum covers
  // the type.
  std::string flipped_type = prefix + WithTrailer(contents, 0);
  flipped_type[prefix.size() + contents.size()] = 1;
  Store(flipped_type);
  EXPECT_EQ(ReadBlock(file_.get(), handle, true, &read).ToString(),
            "Corruption: block checksum mismatch");
}

TEST_F(ReadBlockTest, EmptyBlockIsReadable) {
  Store(WithTrailer("", 0));
  std::string read = "stale";
  ASSERT_OK(ReadBlock(file_.get(), BlockHandle{0, 0}, true, &read));
  EXPECT_TRUE(read.empty());
}

TEST_F(ReadBlockTest, HandleBeyondTheEndOfTheFileIsATruncatedRead) {
  const std::string contents = "twelve bytes";
  Store(WithTrailer(contents, 0));
  std::string read;
  // One byte more than the file has.
  EXPECT_EQ(ReadBlock(file_.get(), BlockHandle{0, contents.size() + 1}, true, &read).ToString(),
            "Corruption: truncated block read");
  // The trailer cut short.
  EXPECT_EQ(ReadBlock(file_.get(), BlockHandle{1, contents.size()}, true, &read).ToString(),
            "Corruption: truncated block read");
  // At and past the end.
  EXPECT_EQ(ReadBlock(file_.get(), BlockHandle{contents.size() + 5, 0}, true, &read).ToString(),
            "Corruption: truncated block read");
  EXPECT_EQ(ReadBlock(file_.get(), BlockHandle{uint64_t{1} << 40, 8}, true, &read).ToString(),
            "Corruption: truncated block read");
}

TEST_F(ReadBlockTest, HostileHandleSizesNeitherAllocateNorOverflow) {
  // A handle comes out of a file, so its size is whatever an attacker wrote. A read that sized its
  // buffer from it would try to allocate terabytes before it found out the file is a few bytes.
  Store(WithTrailer("small", 0));
  std::string read;
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  for (const BlockHandle& handle :
       {BlockHandle{0, uint64_t{1} << 62}, BlockHandle{0, kMax}, BlockHandle{0, kMax - 4},
        BlockHandle{0, kMax - 5}, BlockHandle{kMax, 0}, BlockHandle{kMax, kMax},
        BlockHandle{kMax - 3, 2}, BlockHandle{uint64_t{1} << 63, 1}}) {
    EXPECT_EQ(ReadBlock(file_.get(), handle, true, &read).ToString(),
              "Corruption: truncated block read")
        << "offset " << handle.offset << " size " << handle.size;
    EXPECT_TRUE(read.empty());
  }
}

TEST_F(ReadBlockTest, BlocksLargerThanOneReadChunkAreReadWhole) {
  // Large enough to take several reads of the chunk size the implementation uses, and not a
  // multiple of any size it could pick.
  std::string contents(3 * 1024 * 1024 + 777, 'x');
  for (size_t i = 0; i < contents.size(); i += 4099) {
    contents[i] = static_cast<char>('a' + (i / 4099) % 26);
  }
  Store("pad" + WithTrailer(contents, 0));
  std::string read;
  ASSERT_OK(ReadBlock(file_.get(), BlockHandle{3, contents.size()}, true, &read));
  EXPECT_EQ(read, contents);
  // And one byte short, in the last chunk.
  EXPECT_EQ(ReadBlock(file_.get(), BlockHandle{3, contents.size() + 1}, true, &read).ToString(),
            "Corruption: truncated block read");
}

}  // namespace
}  // namespace shale
