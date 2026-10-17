// The memtable: where every write lands first, and what a read consults before any file.
//
// Two things have to hold. Reads must follow the internal key order, newest entry of a user key
// first and a snapshot seeing only what was written at or before it, because that is how a
// deletion hides an older value without touching it. And one writer with many readers must be
// safe, which the concurrency tests check under ThreadSanitizer.

#include "io/memtable.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/random.h"

namespace shale {
namespace {

struct GetResult {
  bool found = false;
  std::string value;
  Status status;
};

GetResult Lookup(const MemTable& mem, std::string_view key, SequenceNumber sequence) {
  GetResult result;
  result.found = mem.Get(LookupKey(key, sequence), &result.value, &result.status);
  return result;
}

// The entries of a memtable, in iteration order, as "user_key@sequence" with a "!" for a deletion.
std::vector<std::string> Entries(const MemTable& mem) {
  std::vector<std::string> entries;
  const std::unique_ptr<Iterator> iter = mem.NewIterator();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey ikey;
    if (!ParseInternalKey(iter->key(), &ikey)) {
      entries.push_back("<bad internal key>");
      continue;
    }
    entries.push_back(std::string(ikey.user_key) + "@" + std::to_string(ikey.sequence) +
                      (ikey.type == ValueType::kDeletion ? "!" : ""));
  }
  return entries;
}

std::string InternalKeyOf(std::string_view user_key, SequenceNumber sequence, ValueType type) {
  std::string result;
  AppendInternalKey(&result, {user_key, sequence, type});
  return result;
}

TEST(MemTableTest, GetMissing) {
  const auto mem = std::make_shared<MemTable>(1);
  EXPECT_FALSE(Lookup(*mem, "k", 100).found) << "an empty memtable";

  mem->Add(1, ValueType::kValue, "abc", "v");
  mem->Add(2, ValueType::kValue, "abd", "v");
  Status untouched = Status::Corruption("sentinel");
  std::string value = "sentinel";
  EXPECT_FALSE(mem->Get(LookupKey("abe", 100), &value, &untouched));
  EXPECT_EQ(value, "sentinel") << "a miss must not touch the value";
  EXPECT_TRUE(untouched.IsCorruption()) << "a miss must not touch the status";

  // Neighbours in the sort order, whose user keys differ though one is a prefix of the other.
  EXPECT_FALSE(Lookup(*mem, "ab", 100).found) << "a prefix of a stored key";
  EXPECT_FALSE(Lookup(*mem, "abcd", 100).found) << "an extension of a stored key";
  EXPECT_FALSE(Lookup(*mem, "", 100).found);
  EXPECT_FALSE(Lookup(*mem, "zzz", 100).found) << "after every stored key";
}

TEST(MemTableTest, GetRespectsSequence) {
  const auto mem = std::make_shared<MemTable>(1);
  mem->Add(5, ValueType::kValue, "k", "a");

  GetResult r = Lookup(*mem, "k", 5);
  ASSERT_TRUE(r.found);
  EXPECT_EQ(r.value, "a");
  EXPECT_OK(r.status);
  EXPECT_TRUE(Lookup(*mem, "k", 6).found);
  EXPECT_TRUE(Lookup(*mem, "k", kMaxSequenceNumber).found);
  EXPECT_FALSE(Lookup(*mem, "k", 4).found) << "written after the snapshot";
  EXPECT_FALSE(Lookup(*mem, "k", 0).found);
}

TEST(MemTableTest, GetReturnsTheNewestVisibleEntry) {
  const auto mem = std::make_shared<MemTable>(1);
  mem->Add(1, ValueType::kValue, "k", "one");
  mem->Add(3, ValueType::kValue, "k", "three");
  mem->Add(7, ValueType::kValue, "k", "seven");
  EXPECT_EQ(Lookup(*mem, "k", 1).value, "one");
  EXPECT_EQ(Lookup(*mem, "k", 2).value, "one");
  EXPECT_EQ(Lookup(*mem, "k", 3).value, "three");
  EXPECT_EQ(Lookup(*mem, "k", 6).value, "three");
  EXPECT_EQ(Lookup(*mem, "k", 7).value, "seven");
  EXPECT_EQ(Lookup(*mem, "k", 100).value, "seven");
}

TEST(MemTableTest, GetReplacesTheCallersValueAndStatus) {
  // A hit says what it found with both outputs, so a caller that reuses them does not read
  // yesterday's result.
  const auto mem = std::make_shared<MemTable>(1);
  mem->Add(1, ValueType::kValue, "k", "fresh");
  std::string value = "stale and much longer than the value";
  Status s = Status::NotFound("stale");
  ASSERT_TRUE(mem->Get(LookupKey("k", 1), &value, &s));
  EXPECT_EQ(value, "fresh");
  EXPECT_OK(s);
}

TEST(MemTableTest, DeletionShadows) {
  const auto mem = std::make_shared<MemTable>(1);
  mem->Add(1, ValueType::kValue, "k", "a");
  mem->Add(2, ValueType::kDeletion, "k", "");

  GetResult r = Lookup(*mem, "k", 2);
  EXPECT_TRUE(r.found) << "a deletion is an answer: the key is gone, do not look further";
  EXPECT_TRUE(r.status.IsNotFound()) << r.status.ToString();

  r = Lookup(*mem, "k", 1);
  ASSERT_TRUE(r.found);
  EXPECT_EQ(r.value, "a");
  EXPECT_OK(r.status);

  mem->Add(3, ValueType::kValue, "k", "b");
  EXPECT_EQ(Lookup(*mem, "k", 3).value, "b") << "a put after the deletion brings the key back";
  EXPECT_TRUE(Lookup(*mem, "k", 2).status.IsNotFound());
}

TEST(MemTableTest, IteratorOrderIsInternalKeyOrder) {
  const auto mem = std::make_shared<MemTable>(1);
  mem->Add(1, ValueType::kValue, "b", "vb");
  mem->Add(2, ValueType::kValue, "a", "va2");
  mem->Add(3, ValueType::kValue, "a", "va3");
  EXPECT_EQ(Entries(*mem), (std::vector<std::string>{"a@3", "a@2", "b@1"}));

  const std::unique_ptr<Iterator> iter = mem->NewIterator();
  const std::string target = InternalKeyOf("a", 2, ValueType::kValue);
  iter->Seek(target);
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), target);
  EXPECT_EQ(iter->value(), "va2");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->value(), "vb");
  iter->Next();
  EXPECT_FALSE(iter->Valid());
  EXPECT_OK(iter->status());
}

