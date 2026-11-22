// How many times the read path calls operator new. A read that is cheap in time can still be dear
// in allocations, and a count is exact where a clock is not, so these tests hold each figure to a
// bound that is the count itself: a change that makes a read allocate more fails here, and one that
// makes it allocate less lowers the constant.
//
// The file uses the public API only, so that the same file measures a release that predates the
// internals it would otherwise name. The counts are of this thread: the database runs on a
// ManualScheduler and nothing here starts another.
//
// The counts depend on the standard library, since the sizes of the small-string buffer and of
// std::function's buffer differ, and CI builds both. Each figure has one constant for each. With
// libc++, on the machine that measured them, the figures were:
//
//                        v0.1.1   one table iterator and readahead   this change
//   kGetCached                3                 3                          1
//   kGetRead                  5                 5                          2
//   kBlockRead                2                 2                          1
//   kGetReadAndCache          8                 8                          5
//   kSeekCached              27                13                          8
//   kSeekRead                31                17                         10
//
// The 16-byte keys of the second run make 24-byte internal keys, which no small-string buffer
// holds. With libc++, in the same three columns (v0.1.1, 2f95cb6, this change):
//
//                        v0.1.1   one table iterator and readahead   this change
//   kGetCached16              6                 8                          1
//   kGetRead16                8                10                          2
//   kBlockRead16              2                 2                          1
//   kGetReadAndCache16       11                13                          5
//   kSeekCached16            39                21                          9
//   kSeekRead16              43                25                         11
//
// The bounds of libstdc++ are the counts of GCC 13, GCC 14 and Clang 17 and 18, in Debug and
// Release, and the sanitizer build, which agree. Neither v0.1.1 nor 2f95cb6 was measured with
// libstdc++.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "shale/db.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"
#include "testkit/alloc_counter.h"
#include "testkit/test_util.h"

namespace shale {

namespace {

// The bounds of one run of the figures.
struct Bounds {
  uint64_t get_cached;
  uint64_t get_read;
  uint64_t block_read;
  uint64_t get_read_and_cache;
  uint64_t seek_cached;
  uint64_t seek_read;
};

// Keys of 7 bytes, which fit the small-string buffer of both standard libraries with their tag.
#if defined(_LIBCPP_VERSION)
constexpr Bounds kShortKeys = {
    /*kGetCached=*/1,       /*kGetRead=*/2,    /*kBlockRead=*/1,
    /*kGetReadAndCache=*/5, /*kSeekCached=*/8, /*kSeekRead=*/10};
#else
constexpr Bounds kShortKeys = {1, 2, 1, 5, 9, 11};
#endif

// Keys of 16 bytes, as the benchmark's are: 24 bytes with their tag, which no small-string buffer
// holds, so every copy of a key that a std::string makes is an allocation.
#if defined(_LIBCPP_VERSION)
constexpr Bounds kLongKeys = {1, 2, 1, 5, 9, 11};
#else
constexpr Bounds kLongKeys = {1, 2, 1, 5, 9, 11};
#endif

constexpr int kKeys = 2000;
constexpr int kLevel0First = 5000;
constexpr int kLevel0Count = 500;

// The user key of number `i`, padded to `size` bytes: keys of every size sort as the numbers do.
std::string KeyOf(size_t size, int i) {
  std::string key = UserKey(i);
  key.resize(size, 'x');
  return key;
}

// Run once for each size of key.
class ReadAllocationsTest : public ::testing::TestWithParam<size_t> {
 protected:
  size_t key_size() const { return GetParam(); }
  std::string KeyOf(int i) const { return shale::KeyOf(key_size(), i); }
  const Bounds& bounds() const { return key_size() == 7 ? kShortKeys : kLongKeys; }
  // The names of the figures: those of 7-byte keys are bare, the others carry the size.
  std::string Name(const char* figure) const {
    return key_size() == 7 ? figure : std::string(figure) + std::to_string(key_size());
  }

  // A database of 2000 keys of 100-byte values in level 1, and a level-0 table whose range is
  // above every key the tests look up.
  void SetUp() override {
    env_ = NewMemEnv();
    Options options;
    options.env = env_.get();
    options.scheduler = &scheduler_;
    options.create_if_missing = true;
    ASSERT_OK(DB::Open(options, "/db", &db_));
    const std::string value(100, 'v');
    for (int i = 0; i < kKeys; i++) {
      ASSERT_OK(db_->Put(WriteOptions(), KeyOf(i), value));
    }
    ASSERT_OK(db_->Flush());
    ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
    for (int i = kLevel0First; i < kLevel0First + kLevel0Count; i++) {
      ASSERT_OK(db_->Put(WriteOptions(), KeyOf(i), value));
    }
    ASSERT_OK(db_->Flush());
  }

