// The skip list under the memtable: one writer, any number of readers, no locks.
//
// The ordinary tests compare it with std::set. The concurrency test is the one that matters for
// the memory ordering: readers walk the list while the writer links nodes into it, and a missing
// release store or acquire load shows up there, under ThreadSanitizer, as a report, or without it
// as a key that is out of order or not a key anyone inserted.

#include "io/skiplist.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "util/arena.h"
#include "util/hash.h"
#include "util/random.h"

namespace shale {
namespace {

using Key = uint64_t;

struct TestComparator {
  int operator()(const Key& a, const Key& b) const { return (a > b) - (a < b); }
};

using TestList = SkipList<Key, TestComparator>;

TEST(SkipListTest, Empty) {
  Arena arena;
  TestList list(TestComparator(), &arena, 1);
  EXPECT_FALSE(list.Contains(10));

  TestList::Iterator it(&list);
  EXPECT_FALSE(it.Valid());
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.Seek(100);
  EXPECT_FALSE(it.Valid());
}

TEST(SkipListTest, InsertAndLookup) {
  constexpr uint64_t kSeed = 1000;
  constexpr int kInserts = 2000;
  constexpr uint32_t kRange = 5000;
  SCOPED_TRACE("seed " + std::to_string(kSeed));

  Random rnd(kSeed);
  std::set<Key> expected;
  Arena arena;
  TestList list(TestComparator(), &arena, kSeed);
  for (int i = 0; i < kInserts; i++) {
    const Key key = rnd.Uniform(kRange);
    if (expected.insert(key).second) {
      list.Insert(key);
    }
  }
  // A shortfall here would mean the keys above were nearly all duplicates, and the test would not
  // be exercising a list of any size.
  ASSERT_GT(expected.size(), 1000u);

  for (Key k = 0; k < kRange; k++) {
    EXPECT_EQ(list.Contains(k), expected.count(k) == 1) << "key " << k;
  }

  TestList::Iterator it(&list);
  it.SeekToFirst();
  for (const Key k : expected) {
    ASSERT_TRUE(it.Valid()) << "iteration ended before key " << k;
    ASSERT_EQ(it.key(), k);
    it.Next();
  }
  EXPECT_FALSE(it.Valid()) << "iteration went past the last key";

  for (Key target = 0; target <= kRange; target++) {
    it.Seek(target);
    const auto lower = expected.lower_bound(target);
    if (lower == expected.end()) {
      EXPECT_FALSE(it.Valid()) << "Seek(" << target << ") found " << (it.Valid() ? it.key() : 0);
    } else {
      ASSERT_TRUE(it.Valid()) << "Seek(" << target << ") found nothing, wanted " << *lower;
      EXPECT_EQ(it.key(), *lower) << "Seek(" << target << ")";
    }
  }
}

TEST(SkipListTest, TheWriterMayChangeFromOneThreadToAnother) {
  // Group commit hands the writer's role from thread to thread. The rule is no overlap, not one
  // writer for life, so the guard must not trip on a second thread that comes after the first. It
  // must also hand the first thread's writes to the second: the arena's bookkeeping and the height
  // generator are plain memory that no link in the list publishes. The second thread learns that
  // the first is done through a relaxed flag, which orders nothing, and inserts keys below all of
  // the first thread's, so that its search never loads the links the first thread stored last.
  // Left to the guard alone, the hand-off is what ThreadSanitizer checks.
  Arena arena;
  TestList list(TestComparator(), &arena, 7);
  std::atomic<bool> first_is_done{false};
  std::thread second([&list, &first_is_done] {
    while (!first_is_done.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
    for (Key k = 0; k < 100; k++) {
      list.Insert(k * 2);
    }
  });
  for (Key k = 1000; k < 1100; k++) {
    list.Insert(k);
  }
  first_is_done.store(true, std::memory_order_relaxed);
  second.join();
  list.Insert(1);

  TestList::Iterator it(&list);
  it.SeekToFirst();
  size_t count = 0;
  Key previous = 0;
  for (; it.Valid(); it.Next()) {
    ASSERT_TRUE(count == 0 || it.key() > previous) << "out of order after " << previous;
    previous = it.key();
    count++;
  }
  EXPECT_EQ(count, 201u);
}

TEST(SkipListDeathTest, DuplicateInsertIsRefused) {
  EXPECT_DEATH(
      {
        Arena arena;
        TestList list(TestComparator(), &arena, 1);
        list.Insert(5);
        list.Insert(5);
      },
      "duplicate key");
}

// A comparator that, on its first call, inserts into the list it is comparing for. The comparator
// is copied into the list, so it reaches the list through a State that outlives the copy.
struct ReentrantComparator;
using ReentrantList = SkipList<Key, ReentrantComparator>;

struct ReentrantState {
  ReentrantList* list = nullptr;
  bool fired = false;
};

struct ReentrantComparator {
  ReentrantState* state;

  int operator()(const Key& a, const Key& b) const {
    if (!state->fired) {
      state->fired = true;
      state->list->Insert(999);
    }
    return (a > b) - (a < b);
  }
};

TEST(SkipListDeathTest, ReentrantInsertIsRefused) {
  EXPECT_DEATH(
      {
        Arena arena;
        ReentrantState state;
        ReentrantList list(ReentrantComparator{&state}, &arena, 1);
        state.list = &list;
        // The first key goes into an empty list and is never compared; the second is the first
        // to reach the comparator, which then inserts from inside the insert.
        list.Insert(1);
        list.Insert(2);
      },
      "concurrent SkipList::Insert");
}

// Like ReentrantComparator, but the second insert comes from another thread while this one is
// still inside Insert.
struct OtherThreadComparator;
using OtherThreadList = SkipList<Key, OtherThreadComparator>;

struct OtherThreadState {
  OtherThreadList* list = nullptr;
  bool fired = false;
};

struct OtherThreadComparator {
  OtherThreadState* state;

  int operator()(const Key& a, const Key& b) const {
    if (!state->fired) {
      state->fired = true;
      std::thread other([this] { state->list->Insert(999); });
      other.join();
    }
    return (a > b) - (a < b);
  }
};

TEST(SkipListDeathTest, InsertFromAnotherThreadWhileOneIsRunningIsRefused) {
  EXPECT_DEATH(
      {
        Arena arena;
        OtherThreadState state;
        OtherThreadList list(OtherThreadComparator{&state}, &arena, 1);
        state.list = &list;
        list.Insert(1);
        list.Insert(2);
      },
      "concurrent SkipList::Insert");
}

// A key whose high bits are its index and whose low 16 bits are a hash of the index. A reader that
// sees a key can tell, from the key alone, whether the writer finished writing it.
Key MakeKey(uint64_t i) {
  const uint32_t h = Hash(reinterpret_cast<const char*>(&i), sizeof(i), 0);
  return (i << 16) | (h & 0xffff);
}

bool IsWellFormed(Key key) { return MakeKey(key >> 16) == key; }

// What one reader thread reports. `passes` is read by the writer, which waits on it; the rest is
// read after the thread has been joined.
struct ReaderResult {
  std::atomic<uint64_t> passes{0};
  std::atomic<bool> failed{false};
  std::string failure;             // the first violation; written before `failed` is set
  uint64_t partial_passes = 0;     // passes whose result had some keys but not all of them
  uint64_t overlapped_passes = 0;  // passes during which the writer inserted something
  uint64_t last_pass_size = 0;
};

void Fail(ReaderResult* result, std::string message) {
  result->failure = std::move(message);
  result->failed.store(true, std::memory_order_release);
}

// Walks the list from the beginning, again and again, until the writer says it is done, and then
// once more: that last pass starts after the final insert and must see every key. `inserted` is
// how many inserts the writer has finished. It is read before and after each pass, and a pass that
// saw it move is one that raced with the writer, which is not the same as one that merely saw an
// unfinished list.
void ReadUntilDone(const TestList* list, const std::atomic<bool>* done,
                   const std::atomic<uint64_t>* inserted, uint64_t total, ReaderResult* result) {
  uint64_t previous_size = 0;
  bool last = false;
  while (!last) {
    last = done->load(std::memory_order_acquire);
    const uint64_t inserted_before = inserted->load(std::memory_order_acquire);
    TestList::Iterator it(list);
    it.SeekToFirst();
    uint64_t size = 0;
    Key previous = 0;
    for (; it.Valid(); it.Next()) {
      const Key key = it.key();
      if (size > 0 && key <= previous) {
        Fail(result, "key " + std::to_string(key) + " after " + std::to_string(previous) +
                         ": not strictly increasing, at position " + std::to_string(size));
        return;
      }
      if (!IsWellFormed(key)) {
        Fail(result, "key " + std::to_string(key) + " has the wrong hash bits");
        return;
      }
      // A key the iteration has reached must be found by the other two read paths as well.
      if (size % 64 == 0) {
        if (!list->Contains(key)) {
          Fail(result, "key " + std::to_string(key) + " was iterated but Contains() misses it");
          return;
        }
        TestList::Iterator probe(list);
        probe.Seek(key);
        if (!probe.Valid() || probe.key() != key) {
          Fail(result, "key " + std::to_string(key) + " was iterated but Seek() misses it");
          return;
        }
      }
      previous = key;
      size++;
    }
    // Nothing is ever removed, so a later pass cannot see fewer keys than an earlier one.
    if (size < previous_size) {
      Fail(result, "a pass saw " + std::to_string(size) + " keys after one that saw " +
                       std::to_string(previous_size));
      return;
    }
    previous_size = size;
    if (size > 0 && size < total) {
      result->partial_passes++;
    }
    if (inserted->load(std::memory_order_acquire) > inserted_before) {
      result->overlapped_passes++;
    }
    result->last_pass_size = size;
    result->passes.fetch_add(1, std::memory_order_release);
  }
}

TEST(SkipListTest, ConcurrentReadersSeeSortedValidKeys) {
  constexpr uint64_t kSeed = 4242;
  constexpr uint64_t kCount = 50000;
  constexpr uint64_t kChunk = 2000;
  constexpr uint64_t kChunks = kCount / kChunk;
  constexpr int kReaders = 4;
  constexpr auto kPatience = std::chrono::seconds(120);
  SCOPED_TRACE("seed " + std::to_string(kSeed));

  std::vector<uint64_t> order(kCount);
  std::iota(order.begin(), order.end(), uint64_t{0});
  Random rnd(kSeed);
  for (size_t i = order.size() - 1; i > 0; i--) {
    std::swap(order[i], order[rnd.Uniform(static_cast<uint32_t>(i + 1))]);
  }

  Arena arena;
  TestList list(TestComparator(), &arena, kSeed);
  std::atomic<bool> done{false};
  std::atomic<uint64_t> inserted{0};
  std::vector<ReaderResult> results(kReaders);
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; r++) {
    readers.emplace_back(ReadUntilDone, &list, &done, &inserted, kCount,
                         &results[static_cast<size_t>(r)]);
  }

  // The writer inserts a chunk, then waits until every reader has finished a pass that ended
  // after the chunk began. Without the wait a fast writer can finish before a slow reader has
  // started, and the test would pass having read nothing but the finished list.
  bool gave_up = false;
  for (uint64_t chunk = 0; chunk < kChunks && !gave_up; chunk++) {
    std::vector<uint64_t> baseline;
    baseline.reserve(results.size());
    for (const ReaderResult& result : results) {
      baseline.push_back(result.passes.load(std::memory_order_acquire));
    }
    for (uint64_t i = chunk * kChunk; i < (chunk + 1) * kChunk; i++) {
      list.Insert(MakeKey(order[i]));
      inserted.store(i + 1, std::memory_order_release);
    }
    const auto deadline = std::chrono::steady_clock::now() + kPatience;
    for (size_t r = 0; r < results.size() && !gave_up; r++) {
      while (results[r].passes.load(std::memory_order_acquire) <= baseline[r] &&
             !results[r].failed.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
          gave_up = true;
          break;
        }
        std::this_thread::yield();
      }
    }
  }
  done.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  ASSERT_FALSE(gave_up) << "a reader made no progress for " << kPatience.count() << " s";
  for (int r = 0; r < kReaders; r++) {
    const ReaderResult& result = results[static_cast<size_t>(r)];
    EXPECT_FALSE(result.failed.load()) << "reader " << r << ": " << result.failure;
    EXPECT_EQ(result.last_pass_size, kCount)
        << "reader " << r << ": the pass after the last insert";
    // Two different things are measured. A partial pass is one whose result was neither empty nor
    // complete; each chunk waits for a pass of every reader, and all but the last chunk's end
    // before the list is full, so about one per chunk is expected, and this checks that the
    // handshake above worked. An overlapped pass is one during which the writer's count of
    // finished inserts moved, which is what shows that a pass raced with inserts, and not only
    // that it ran against a list that was not yet finished.
    EXPECT_GE(result.partial_passes, kChunks / 2) << "reader " << r;
    EXPECT_GT(result.overlapped_passes, 0u)
        << "reader " << r << " never ran a pass while the writer was inserting";
  }
}

}  // namespace
}  // namespace shale