TEST(MemTableTest, SeekLandsOnTheFirstEntryNotBeforeTheTarget) {
  const auto mem = std::make_shared<MemTable>(1);
  mem->Add(1, ValueType::kValue, "b", "vb");
  mem->Add(5, ValueType::kValue, "d", "vd");
  const std::unique_ptr<Iterator> iter = mem->NewIterator();

  // Between two user keys.
  iter->Seek(LookupKey("c", 100).internal_key());
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(ExtractUserKey(iter->key()), "d");
  // At a user key, at a snapshot older than every entry for it: the next user key is next.
  iter->Seek(LookupKey("b", 0).internal_key());
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(ExtractUserKey(iter->key()), "d");
  // At a snapshot that sees the entry.
  iter->Seek(LookupKey("b", 1).internal_key());
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(ExtractUserKey(iter->key()), "b");
  // Before everything, and after everything.
  iter->Seek(LookupKey("", 100).internal_key());
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(ExtractUserKey(iter->key()), "b");
  iter->Seek(LookupKey("e", 100).internal_key());
  EXPECT_FALSE(iter->Valid());
  // Seeking again after running off the end works.
  iter->SeekToFirst();
  EXPECT_TRUE(iter->Valid());
}

TEST(MemTableTest, AnEmptyMemTableHasAnEmptyIterator) {
  const auto mem = std::make_shared<MemTable>(1);
  const std::unique_ptr<Iterator> iter = mem->NewIterator();
  EXPECT_FALSE(iter->Valid());
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  iter->Seek(LookupKey("a", 1).internal_key());
  EXPECT_FALSE(iter->Valid());
  EXPECT_OK(iter->status());
}

TEST(MemTableTest, BinaryKeysOrderUnsigned) {
  const auto mem = std::make_shared<MemTable>(1);
  mem->Add(1, ValueType::kValue, "\x80", "high");
  mem->Add(2, ValueType::kValue, "\x7f", "mid");
  mem->Add(3, ValueType::kValue, std::string("\0", 1), "low");

  std::vector<std::string> user_keys;
  const std::unique_ptr<Iterator> iter = mem->NewIterator();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    user_keys.emplace_back(ExtractUserKey(iter->key()));
  }
  EXPECT_EQ(user_keys, (std::vector<std::string>{std::string("\0", 1), "\x7f", "\x80"}));
  EXPECT_EQ(Lookup(*mem, "\x80", 10).value, "high");
  EXPECT_EQ(Lookup(*mem, std::string("\0", 1), 10).value, "low");
  EXPECT_FALSE(Lookup(*mem, "", 10).found) << "the empty key is not the key of one zero byte";
}

