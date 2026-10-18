#ifndef SHALE_IO_LRU_CACHE_H_
#define SHALE_IO_LRU_CACHE_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util/check.h"
#include "util/mutex.h"
#include "util/thread_annotations.h"

namespace shale {

/// A cache that keeps the entries used most recently, up to a total charge, and is safe to use
/// from many threads at once.
///
/// Each entry has a charge, the caller's measure of what it costs to keep: the bytes of a block, or
/// 1 for each open file. When the charges of a shard's entries add up to more than the shard's
/// capacity, the entries least recently used are dropped until they fit. A lookup counts as a use.
///
/// The values are shared. Dropping an entry gives up the cache's share of its value and nothing
/// more, so a reader that got the value from Lookup() can go on using it after it has been evicted,
/// and the value is destroyed when the last reader lets go. That is how a block cache can evict a
/// block that an iterator is still reading from.
///
/// One lock for a whole cache would have every thread queue behind it, so the keys are divided
/// among `2^num_shard_bits` shards by a hash, each with its own lock and its own share of the
/// capacity. The price is that the cache is least-recently-used only within a shard: a shard can
/// be full and evicting while another has room.
///
/// A value's destructor can run on whichever thread drops the last share, and never while the
/// cache holds a lock, so it may use the cache.
/// @tparam Key copyable and comparable with `==`.
/// @tparam Value the type of the cached objects.
/// @tparam Hasher hashes a Key to a size_t; need not be well mixed, because the cache mixes it.
template <typename Key, typename Value, typename Hasher = std::hash<Key>>
class ShardedLRUCache {
 public:
  /// Creates a cache that holds entries worth `capacity` in all, divided evenly, and rounded up,
  /// among the shards: with 10 and 4 shards each holds 3, so 12 can be cached. Aborts if
  /// `num_shard_bits` is outside [0, 16].
  /// @param capacity the total charge to keep; 0 caches nothing.
  /// @param num_shard_bits the base-2 logarithm of the number of shards. Use 0 where the order of
  ///   eviction has to be exact, in a test for one.
  explicit ShardedLRUCache(size_t capacity, int num_shard_bits = 4) {
    SHALE_CHECK(num_shard_bits >= 0 && num_shard_bits <= kMaxShardBits,
                "ShardedLRUCache needs num_shard_bits in [0, 16]");
    const size_t num_shards = size_t{1} << num_shard_bits;
    shard_mask_ = num_shards - 1;
    const size_t per_shard = capacity / num_shards + (capacity % num_shards != 0 ? 1 : 0);
    shards_.reserve(num_shards);
    for (size_t i = 0; i < num_shards; ++i) {
      shards_.push_back(std::make_unique<Shard>(per_shard));
    }
  }

  ShardedLRUCache(const ShardedLRUCache&) = delete;
  ShardedLRUCache& operator=(const ShardedLRUCache&) = delete;

  /// Returns the value cached for `key` and marks the entry as the most recently used, or nullptr
  /// if there is none. The value is shared with the cache and with every other caller that holds
  /// it.
  std::shared_ptr<Value> Lookup(const Key& key) { return ShardFor(key).Lookup(key); }

  /// Caches `value` under `key`, replacing any entry for the key, then drops the least recently
  /// used entries of the shard until its charges fit its capacity. The new entry is the most
  /// recently used. Returns `value`, so the caller can go on using it whether or not it was kept.
  ///
  /// It is not kept if the cache has no capacity, or if its charge alone is more than a shard's,
  /// which no amount of eviction would make room for. Nothing else is evicted for such an entry,
  /// but an entry for the key is still replaced, by nothing, so that a later Lookup() cannot return
  /// the value this one superseded.
  ///
  /// A charge of 0 counts as 1: an entry that costs nothing would never be evicted by the charges
  /// adding up, and a cache full of them, a block of no bytes from a damaged table among them,
  /// would grow without bound. Aborts if `value` is null, because a lookup could not tell it from
  /// a miss.
  /// @param key the key to cache under.
  /// @param value the object to cache.
  /// @param charge what the entry costs to keep, in the units `capacity` is in.
  std::shared_ptr<Value> Insert(const Key& key, std::shared_ptr<Value> value, size_t charge) {
    SHALE_CHECK(value != nullptr, "ShardedLRUCache::Insert needs a value");
    charge = std::max<size_t>(charge, 1);
    // Declared before the shard's lock is taken, so that it is destroyed after the lock is
    // released: what the cache drops may be the last share of a value that uses the cache.
    std::vector<std::shared_ptr<Value>> dropped;
    ShardFor(key).Insert(key, value, charge, &dropped);
    return value;
  }

