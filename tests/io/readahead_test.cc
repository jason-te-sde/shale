// The readahead buffer of a table iterator: when it starts, how much it reads, and what it holds.

#include "io/readahead.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/table_format.h"
#include "shale/env.h"
#include "shale/status.h"
#include "testkit/test_util.h"

namespace shale {
namespace {

// A file over a string that records every read it is asked for.
class RecordingFile final : public RandomAccessFile {
 public:
  explicit RecordingFile(std::string bytes) : bytes_(std::move(bytes)) {}

  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    reads_.emplace_back(offset, n);
    if (!error_.ok()) {
      return error_;
    }
    const size_t from = static_cast<size_t>(std::min<uint64_t>(offset, bytes_.size()));
    const size_t len = std::min(n, bytes_.size() - from);
    std::memcpy(scratch, bytes_.data() + from, len);
    *result = std::string_view(scratch, len);
    return Status::OK();
  }

  void FailWith(Status s) { error_ = std::move(s); }
  const std::vector<std::pair<uint64_t, size_t>>& reads() const { return reads_; }

 private:
  const std::string bytes_;
  Status error_;
  mutable std::vector<std::pair<uint64_t, size_t>> reads_;
};

constexpr uint64_t kMiB = uint64_t{1} << 20;

void Step(Readahead* readahead, int times) {
  for (int i = 0; i < times; ++i) {
    readahead->NoteStep();
  }
}

TEST(ReadaheadTest, NoFillUntilTwoStepsAfterAReset) {
  Readahead readahead;
  const BlockHandle handle{0, 4096};
  EXPECT_FALSE(readahead.ShouldFill(handle));
  readahead.Reset();
  EXPECT_FALSE(readahead.ShouldFill(handle));
  readahead.NoteStep();
  EXPECT_FALSE(readahead.ShouldFill(handle));
  readahead.NoteStep();
  EXPECT_TRUE(readahead.ShouldFill(handle));
  readahead.Reset();
  EXPECT_FALSE(readahead.ShouldFill(handle));
}

TEST(ReadaheadTest, FillReadsTheBlockAndTheInitialBytesAfterIt) {
  RecordingFile file(std::string(kMiB, 'x'));
  Readahead readahead;
  ASSERT_OK(readahead.Fill(file, {0, 4096}, kMiB));
  ASSERT_EQ(file.reads().size(), 1u);
  EXPECT_EQ(file.reads()[0], std::make_pair(uint64_t{0}, size_t{4101 + 8192}));
  std::string_view stored;
  EXPECT_TRUE(readahead.Find({0, 4096}, &stored));
  EXPECT_EQ(stored.size(), 4101u);
  // The block after it, 4101 bytes on, is whole in the 8192 bytes read ahead.
  EXPECT_TRUE(readahead.Find({4101, 4096}, &stored));
  EXPECT_EQ(stored.size(), 4101u);
  // The third does not fit.
  EXPECT_FALSE(readahead.Find({2 * 4101, 4096}, &stored));
}

TEST(ReadaheadTest, EachFillDoublesUpToTheCap) {
  RecordingFile file(std::string(8 * kMiB, 'x'));
  Readahead readahead;
  const BlockHandle handle{0, 4096};
  for (int i = 0; i < 7; ++i) {
    ASSERT_OK(readahead.Fill(file, handle, 8 * kMiB));
  }
  const std::vector<size_t> past = {8, 16, 32, 64, 128, 256, 256};
  ASSERT_EQ(file.reads().size(), past.size());
  for (size_t i = 0; i < past.size(); ++i) {
    EXPECT_EQ(file.reads()[i].second, 4101 + (past[i] << 10)) << "fill " << i;
  }
}

TEST(ReadaheadTest, FillStopsAtTheLimit) {
  RecordingFile file(std::string(kMiB, 'x'));
  Readahead readahead;
  ASSERT_OK(readahead.Fill(file, {1000, 4096}, 1000 + 4101 + 100));
  ASSERT_EQ(file.reads().size(), 1u);
  EXPECT_EQ(file.reads()[0], std::make_pair(uint64_t{1000}, size_t{4101 + 100}));
}

TEST(ReadaheadTest, ResetStartsAgainFromTheInitialBytes) {
  RecordingFile file(std::string(kMiB, 'x'));
  Readahead readahead;
  ASSERT_OK(readahead.Fill(file, {0, 4096}, kMiB));
  ASSERT_OK(readahead.Fill(file, {0, 4096}, kMiB));
  readahead.Reset();
  ASSERT_OK(readahead.Fill(file, {0, 4096}, kMiB));
  ASSERT_EQ(file.reads().size(), 3u);
  EXPECT_EQ(file.reads()[1].second, 4101u + (size_t{16} << 10));
  EXPECT_EQ(file.reads()[2].second, 4101u + (size_t{8} << 10));
}

TEST(ReadaheadTest, ResetKeepsTheBufferedBytesUsable) {
  RecordingFile file(std::string(kMiB, 'x'));
  Readahead readahead;
  ASSERT_OK(readahead.Fill(file, {0, 4096}, kMiB));
  readahead.Reset();
  std::string_view stored;
  EXPECT_TRUE(readahead.Find({0, 4096}, &stored));
}

TEST(ReadaheadTest, BlocksLargerThanTheCapAreNotFilled) {
  Readahead readahead;
  Step(&readahead, 5);
  EXPECT_TRUE(readahead.ShouldFill({0, Readahead::kMaxBytes - kBlockTrailerSize}));
  EXPECT_FALSE(readahead.ShouldFill({0, Readahead::kMaxBytes}));
  EXPECT_FALSE(readahead.ShouldFill({0, uint64_t{1} << 40}));
}

TEST(ReadaheadTest, ShortFillHoldsOnlyWhatCameBack) {
  RecordingFile file(std::string(5000, 'x'));
  Readahead readahead;
  ASSERT_OK(readahead.Fill(file, {0, 4096}, kMiB));
  std::string_view stored;
  EXPECT_TRUE(readahead.Find({0, 4096}, &stored));
  EXPECT_FALSE(readahead.Find({4101, 4096}, &stored));
  EXPECT_FALSE(readahead.Find({4000, 1000}, &stored));
}

TEST(ReadaheadTest, FindNeedsTheWholeBlock) {
  RecordingFile file(std::string(kMiB, 'x'));
  Readahead readahead;
  ASSERT_OK(readahead.Fill(file, {100, 50}, kMiB));  // 55 bytes of block and 8192 after
  std::string_view stored;
  EXPECT_TRUE(readahead.Find({100, 50}, &stored));
  // The buffer is 55 + 8192 bytes from offset 100, so a block at 105 may be 8242 bytes in all.
  EXPECT_TRUE(readahead.Find({105, 8242 - 5}, &stored));
  EXPECT_FALSE(readahead.Find({105, 8242 - 5 + 1}, &stored));  // one byte too long
  EXPECT_FALSE(readahead.Find({99, 50}, &stored));             // starts before the buffer
  EXPECT_FALSE(readahead.Find({100, uint64_t{1} << 63}, &stored));
  EXPECT_FALSE(readahead.Find({~uint64_t{0}, 10}, &stored));
}

TEST(ReadaheadTest, FailedFillReturnsTheErrorAndHoldsNothing) {
  RecordingFile file(std::string(kMiB, 'x'));
  Readahead readahead;
  ASSERT_OK(readahead.Fill(file, {0, 4096}, kMiB));
  file.FailWith(Status::IOError("disk on fire"));
  const Status s = readahead.Fill(file, {0, 4096}, kMiB);
  EXPECT_TRUE(s.IsIOError());
  std::string_view stored;
  EXPECT_FALSE(readahead.Find({0, 4096}, &stored));
}

TEST(ReadaheadTest, DiscardForgetsTheBuffer) {
  RecordingFile file(std::string(kMiB, 'x'));
  Readahead readahead;
  ASSERT_OK(readahead.Fill(file, {0, 4096}, kMiB));
  readahead.Discard();
  std::string_view stored;
  EXPECT_FALSE(readahead.Find({0, 4096}, &stored));
}

TEST(ReadaheadTest, CapacityNeverExceedsTheCapPlusOneBlock) {
  RecordingFile file(std::string(16 * kMiB, 'x'));
  Readahead readahead;
  EXPECT_EQ(readahead.capacity(), 0u);
  uint64_t offset = 0;
  for (int i = 0; i < 50; ++i) {
    const uint64_t size = 100 + static_cast<uint64_t>(i) * 160;  // up to 8 KiB
    ASSERT_OK(readahead.Fill(file, {offset, std::min<uint64_t>(size, 8192)}, 16 * kMiB));
    EXPECT_LE(readahead.capacity(), Readahead::kMaxBytes + 8192 + kBlockTrailerSize);
    offset += size;
  }
}

}  // namespace
}  // namespace shale