TEST(MemTableTest, EntriesKeepTheirBytesWhateverTheLengths) {
  // The entry stores its two lengths as varints, so the sizes where a varint grows by a byte
  // are where an encoding mistake would show. The internal key is eight bytes longer than the
  // user key.
  const std::vector<size_t> key_sizes = {0, 1, 119, 120, 16375, 16376};
  const std::vector<size_t> value_sizes = {0, 1, 127, 128, 16383, 16384, 2097152};
  Random rnd(77);
  const auto mem = std::make_shared<MemTable>(1);
  SequenceNumber sequence = 1;
  struct Written {
    std::string key;
    std::string value;
    SequenceNumber sequence;
  };
  std::vector<Written> written;
  for (const size_t key_size : key_sizes) {
    for (const size_t value_size : value_sizes) {
      Written w{RandomBinaryString(&rnd, key_size) + std::to_string(written.size()),
                RandomBinaryString(&rnd, value_size), sequence++};
      mem->Add(w.sequence, ValueType::kValue, w.key, w.value);
      written.push_back(std::move(w));
    }
  }
  for (const Written& w : written) {
    const GetResult r = Lookup(*mem, w.key, w.sequence);
    ASSERT_TRUE(r.found) << "key of " << w.key.size() << " bytes";
    ASSERT_EQ(r.value.size(), w.value.size());
    ASSERT_TRUE(r.value == w.value) << "value of " << w.value.size() << " bytes differs";
  }
  size_t count = 0;
  const std::unique_ptr<Iterator> iter = mem->NewIterator();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    count++;
  }
  EXPECT_EQ(count, written.size());
}

TEST(MemTableTest, RandomOperationsAgreeWithAModel) {
  constexpr uint64_t kSeed = 20261017;
  constexpr uint32_t kOperations = 4000;
  SCOPED_TRACE("seed " + std::to_string(kSeed));

  struct Entry {
    ValueType type;
    std::string value;
  };
  // Small alphabet and short keys, so that many entries share a user key and many user keys are
  // prefixes of others. The model orders by user key, then newest first, like the memtable.
  const std::string alphabet(
      "\x00\x01\x7f\x80\xff"
      "a",
      6);
  Random rnd(kSeed);
  auto random_key = [&] {
    std::string key;
    const uint32_t length = rnd.Uniform(5);
    for (uint32_t i = 0; i < length; i++) {
      key.push_back(alphabet[rnd.Uniform(static_cast<uint32_t>(alphabet.size()))]);
    }
    return key;
  };

  std::vector<SequenceNumber> sequences(kOperations);
  std::iota(sequences.begin(), sequences.end(), SequenceNumber{1});
  for (size_t i = sequences.size() - 1; i > 0; i--) {
    std::swap(sequences[i], sequences[rnd.Uniform(static_cast<uint32_t>(i + 1))]);
  }

  const auto mem = std::make_shared<MemTable>(kSeed);
  std::map<std::pair<std::string, SequenceNumber>, Entry> model;
  for (const SequenceNumber sequence : sequences) {
    const std::string key = random_key();
    Entry entry;
    entry.type = rnd.OneIn(4) ? ValueType::kDeletion : ValueType::kValue;
    if (entry.type == ValueType::kValue) {
      entry.value = RandomBinaryString(&rnd, rnd.Uniform(20));
    }
    mem->Add(sequence, entry.type, key, entry.value);
    model[{key, sequence}] = entry;
  }

  // Every user key the model knows, at a spread of snapshots, plus keys it does not know.
  for (int probe = 0; probe < 3000; probe++) {
    const std::string key = random_key();
    const SequenceNumber snapshot = rnd.Uniform(kOperations + 10);
    const Entry* expected = nullptr;
    for (const auto& [id, entry] : model) {
      if (id.first == key && id.second <= snapshot) {
        expected = &entry;  // the map runs oldest first within a key, so the last one is newest
      }
    }
    const GetResult r = Lookup(*mem, key, snapshot);
    if (expected == nullptr) {
      ASSERT_FALSE(r.found) << "key #" << probe << " at snapshot " << snapshot;
    } else if (expected->type == ValueType::kDeletion) {
      ASSERT_TRUE(r.found) << "probe " << probe;
      ASSERT_TRUE(r.status.IsNotFound()) << "probe " << probe;
    } else {
      ASSERT_TRUE(r.found) << "probe " << probe;
      ASSERT_OK(r.status);
      ASSERT_TRUE(r.value == expected->value) << "probe " << probe;
    }
  }

  // The whole iteration: every entry once, in the order the model gives with newest first.
  std::vector<std::pair<std::string, SequenceNumber>> order;
  for (const auto& [id, entry] : model) {
    order.push_back(id);
  }
  std::vector<std::pair<std::string, SequenceNumber>> expected_order;
  for (size_t begin = 0; begin < order.size();) {
    size_t end = begin;
    while (end < order.size() && order[end].first == order[begin].first) {
      end++;
    }
    for (size_t i = end; i > begin; i--) {
      expected_order.push_back(order[i - 1]);
    }
    begin = end;
  }
  const std::unique_ptr<Iterator> iter = mem->NewIterator();
  size_t position = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next(), position++) {
    ParsedInternalKey ikey;
    ASSERT_TRUE(ParseInternalKey(iter->key(), &ikey));
    ASSERT_LT(position, expected_order.size());
    ASSERT_EQ(std::string(ikey.user_key), expected_order[position].first) << position;
    ASSERT_EQ(ikey.sequence, expected_order[position].second) << position;
    const Entry& entry = model.at(expected_order[position]);
    ASSERT_EQ(ikey.type, entry.type);
    ASSERT_TRUE(iter->value() == entry.value) << position;
  }
  EXPECT_EQ(position, expected_order.size());
}