  /// Drops the entry for `key`, if there is one. A reader that holds its value keeps it.
  void Erase(const Key& key) {
    std::vector<std::shared_ptr<Value>> dropped;  // Destroyed after the lock is released.
    ShardFor(key).Erase(key, &dropped);
  }

  /// Returns the sum of the charges of the entries now in the cache. Under concurrent use it is a
  /// sum of the shards taken one after another, and is not a snapshot of the cache at one moment.
  size_t TotalCharge() const {
    size_t total = 0;
    for (const std::unique_ptr<Shard>& shard : shards_) {
      total += shard->TotalCharge();
    }
    return total;
  }

 private:
  static constexpr int kMaxShardBits = 16;

  struct Entry {
    Key key;
    std::shared_ptr<Value> value;
    size_t charge;
  };

  // The part of the cache behind one lock: the entries from most to least recently used, and an
  // index from key to the entry's place in that list, which stays put while the list is reordered.
  class Shard {
   public:
    explicit Shard(size_t capacity) : capacity_(capacity) {}

    std::shared_ptr<Value> Lookup(const Key& key) SHALE_EXCLUDES(mu_) {
      MutexLock lock(&mu_);
      const auto found = index_.find(key);
      if (found == index_.end()) {
        return nullptr;
      }
      lru_.splice(lru_.begin(), lru_, found->second);
      return found->second->value;
    }

    // The values the call drops are moved into `*dropped`, for the caller to destroy.
    void Insert(const Key& key, std::shared_ptr<Value> value, size_t charge,
                std::vector<std::shared_ptr<Value>>* dropped) SHALE_EXCLUDES(mu_) {
      MutexLock lock(&mu_);
      const auto found = index_.find(key);
      if (found != index_.end()) {
        RemoveLocked(found->second, dropped);
      }
      if (capacity_ == 0 || charge > capacity_) {
        return;
      }

      lru_.push_front(Entry{key, std::move(value), charge});
      index_.emplace(key, lru_.begin());
      usage_ += charge;
      // The entry just added is at the front and fits by itself, so this stops before it.
      while (usage_ > capacity_) {
        SHALE_CHECK(lru_.size() > 1, "ShardedLRUCache would evict the entry it just added");
        RemoveLocked(std::prev(lru_.end()), dropped);
      }
    }

    void Erase(const Key& key, std::vector<std::shared_ptr<Value>>* dropped) SHALE_EXCLUDES(mu_) {
      MutexLock lock(&mu_);
      const auto found = index_.find(key);
      if (found != index_.end()) {
        RemoveLocked(found->second, dropped);
      }
    }

    size_t TotalCharge() const SHALE_EXCLUDES(mu_) {
      MutexLock lock(&mu_);
      return usage_;
    }

   private:
    using List = std::list<Entry>;

    // Removes the entry at `it` and hands its value to `*dropped`.
    void RemoveLocked(typename List::iterator it, std::vector<std::shared_ptr<Value>>* dropped)
        SHALE_REQUIRES(mu_) {
      usage_ -= it->charge;
      dropped->push_back(std::move(it->value));
      index_.erase(it->key);
      lru_.erase(it);
    }

    const size_t capacity_;
    mutable Mutex mu_;
    List lru_ SHALE_GUARDED_BY(mu_);
    std::unordered_map<Key, typename List::iterator, Hasher> index_ SHALE_GUARDED_BY(mu_);
    size_t usage_ SHALE_GUARDED_BY(mu_) = 0;
  };

  Shard& ShardFor(const Key& key) const { return *shards_[Mix(hasher_(key)) & shard_mask_]; }

  // The 64-bit finalizer of MurmurHash3: every input bit changes about half the output bits. The
  // standard hash of an integer is the integer, and the keys of a block cache are offsets that are
  // multiples of the block size, whose low bits are all zero; used as they are, they would pick
  // the same shard every time.
  static size_t Mix(size_t h) {
    uint64_t x = h;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return static_cast<size_t>(x);
  }

  Hasher hasher_;
  size_t shard_mask_ = 0;
  std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace shale

#endif  // SHALE_IO_LRU_CACHE_H_
