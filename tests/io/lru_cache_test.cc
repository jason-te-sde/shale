// The sharded LRU cache under the block and table caches. Most tests use one shard, because the
// order of eviction is only defined within a shard. The concurrency test is the one that matters
// for the locking: it runs under ThreadSanitizer, and checks that however the threads interleave,
// a key is only ever seen with the value that was stored under it.

#include "io/lru_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "util/random.h"

namespace shale {
namespace {

using IntCache = ShardedLRUCache<int, int>;

std::shared_ptr<int> Boxed(int v) { return std::make_shared<int>(v); }

// A cache with one shard, so that the least recently used entry is the one that goes first.
IntCache SingleShard(size_t capacity) { return IntCache(capacity, 0); }

TEST(LRUCacheTest, HitAndMiss) {
  IntCache cache = SingleShard(10);
  EXPECT_EQ(cache.Lookup(1), nullptr);

  const std::shared_ptr<int> value = Boxed(100);
  EXPECT_EQ(cache.Insert(1, value, 1), value) << "Insert returns the value it was given";
  const std::shared_ptr<int> hit = cache.Lookup(1);
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(hit, value) << "and a lookup returns that same object, not a copy";
  EXPECT_EQ(*hit, 100);
  EXPECT_EQ(cache.Lookup(2), nullptr);
}

TEST(LRUCacheTest, EvictsLeastRecentlyUsed) {
  IntCache cache = SingleShard(3);
  cache.Insert(1, Boxed(1), 1);  // a
  cache.Insert(2, Boxed(2), 1);  // b
  cache.Insert(3, Boxed(3), 1);  // c
  ASSERT_NE(cache.Lookup(1), nullptr) << "a is now the most recently used";
  cache.Insert(4, Boxed(4), 1);  // d pushes out the oldest, which is b

  EXPECT_EQ(cache.Lookup(2), nullptr);
  EXPECT_NE(cache.Lookup(1), nullptr);
  EXPECT_NE(cache.Lookup(3), nullptr);
  EXPECT_NE(cache.Lookup(4), nullptr);
  EXPECT_EQ(cache.TotalCharge(), 3U);
}

TEST(LRUCacheTest, ChargeBasedEviction) {
  IntCache cache = SingleShard(10);
  cache.Insert(1, Boxed(1), 6);
  EXPECT_EQ(cache.TotalCharge(), 6U);
  cache.Insert(2, Boxed(2), 6);

  EXPECT_EQ(cache.Lookup(1), nullptr) << "6 + 6 does not fit in 10, so the older one goes";
  EXPECT_NE(cache.Lookup(2), nullptr);
  EXPECT_EQ(cache.TotalCharge(), 6U);

  // One large entry can push out several small ones.
  cache.Insert(3, Boxed(3), 2);
  cache.Insert(4, Boxed(4), 2);
  EXPECT_EQ(cache.TotalCharge(), 10U);
  cache.Insert(5, Boxed(5), 9);
  EXPECT_EQ(cache.TotalCharge(), 9U);
  EXPECT_NE(cache.Lookup(5), nullptr);
  EXPECT_EQ(cache.Lookup(2), nullptr);
  EXPECT_EQ(cache.Lookup(3), nullptr);
  EXPECT_EQ(cache.Lookup(4), nullptr);
}

TEST(LRUCacheTest, InsertReplaces) {
  IntCache cache = SingleShard(10);
  std::shared_ptr<int> first = Boxed(1);
  const std::weak_ptr<int> first_watcher = first;
  cache.Insert(7, std::move(first), 3);
  cache.Insert(7, Boxed(2), 4);

  const std::shared_ptr<int> hit = cache.Lookup(7);
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(*hit, 2) << "the second value replaces the first";
  EXPECT_EQ(cache.TotalCharge(), 4U) << "and its charge is counted once, not added to the old one";
  EXPECT_TRUE(first_watcher.expired()) << "the cache let go of the one it replaced";
}

TEST(LRUCacheTest, Erase) {
  IntCache cache = SingleShard(10);
  cache.Insert(1, Boxed(1), 3);
  cache.Insert(2, Boxed(2), 4);

  cache.Erase(1);
  EXPECT_EQ(cache.Lookup(1), nullptr);
  EXPECT_NE(cache.Lookup(2), nullptr);
  EXPECT_EQ(cache.TotalCharge(), 4U);

  cache.Erase(1);  // nothing to erase, and nothing happens
  cache.Erase(99);
  EXPECT_EQ(cache.TotalCharge(), 4U);
}

TEST(LRUCacheTest, ZeroCapacityRetainsNothing) {
  IntCache cache = SingleShard(0);
  const std::shared_ptr<int> value = Boxed(5);
  EXPECT_EQ(cache.Insert(1, value, 1), value) << "the caller still gets its value back";
  EXPECT_EQ(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.TotalCharge(), 0U);

  IntCache sharded(0);
  sharded.Insert(1, Boxed(5), 1);
  EXPECT_EQ(sharded.Lookup(1), nullptr);
  EXPECT_EQ(sharded.TotalCharge(), 0U);

  cache.Insert(2, Boxed(6), 0);
  EXPECT_EQ(cache.Lookup(2), nullptr) << "nor does an entry that says it costs nothing";
  EXPECT_EQ(cache.TotalCharge(), 0U);
}

TEST(LRUCacheTest, ZeroChargeCountsAsOne) {
  // A block of no bytes, from a damaged table, is still a block that takes an entry. If it cost
  // nothing, entries like it would never be evicted by the charges adding up.
  IntCache cache = SingleShard(2);
  cache.Insert(1, Boxed(1), 0);
  EXPECT_NE(cache.Lookup(1), nullptr) << "it is cached";
  EXPECT_EQ(cache.TotalCharge(), 1U) << "and counted as 1";

  cache.Insert(2, Boxed(2), 0);
  EXPECT_EQ(cache.TotalCharge(), 2U);
  cache.Insert(3, Boxed(3), 0);
  EXPECT_EQ(cache.TotalCharge(), 2U) << "so the third pushes the least recently used one out";
  EXPECT_EQ(cache.Lookup(1), nullptr);
  EXPECT_NE(cache.Lookup(2), nullptr);
  EXPECT_NE(cache.Lookup(3), nullptr);
}

TEST(LRUCacheTest, EvictedValueStaysAliveWhileHeld) {
  IntCache cache = SingleShard(1);
  const std::shared_ptr<int> held = cache.Insert(1, Boxed(11), 1);
  std::shared_ptr<int> unheld = Boxed(22);
  const std::weak_ptr<int> unheld_watcher = unheld;
  cache.Insert(2, std::move(unheld), 1);  // evicts 1
  cache.Insert(3, Boxed(33), 1);          // evicts 2

  EXPECT_EQ(cache.Lookup(1), nullptr);
  EXPECT_EQ(*held, 11) << "a reader that still holds an evicted value can keep using it";
  EXPECT_TRUE(unheld_watcher.expired()) << "and nothing keeps it alive once nobody holds it";
}

TEST(LRUCacheTest, OversizedEntryIsNotRetainedAndEvictsNothing) {
  IntCache cache = SingleShard(10);
  cache.Insert(1, Boxed(1), 4);
  cache.Insert(2, Boxed(2), 4);

  // An entry bigger than the shard could never be kept. Making room for it would empty the shard
  // for nothing, so the others stay.
  const std::shared_ptr<int> big = Boxed(3);
  EXPECT_EQ(cache.Insert(3, big, 11), big);
  EXPECT_EQ(cache.Lookup(3), nullptr);
  EXPECT_NE(cache.Lookup(1), nullptr);
  EXPECT_NE(cache.Lookup(2), nullptr);
  EXPECT_EQ(cache.TotalCharge(), 8U);

  // But it does replace what was stored under its key: the old value must not survive an Insert
  // that said the key now has a new one.
  cache.Insert(1, Boxed(100), 11);
  EXPECT_EQ(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.TotalCharge(), 4U);
}

TEST(LRUCacheTest, EvictedValueIsDestroyedOutsideTheLock) {
  // A value whose destructor uses the cache, as a cached table that closes a file might. If the
  // cache destroyed it while holding its lock, the lookup would find the lock already held.
  struct Reentrant;
  using ReentrantCache = ShardedLRUCache<int, Reentrant>;
  struct Reentrant {
    explicit Reentrant(ReentrantCache* c) : cache(c) {}
    ~Reentrant() { cache->Lookup(0); }
    ReentrantCache* cache;
  };

  ReentrantCache cache(1, 0);
  cache.Insert(1, std::make_shared<Reentrant>(&cache), 1);
  cache.Insert(2, std::make_shared<Reentrant>(&cache), 1);  // evicts 1
  cache.Insert(2, std::make_shared<Reentrant>(&cache), 1);  // replaces 2
  cache.Erase(2);
  EXPECT_EQ(cache.TotalCharge(), 0U);
}

TEST(LRUCacheTest, CapacityIsSplitEvenlyAcrossShards) {
  // 10 over 4 shards is 3 each, rounded up, so the cache holds 12 once every shard is full.
  ShardedLRUCache<int, int> cache(10, 2);
  for (int i = 0; i < 1000; ++i) {
    cache.Insert(i, Boxed(i), 1);
  }
  EXPECT_EQ(cache.TotalCharge(), 12U);

  ShardedLRUCache<int, int> exact(100, 2);
  for (int i = 0; i < 1000; ++i) {
    exact.Insert(i, Boxed(i), 1);
  }
  EXPECT_EQ(exact.TotalCharge(), 100U);
}

TEST(LRUCacheTest, KeysThatAreMultiplesOfABlockSizeSpreadAcrossShards) {
  // A block cache is keyed by file offsets, which are multiples of the block size. The standard
  // hash of an integer is the integer itself, so with 16 shards a shard taken from its low bits
  // would be 0 for all of them, and a cache of 160 would hold 10. The hash is mixed first.
  ShardedLRUCache<int, int> cache(160, 4);
  for (int i = 0; i < 160; ++i) {
    cache.Insert(i * 4096, Boxed(i), 1);
  }
  EXPECT_GE(cache.TotalCharge(), 110U) << "16 shards of 10, 160 keys";
}

TEST(LRUCacheTest, StringKeys) {
  ShardedLRUCache<std::string, std::string> cache(100, 2);
  cache.Insert("alpha", std::make_shared<std::string>("a"), 1);
  cache.Insert("beta", std::make_shared<std::string>("b"), 1);
  ASSERT_NE(cache.Lookup("alpha"), nullptr);
  EXPECT_EQ(*cache.Lookup("alpha"), "a");
  EXPECT_EQ(*cache.Lookup("beta"), "b");
  EXPECT_EQ(cache.Lookup("gamma"), nullptr);
}

TEST(LRUCacheDeathTest, MisuseAborts) {
  IntCache cache = SingleShard(10);
  EXPECT_DEATH(cache.Insert(1, nullptr, 1), "value");
  EXPECT_DEATH(IntCache(10, -1), "num_shard_bits");
  EXPECT_DEATH(IntCache(10, 17), "num_shard_bits");
}

TEST(LRUCacheTest, ConcurrentUseIsConsistent) {
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 10000;
  constexpr uint32_t kKeys = 1000;
  // Fewer than the keys, so that inserts evict while other threads look up.
  ShardedLRUCache<int, int> cache(300, 4);

  std::atomic<int> wrong_values{0};
  std::atomic<int> hits{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&cache, &wrong_values, &hits, t] {
      Random rnd(1000 + static_cast<uint64_t>(t));
      for (int i = 0; i < kOpsPerThread; ++i) {
        const int key = static_cast<int>(rnd.Uniform(kKeys));
        switch (rnd.Uniform(4)) {
          case 0:
            cache.Insert(key, Boxed(key * 7), 1);
            break;
          case 1:
            cache.Erase(key);
            break;
          default: {
            const std::shared_ptr<int> value = cache.Lookup(key);
            if (value != nullptr) {
              hits.fetch_add(1);
              if (*value != key * 7) {
                wrong_values.fetch_add(1);
              }
            }
            break;
          }
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(wrong_values.load(), 0) << "a key was seen with a value stored under another";
  EXPECT_GT(hits.load(), 0) << "the test never hit the cache, so it checked nothing";
  EXPECT_LE(cache.TotalCharge(), 304U) << "16 shards of 19, rounded up from 300";
}

}  // namespace
}  // namespace shale