TEST(MemTableTest, IteratorKeepsMemTableAlive) {
  auto m = std::make_shared<MemTable>(1);
  m->Add(1, ValueType::kValue, "a", "va");
  m->Add(2, ValueType::kValue, "b", "vb");
  std::unique_ptr<Iterator> it = m->NewIterator();
  const std::weak_ptr<MemTable> w = m;
  m.reset();
  EXPECT_FALSE(w.expired()) << "the iterator is the last owner, so the memtable must still exist";

  std::vector<std::string> values;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    values.emplace_back(it->value());
  }
  EXPECT_EQ(values, (std::vector<std::string>{"va", "vb"}));
  it.reset();
  EXPECT_TRUE(w.expired()) << "an iterator that is gone must not keep the memtable alive";
}

TEST(MemTableDeathTest, AMemTableNotOwnedByASharedPtrRefusesToHandOutAnIterator) {
  // Without an owner the iterator would hold a dangling reference. The memtable says so instead.
  EXPECT_DEATH(
      {
        MemTable on_the_stack(1);
        const std::unique_ptr<Iterator> it = on_the_stack.NewIterator();
      },
      "owned by a shared_ptr");
}

TEST(MemTableDeathTest, AddingTheSameEntryTwiceAborts) {
  // The engine never issues a sequence number twice, so two entries for one user key, sequence
  // number and type mean the caller is broken, and the skip list refuses the second.
  EXPECT_DEATH(
      {
        const auto mem = std::make_shared<MemTable>(1);
        mem->Add(7, ValueType::kValue, "k", "a");
        mem->Add(7, ValueType::kValue, "k", "b");
      },
      "duplicate key");
}

TEST(MemTableDeathTest, ADeletionWithAValueAborts) {
  EXPECT_DEATH(
      {
        const auto mem = std::make_shared<MemTable>(1);
        mem->Add(1, ValueType::kDeletion, "k", "not empty");
      },
      "a deletion carries no value");
}

TEST(MemTableDeathTest, SeekingToSomethingThatIsNotAnInternalKeyAborts) {
  // The iterator's keys are internal keys, so its Seek() takes one: eight bytes at least, for the
  // tag. A user key is the likely mistake, and it must not be silently read as something else.
  EXPECT_DEATH(
      {
        const auto mem = std::make_shared<MemTable>(1);
        const std::unique_ptr<Iterator> it = mem->NewIterator();
        it->Seek("key");
      },
      "internal key");
}