  void TearDown() override { db_.reset(); }

  // Looks `key` up, and returns the allocations that took.
  uint64_t CountGet(const ReadOptions& options, int key) {
    const std::string user_key = KeyOf(key);
    std::string value;
    Status s;
    uint64_t count = 0;
    {
      const AllocationCounter counter;
      s = db_->Get(options, user_key, &value);
      count = counter.count();
    }
    EXPECT_OK(s);
    EXPECT_EQ(value.size(), 100u);
    return count;
  }

  // Makes an iterator, seeks to `key`, reads the entry, and destroys the iterator: the allocations
  // of all of it.
  uint64_t CountSeek(const ReadOptions& options, int key) {
    const std::string user_key = KeyOf(key);
    uint64_t count = 0;
    bool valid = false;
    {
      const AllocationCounter counter;
      {
        std::unique_ptr<Iterator> it = db_->NewIterator(options);
        it->Seek(user_key);
        valid = it->Valid();
        if (valid) {
          const std::string_view k = it->key();
          const std::string_view v = it->value();
          valid = k == user_key && v.size() == 100;
        }
      }
      count = counter.count();
    }
    EXPECT_TRUE(valid);
    return count;
  }

  static void Print(const char* name, uint64_t count) {
    std::printf("read-allocations: %s=%llu\n", name, static_cast<unsigned long long>(count));
  }

  std::unique_ptr<Env> env_;
  ManualScheduler scheduler_;
  std::unique_ptr<DB> db_;
};

TEST_P(ReadAllocationsTest, GetThatFindsItsBlockInTheCache) {
  const ReadOptions options;
  CountGet(options, 1000);  // opens the table and caches the block
  const uint64_t count = CountGet(options, 1000);
  Print(Name("kGetCached").c_str(), count);
  EXPECT_LE(count, bounds().get_cached);
}

TEST_P(ReadAllocationsTest, GetThatReadsItsBlock) {
  ReadOptions options;
  options.fill_cache = false;
  CountGet(options, 100);  // opens the table
  const uint64_t count = CountGet(options, 1000);
  Print(Name("kGetRead").c_str(), count);
  EXPECT_LE(count, bounds().get_read);
}

TEST_P(ReadAllocationsTest, BlockReadIsTheDifference) {
  const ReadOptions cached;
  ReadOptions uncached;
  uncached.fill_cache = false;
  CountGet(cached, 1000);  // opens the table and caches the block
  const uint64_t hit = CountGet(cached, 1000);
  const uint64_t read = CountGet(uncached, 1500);
  ASSERT_GE(read, hit);
  Print(Name("kBlockRead").c_str(), read - hit);
  EXPECT_LE(read - hit, bounds().block_read);
}

TEST_P(ReadAllocationsTest, GetThatReadsAndCachesItsBlock) {
  const ReadOptions options;
  CountGet(options, 100);  // opens the table
  const uint64_t count = CountGet(options, 1000);
  Print(Name("kGetReadAndCache").c_str(), count);
  EXPECT_LE(count, bounds().get_read_and_cache);
}

TEST_P(ReadAllocationsTest, NewIteratorAndSeekWithBlocksCached) {
  const ReadOptions options;
  CountSeek(options, 1000);  // opens the tables and caches their blocks
  const uint64_t count = CountSeek(options, 1000);
  Print(Name("kSeekCached").c_str(), count);
  EXPECT_LE(count, bounds().seek_cached);
}

TEST_P(ReadAllocationsTest, NewIteratorAndSeekThatReadsBlocks) {
  ReadOptions options;
  options.fill_cache = false;
  CountSeek(options, 100);  // opens the tables
  const uint64_t count = CountSeek(options, 1000);
  Print(Name("kSeekRead").c_str(), count);
  EXPECT_LE(count, bounds().seek_read);
}

TEST_P(ReadAllocationsTest, NextWithinABlockAllocatesNothing) {
  const ReadOptions options;
  std::unique_ptr<Iterator> it = db_->NewIterator(options);
  it->Seek(KeyOf(1000));
  ASSERT_TRUE(it->Valid());
  it->Next();  // the first step after a seek
  ASSERT_TRUE(it->Valid());
  uint64_t count = 0;
  {
    const AllocationCounter counter;
    for (int i = 0; i < 10; i++) {
      it->Next();
    }
    count = counter.count();
  }
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), KeyOf(1011));
  Print(Name("next").c_str(), count);
  EXPECT_EQ(count, 0u);
}

INSTANTIATE_TEST_SUITE_P(KeySizes, ReadAllocationsTest, ::testing::Values(size_t{7}, size_t{16}),
                         [](const ::testing::TestParamInfo<size_t>& param) {
                           return "Keys" + std::to_string(param.param);
                         });

}  // namespace

}  // namespace shale
