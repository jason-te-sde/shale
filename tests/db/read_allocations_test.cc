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
// The bounds of libstdc++ are the counts of GCC 13, GCC 14 and Clang 17 and 18, in Debug and
// Release, which agree. v0.1.1 was not measured with libstdc++.

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

#if defined(_LIBCPP_VERSION)
constexpr uint64_t kGetCached = 1;
constexpr uint64_t kGetRead = 2;
constexpr uint64_t kBlockRead = 1;
constexpr uint64_t kGetReadAndCache = 5;
constexpr uint64_t kSeekCached = 8;
constexpr uint64_t kSeekRead = 10;
#else
constexpr uint64_t kGetCached = 1;
constexpr uint64_t kGetRead = 2;
constexpr uint64_t kBlockRead = 1;
constexpr uint64_t kGetReadAndCache = 5;
constexpr uint64_t kSeekCached = 9;
constexpr uint64_t kSeekRead = 11;
#endif

constexpr int kKeys = 2000;
constexpr int kLevel0First = 5000;
constexpr int kLevel0Count = 500;

class ReadAllocationsTest : public ::testing::Test {
 protected:
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
      ASSERT_OK(db_->Put(WriteOptions(), UserKey(i), value));
    }
    ASSERT_OK(db_->Flush());
    ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
    for (int i = kLevel0First; i < kLevel0First + kLevel0Count; i++) {
      ASSERT_OK(db_->Put(WriteOptions(), UserKey(i), value));
    }
    ASSERT_OK(db_->Flush());
  }

  void TearDown() override { db_.reset(); }

  // Looks `key` up, and returns the allocations that took.
  uint64_t CountGet(const ReadOptions& options, int key) {
    std::string value;
    Status s;
    uint64_t count = 0;
    {
      const AllocationCounter counter;
      s = db_->Get(options, UserKey(key), &value);
      count = counter.count();
    }
    EXPECT_OK(s);
    EXPECT_EQ(value.size(), 100u);
    return count;
  }

  // Makes an iterator, seeks to `key`, reads the entry, and destroys the iterator: the allocations
  // of all of it.
  uint64_t CountSeek(const ReadOptions& options, int key) {
    uint64_t count = 0;
    bool valid = false;
    {
      const AllocationCounter counter;
      {
        std::unique_ptr<Iterator> it = db_->NewIterator(options);
        it->Seek(UserKey(key));
        valid = it->Valid();
        if (valid) {
          const std::string_view k = it->key();
          const std::string_view v = it->value();
          valid = k == UserKey(key) && v.size() == 100;
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

TEST_F(ReadAllocationsTest, GetThatFindsItsBlockInTheCache) {
  const ReadOptions options;
  CountGet(options, 1000);  // opens the table and caches the block
  const uint64_t count = CountGet(options, 1000);
  Print("kGetCached", count);
  EXPECT_LE(count, kGetCached);
}

TEST_F(ReadAllocationsTest, GetThatReadsItsBlock) {
  ReadOptions options;
  options.fill_cache = false;
  CountGet(options, 100);  // opens the table
  const uint64_t count = CountGet(options, 1000);
  Print("kGetRead", count);
  EXPECT_LE(count, kGetRead);
}

TEST_F(ReadAllocationsTest, BlockReadIsTheDifference) {
  const ReadOptions cached;
  ReadOptions uncached;
  uncached.fill_cache = false;
  CountGet(cached, 1000);  // opens the table and caches the block
  const uint64_t hit = CountGet(cached, 1000);
  const uint64_t read = CountGet(uncached, 1500);
  ASSERT_GE(read, hit);
  Print("kBlockRead", read - hit);
  EXPECT_LE(read - hit, kBlockRead);
}

TEST_F(ReadAllocationsTest, GetThatReadsAndCachesItsBlock) {
  const ReadOptions options;
  CountGet(options, 100);  // opens the table
  const uint64_t count = CountGet(options, 1000);
  Print("kGetReadAndCache", count);
  EXPECT_LE(count, kGetReadAndCache);
}

TEST_F(ReadAllocationsTest, NewIteratorAndSeekWithBlocksCached) {
  const ReadOptions options;
  CountSeek(options, 1000);  // opens the tables and caches their blocks
  const uint64_t count = CountSeek(options, 1000);
  Print("kSeekCached", count);
  EXPECT_LE(count, kSeekCached);
}

TEST_F(ReadAllocationsTest, NewIteratorAndSeekThatReadsBlocks) {
  ReadOptions options;
  options.fill_cache = false;
  CountSeek(options, 100);  // opens the tables
  const uint64_t count = CountSeek(options, 1000);
  Print("kSeekRead", count);
  EXPECT_LE(count, kSeekRead);
}

TEST_F(ReadAllocationsTest, NextWithinABlockAllocatesNothing) {
  const ReadOptions options;
  std::unique_ptr<Iterator> it = db_->NewIterator(options);
  it->Seek(UserKey(1000));
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
  EXPECT_EQ(it->key(), UserKey(1011));
  Print("next", count);
  EXPECT_EQ(count, 0u);
}

}  // namespace

}  // namespace shale