TEST(MemTableTest, MemoryUsageGrows) {
  const auto mem = std::make_shared<MemTable>(1);
  const size_t empty = mem->ApproximateMemoryUsage();
  EXPECT_GT(empty, 0u) << "even an empty memtable has its list head";

  size_t payload = 0;
  size_t previous = empty;
  for (int i = 0; i < 1000; i++) {
    const std::string key = "key" + std::to_string(i);
    const std::string value(100, 'v');
    mem->Add(static_cast<SequenceNumber>(i + 1), ValueType::kValue, key, value);
    payload += key.size() + value.size();
    const size_t usage = mem->ApproximateMemoryUsage();
    ASSERT_GE(usage, previous) << "memory usage went down at entry " << i;
    previous = usage;
  }
  // Every byte of key and value is held, so the figure is at least that much above where it began.
  EXPECT_GE(previous, empty + payload);
}

TEST(MemTableTest, ALargeValueCountsAtOnce) {
  const auto mem = std::make_shared<MemTable>(1);
  const size_t before = mem->ApproximateMemoryUsage();
  mem->Add(1, ValueType::kValue, "k", std::string(1 << 20, 'v'));
  EXPECT_GE(mem->ApproximateMemoryUsage(), before + (1 << 20));
}

// Keys and values for the concurrent tests: the value says which key it belongs to, so a reader
// can tell a correct entry from a half-written one.
std::string ConcurrentKey(uint64_t i) {
  // Zero-padded, so that the keys sort in the order they are numbered.
  const std::string digits = std::to_string(i);
  return "key" + std::string(digits.size() < 8 ? 8 - digits.size() : 0, '0') + digits;
}

std::string ConcurrentValue(uint64_t i) {
  return "value of " + ConcurrentKey(i) + std::string(i % 50, '*');
}

TEST(MemTableTest, ReadersSeeEveryCompletedAddWhileTheWriterRuns) {
  constexpr uint64_t kSeed = 99;
  constexpr uint64_t kCount = 20000;
  constexpr uint64_t kChunk = 1000;
  constexpr int kReaders = 3;
  SCOPED_TRACE("seed " + std::to_string(kSeed));

  const auto mem = std::make_shared<MemTable>(kSeed);
  std::atomic<uint64_t> published{0};  // entries 0..published-1 are complete
  std::atomic<bool> done{false};
  struct Report {
    std::atomic<uint64_t> passes{0};
    std::atomic<bool> failed{false};
    std::string failure;
    uint64_t gets = 0;
  };
  std::vector<Report> reports(kReaders);

  auto fail = [](Report* report, std::string message) {
    report->failure = std::move(message);
    report->failed.store(true, std::memory_order_release);
  };

  auto read = [&](int id) {
    Report* report = &reports[static_cast<size_t>(id)];
    Random rnd(kSeed + static_cast<uint64_t>(id));
    size_t previous_usage = 0;
    bool last = false;
    while (!last && !report->failed.load(std::memory_order_relaxed)) {
      last = done.load(std::memory_order_acquire);
      // Everything below this was fully added before it was published, so Get must find it.
      const uint64_t complete = published.load(std::memory_order_acquire);
      for (int i = 0; i < 200 && complete > 0; i++) {
        const uint64_t n = rnd.Next() % complete;
        const GetResult r = Lookup(*mem, ConcurrentKey(n), kMaxSequenceNumber);
        report->gets++;
        if (!r.found || !r.status.ok() || r.value != ConcurrentValue(n)) {
          fail(report, "entry " + std::to_string(n) + " of " + std::to_string(complete) +
                           " published was not found intact");
          return;
        }
      }
      // A walk sees sorted, whole entries, and at least the published ones.
      const std::unique_ptr<Iterator> iter = mem->NewIterator();
      uint64_t seen = 0;
      std::string previous_key;
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        const std::string_view user_key = ExtractUserKey(iter->key());
        if (seen > 0 && !(previous_key < user_key)) {
          fail(report, "keys out of order at " + std::to_string(seen));
          return;
        }
        // The keys are written in order, so position k holds entry k.
        if (user_key != ConcurrentKey(seen) || iter->value() != ConcurrentValue(seen)) {
          fail(report, "position " + std::to_string(seen) + " holds the wrong entry");
          return;
        }
        previous_key.assign(user_key);
        seen++;
      }
      if (seen < complete) {
        fail(report, "a walk saw " + std::to_string(seen) + " entries but " +
                         std::to_string(complete) + " were published");
        return;
      }
      // The figure is read from another thread while the writer allocates, and only grows.
      const size_t usage = mem->ApproximateMemoryUsage();
      if (usage < previous_usage) {
        fail(report, "memory usage went from " + std::to_string(previous_usage) + " to " +
                         std::to_string(usage));
        return;
      }
      previous_usage = usage;
      report->passes.fetch_add(1, std::memory_order_release);
    }
  };

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; r++) {
    readers.emplace_back(read, r);
  }

  // As in the skip list test, the writer waits at every chunk until each reader has finished a
  // pass, so the readers cannot all be late.
  bool gave_up = false;
  for (uint64_t i = 0; i < kCount && !gave_up; i++) {
    mem->Add(i + 1, ValueType::kValue, ConcurrentKey(i), ConcurrentValue(i));
    published.store(i + 1, std::memory_order_release);
    if ((i + 1) % kChunk != 0) {
      continue;
    }
    std::vector<uint64_t> baseline;
    baseline.reserve(reports.size());
    for (const Report& report : reports) {
      baseline.push_back(report.passes.load(std::memory_order_acquire));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (size_t r = 0; r < reports.size() && !gave_up; r++) {
      while (reports[r].passes.load(std::memory_order_acquire) <= baseline[r] &&
             !reports[r].failed.load(std::memory_order_acquire)) {
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

  ASSERT_FALSE(gave_up) << "a reader made no progress for 120 s";
  for (int r = 0; r < kReaders; r++) {
    const Report& report = reports[static_cast<size_t>(r)];
    EXPECT_FALSE(report.failed.load()) << "reader " << r << ": " << report.failure;
    EXPECT_GE(report.passes.load(), kCount / kChunk) << "reader " << r << " made too few passes";
    EXPECT_GT(report.gets, 0u) << "reader " << r;
  }
}

TEST(MemTableTest, TheWriterMayChangeFromOneThreadToAnother) {
  // The write path hands the memtable from one writer thread to the next. That is allowed; only
  // two at once is not, and the guard against that must not mistake the first for the second.
  // It must also hand over the arena's bookkeeping, which nothing else publishes. The second
  // thread learns that the first is done through a relaxed flag, which orders nothing, so only
  // the guard can do it, and ThreadSanitizer reports the arena race if it does not. The second
  // thread's keys sort before the first's, so that its skip list search never loads the links that
  // the first thread stored last, which would hand it the same bytes by another route.
  const auto mem = std::make_shared<MemTable>(1);
  std::atomic<bool> first_is_done{false};
  std::thread second([&mem, &first_is_done] {
    while (!first_is_done.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
    for (SequenceNumber i = 0; i < 100; i++) {
      mem->Add(1000 + i, ValueType::kValue, "a" + std::to_string(i), "second");
    }
  });
  for (SequenceNumber i = 0; i < 100; i++) {
    mem->Add(1 + i, ValueType::kValue, "b" + std::to_string(i), "first");
  }
  first_is_done.store(true, std::memory_order_relaxed);
  second.join();

  const std::vector<std::string> entries = Entries(*mem);
  EXPECT_EQ(entries.size(), 200u);
  EXPECT_EQ(entries.front().substr(0, 2), "a0");
}

TEST(MemTableDeathTest, TwoWritersAtOnceAreRefused) {
  // The arena and the skip list both assume one writer, and the guard in Add is what makes that
  // an error instead of a corrupted memtable. To catch two threads inside Add together, the child
  // runs rounds until one is caught: in each, two threads start together and add large entries,
  // so that nearly all of their time is spent inside Add. A child that is never caught gives up
  // after a minute and exits normally, which fails the test.
  EXPECT_DEATH(
      {
        const std::string value(1 << 16, 'v');
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < give_up) {
          const auto mem = std::make_shared<MemTable>(1);
          std::atomic<int> ready{0};
          auto write = [&](char who) {
            ready.fetch_add(1);
            while (ready.load() < 2) {
              std::this_thread::yield();
            }
            for (int i = 0; i < 500; i++) {
              mem->Add(static_cast<SequenceNumber>(i + 1), ValueType::kValue,
                       std::string(1, who) + std::to_string(i), value);
            }
          };
          std::thread a(write, 'a');
          std::thread b(write, 'b');
          a.join();
          b.join();
        }
      },
      "concurrent MemTable::Add");
}

}  // namespace
}  // namespace shale
