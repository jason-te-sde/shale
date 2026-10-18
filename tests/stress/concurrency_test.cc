// The open database under real threads: writers, readers, scanners and snapshot readers that run
// at once, with the database's own scheduler thread doing the flushes and compactions that they
// race with, on an in-memory Env. The tests of the other layers drive the database from one thread
// and a ManualScheduler, which makes them deterministic and cannot find a race. These can, and
// they are meant to be run under ThreadSanitizer, more than once: a race is a thing that does not
// happen on every run.
//
// A test of this kind passes just as well while the engine sits still, so each one also says what
// the run did: it reads the flush and compaction counters, and the threads wait for each other at
// a few points so that the readers are known to have read while a writer was part of the way
// through. No test waits for a fixed time; every wait is a poll with a bound.

#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "db/db_test_util.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/test_util.h"
#include "util/random.h"

namespace shale {
namespace {

// The options of every test here: a memtable of the smallest size that is allowed, and levels of
// the smallest, so that a few thousand writes make dozens of flushes and make compactions run
// while the threads read. The scheduler is the database's own, on a thread of its own.
Options StressOptions(Env* env) {
  Options options;
  options.env = env;
  options.scheduler = nullptr;
  options.create_if_missing = true;
  options.write_buffer_size = 16384;
  options.target_file_size = 16384;
  options.max_bytes_for_level_base = 65536;
  return options;
}

WriteOptions NoSync() {
  WriteOptions options;
  options.sync = false;
  return options;
}

// The number that `text` is, if it is a decimal number and nothing else.
template <typename Number>
std::optional<Number> ParseNumber(std::string_view text) {
  Number number = 0;
  const char* const end = text.data() + text.size();
  const auto [stopped_at, error] = std::from_chars(text.data(), end, number);
  if (error != std::errc() || stopped_at != end) {
    return std::nullopt;
  }
  return number;
}

// Threads that are always joined, so that a test that fails early does not leave one running over
// the memory of the test it was started by.
class ThreadGroup {
 public:
  ThreadGroup() = default;
  ~ThreadGroup() { Join(); }
  ThreadGroup(const ThreadGroup&) = delete;
  ThreadGroup& operator=(const ThreadGroup&) = delete;

  void Start(std::function<void()> body) { threads_.emplace_back(std::move(body)); }

  void Join() {
    for (std::thread& thread : threads_) {
      thread.join();
    }
    threads_.clear();
  }

 private:
  std::vector<std::thread> threads_;
};

// What the threads of a test say went wrong. A thread that finds a failure keeps its message and
// stops, and so do the others, so that one broken invariant is one message and not thousands.
// gtest's own assertions are not used on the threads for that reason, and so that the test's
// thread says it all at the end.
class Failures {
 public:
  void Add(const std::string& message) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (count_++ == 0) {
      first_ = message;
    }
    any_.store(true);
  }

  bool Any() const { return any_.load(); }

  std::string Report() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return std::to_string(count_) + " failure(s), the first: " + first_;
  }

 private:
  mutable std::mutex mutex_;
  int count_ = 0;
  std::string first_;
  std::atomic<bool> any_{false};
};

// Waits until `done` is true, or until some thread has failed, which ends the test early anyway.
// A wait that runs out of time is a failure of its own: it means a thread is stuck.
void AwaitProgress(Failures* failures, const char* what, const std::function<bool()>& done) {
  const bool settled = PollUntil([&] { return done() || failures->Any(); });
  if (!settled) {
    failures->Add(std::string("timed out waiting for ") + what);
  }
}

// Waits until every one of `counters` has gone up since the call. A thread that holds still for
// this knows that the others have done some more of their work in the meantime, so that what they
// did overlapped what the thread was in the middle of.
void AwaitAllAdvance(Failures* failures, const char* what,
                     const std::vector<std::atomic<uint64_t>>& counters) {
  std::vector<uint64_t> before(counters.size());
  for (size_t i = 0; i < counters.size(); ++i) {
    before[i] = counters[i].load();
  }
  AwaitProgress(failures, what, [&] {
    for (size_t i = 0; i < counters.size(); ++i) {
      if (counters[i].load() <= before[i]) {
        return false;
      }
    }
    return true;
  });
}

// What the database has counted of the two kinds of background work, and the whole text of the
// property they come from, for a message that says what the database had done.
struct Counters {
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  std::string text;
};

// Finds the counter `name` in `text`, a list of "name=value" pairs separated by spaces.
bool FindCounter(std::string_view text, std::string_view name, uint64_t* value) {
  size_t begin = 0;
  while (begin < text.size()) {
    size_t end = text.find(' ', begin);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::string_view pair = text.substr(begin, end - begin);
    const size_t equals = pair.find('=');
    if (equals != std::string_view::npos && pair.substr(0, equals) == name) {
      const std::optional<uint64_t> parsed = ParseNumber<uint64_t>(pair.substr(equals + 1));
      if (!parsed.has_value()) {
        return false;
      }
      *value = *parsed;
      return true;
    }
    begin = end + 1;
  }
  return false;
}

// Reads "shale.counters". It never touches gtest, so a worker thread may call it; it returns false,
// with `text` saying what was read, when the property is missing or has no such counter.
bool ReadCounters(DB* db, Counters* counters) {
  if (!db->GetProperty("shale.counters", &counters->text)) {
    counters->text = "(no shale.counters property)";
    return false;
  }
  return FindCounter(counters->text, "flushes", &counters->flushes) &&
         FindCounter(counters->text, "compactions", &counters->compactions);
}

// Reads everything that the database holds, as of `options`, in key order. Returns the status of
// the iterator, which is what says whether the scan ended because the data did.
Status ScanAll(DB* db, const ReadOptions& options, Entries* entries) {
  entries->clear();
  const std::unique_ptr<Iterator> it = db->NewIterator(options);
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    entries->emplace_back(std::string(it->key()), std::string(it->value()));
  }
  return it->status();
}

// A snapshot that is given back on every path out of the test: the database aborts when it is
// closed with one still open, which would hide the failure that made the test return.
class ScopedSnapshot {
 public:
  explicit ScopedSnapshot(DB* db) : db_(db), snapshot_(db->GetSnapshot()) {}
  ~ScopedSnapshot() { Release(); }
  ScopedSnapshot(const ScopedSnapshot&) = delete;
  ScopedSnapshot& operator=(const ScopedSnapshot&) = delete;

  const Snapshot* get() const { return snapshot_; }

  void Release() {
    if (snapshot_ != nullptr) {
      db_->ReleaseSnapshot(snapshot_);
      snapshot_ = nullptr;
    }
  }

 private:
  DB* const db_;
  const Snapshot* snapshot_;
};

// An in-memory database that flushes and compacts on a thread of its own.
class ConcurrencyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = NewMemEnv();
    ASSERT_OK(DB::Open(StressOptions(env_.get()), "/db", &db_));
  }

  // Fails the test unless, since `baseline`, the run flushed `min_flushes` times and compacted at
  // least once: the work that the threads raced with, and not the work of the setup before them.
  // The counters are polled, since the last compaction may finish a moment after the last write,
  // and the assertion is made once, with what the database had counted before and after.
  void ExpectBackgroundWork(const Counters& baseline, uint64_t min_flushes) {
    Counters now;
    bool readable = true;
    const bool enough = PollUntil([&] {
      readable = ReadCounters(db_.get(), &now);
      return !readable || (now.flushes >= baseline.flushes + min_flushes &&
                           now.compactions >= baseline.compactions + 1);
    });
    EXPECT_TRUE(readable && enough)
        << "expected at least " << min_flushes << " flushes and 1 compaction since ["
        << baseline.text << "], and the counters now are [" << now.text << "]";
  }

  std::unique_ptr<Env> env_;  // declared before db_: it is destroyed after the database
  std::unique_ptr<DB> db_;
};

// Four writers, each with a hundred keys of its own, put the versions 1 to 400 into each in
// order. After every put the writer reads its key and must see that version: nothing else writes
// it. Four readers read random keys, whoever's, and remember the newest version they saw of each:
// a version never goes down, and a key that was seen does not go missing, whatever flush or
// compaction is going on, and no version is seen that its writer had not begun to put.
TEST_F(ConcurrencyTest, ReadYourWritesAndMonotonicVersions) {
  constexpr size_t kWriters = 4;
  constexpr size_t kReaders = 4;
  constexpr size_t kKeysPerWriter = 100;
  constexpr size_t kKeys = kWriters * kKeysPerWriter;
  constexpr int kVersions = 400;
  constexpr uint64_t kSeed = 0x5eed0001;

  std::vector<std::string> keys;
  for (size_t w = 0; w < kWriters; ++w) {
    for (size_t k = 0; k < kKeysPerWriter; ++k) {
      keys.push_back("w" + std::to_string(w) + "-k" + std::to_string(k));
    }
  }
  // The newest version that the writer of a key has begun to put. A reader loads it after its
  // read, so the version that it read is at most this.
  std::vector<std::atomic<int>> started(kKeys);
  std::vector<std::atomic<uint64_t>> reads(kReaders);
  // The reads that found a newer version of a key than the reader had seen of it: the cases that
  // the check that versions never go down has something to compare.
  std::vector<std::atomic<uint64_t>> increases(kReaders);
  std::atomic<bool> writers_done{false};
  Failures failures;
  const WriteOptions write_options = NoSync();
  const ReadOptions read_options;
  Counters baseline;
  ASSERT_TRUE(ReadCounters(db_.get(), &baseline)) << baseline.text;

  auto write_and_read_back = [&](size_t w) {
    std::string value;
    for (int version = 1; version <= kVersions; ++version) {
      for (size_t k = 0; k < kKeysPerWriter; ++k) {
        if (failures.Any()) {
          return;
        }
        const size_t index = w * kKeysPerWriter + k;
        const std::string expected = std::to_string(version);
        started[index].store(version);
        Status s = db_->Put(write_options, keys[index], expected);
        if (!s.ok()) {
          failures.Add("writer " + std::to_string(w) + " Put(" + keys[index] +
                       ") failed: " + s.ToString());
          return;
        }
        s = db_->Get(read_options, keys[index], &value);
        if (!s.ok() || value != expected) {
          failures.Add("writer " + std::to_string(w) + " put version " + expected + " of " +
                       keys[index] + " and read back " +
                       (s.ok() ? "version " + value : s.ToString()));
          return;
        }
        // Three times in the run, writer 0 holds still until every reader has read some more, so
        // that the readers are known to have read while a writer was part of the way through.
        if (w == 0 && k == 0 && version % 100 == 0 && version < kVersions) {
          AwaitAllAdvance(&failures, "every reader to read while a writer is part way", reads);
        }
      }
    }
  };

  auto read_at_random = [&](size_t r) {
    Random rnd(kSeed + r);
    std::vector<int> last_seen(kKeys, 0);
    std::string value;
    while (!writers_done.load() && !failures.Any()) {
      const size_t index = rnd.Uniform(static_cast<uint32_t>(kKeys));
      const Status s = db_->Get(read_options, keys[index], &value);
      const int newest_started = started[index].load();
      auto where = [&] {
        return "reader " + std::to_string(r) + " (seed " + std::to_string(kSeed + r) + ") of " +
               keys[index] + ": ";
      };
      if (s.IsNotFound()) {
        if (last_seen[index] != 0) {
          failures.Add(where() + "the key was seen at version " + std::to_string(last_seen[index]) +
                       " and is now missing");
          return;
        }
      } else if (!s.ok()) {
        failures.Add(where() + "Get failed: " + s.ToString());
        return;
      } else {
        const std::optional<int> version = ParseNumber<int>(value);
        if (!version.has_value() || *version < 1 || *version > kVersions) {
          failures.Add(where() + "the value is not a version: \"" + value + "\"");
          return;
        }
        if (*version < last_seen[index]) {
          failures.Add(where() + "version " + value + " after version " +
                       std::to_string(last_seen[index]));
          return;
        }
        if (*version > newest_started) {
          failures.Add(where() + "version " + value + " before its writer began to put it, at " +
                       std::to_string(newest_started));
          return;
        }
        if (last_seen[index] != 0 && *version > last_seen[index]) {
          increases[r].fetch_add(1);
        }
        last_seen[index] = *version;
      }
      reads[r].fetch_add(1);
    }
  };

  ThreadGroup writers;
  ThreadGroup readers;
  for (size_t r = 0; r < kReaders; ++r) {
    readers.Start([&, r] { read_at_random(r); });
  }
  for (size_t w = 0; w < kWriters; ++w) {
    writers.Start([&, w] { write_and_read_back(w); });
  }
  writers.Join();
  writers_done.store(true);
  readers.Join();

  ASSERT_FALSE(failures.Any()) << failures.Report();
  uint64_t total_increases = 0;
  for (size_t r = 0; r < kReaders; ++r) {
    EXPECT_GT(reads[r].load(), 0u) << "reader " << r << " never read";
    total_increases += increases[r].load();
  }
  EXPECT_GT(total_increases, 0u) << "no reader saw a key go to a newer version, so the check that "
                                    "versions never go down had nothing to check";
  ExpectBackgroundWork(baseline, 5);

  // Nothing was lost on the way: every key holds the last version, to a read and to a scan.
  std::string value;
  for (const std::string& key : keys) {
    ASSERT_OK(db_->Get(read_options, key, &value));
    EXPECT_EQ(value, std::to_string(kVersions)) << key;
  }
  Entries all;
  ASSERT_OK(ScanAll(db_.get(), read_options, &all));
  EXPECT_EQ(all.size(), kKeys);
  for (const auto& [key, final_value] : all) {
    EXPECT_EQ(final_value, std::to_string(kVersions)) << key;
  }
}

// One writer applies 300 batches, and batch i sets all of a hundred keys to i. Three scanners
// scan the whole database again and again: a scan that sees a batch sees all of it, so it holds
// exactly the hundred keys with one value among them, and that value never goes back from one scan
// to the next, whatever flush or compaction installs a new version while it runs. Each scan is
// bracketed by two readings of the compactions counter, and the test asks that some scan was
// running when a compaction finished. That the files a scan reads are not deleted under it is not
// shown here, where the live data is one small file; SnapshotReadsAreStable holds a scan over
// several files across compactions for that.
TEST_F(ConcurrencyTest, IteratorsSeeAtomicBatchesDuringCompaction) {
  constexpr size_t kKeys = 100;
  constexpr size_t kScanners = 3;
  constexpr int kBatches = 300;

  std::vector<std::string> keys;
  for (size_t i = 0; i < kKeys; ++i) {
    keys.push_back((i < 10 ? "k0" : "k") + std::to_string(i));
  }
  const WriteOptions write_options = NoSync();
  auto apply_batch = [&](int number) {
    WriteBatch batch;
    for (const std::string& key : keys) {
      batch.Put(key, std::to_string(number));
    }
    return db_->Write(write_options, &batch);
  };
  // So that a scan always has the hundred keys to find, whenever it starts.
  ASSERT_OK(apply_batch(0));
  Counters baseline;
  ASSERT_TRUE(ReadCounters(db_.get(), &baseline)) << baseline.text;

  std::atomic<int> newest_started{0};
  std::vector<std::atomic<uint64_t>> scans(kScanners);
  // The scans that were running when a compaction finished, by scanner.
  std::vector<std::atomic<uint64_t>> spanned(kScanners);
  std::atomic<bool> writer_done{false};
  Failures failures;

  auto write_batches = [&] {
    for (int number = 1; number <= kBatches; ++number) {
      if (failures.Any()) {
        return;
      }
      newest_started.store(number);
      const Status s = apply_batch(number);
      if (!s.ok()) {
        failures.Add("batch " + std::to_string(number) + " failed: " + s.ToString());
        return;
      }
      // Twice in the run, the writer pauses between two batches until every scanner has finished a
      // scan. That only makes sure that the scanners are getting on with it while the run is part
      // way through, and those scans run between batches: the scans that overlap a write, a flush
      // or a compaction are the ones of the scanners looping for the whole run.
      if (number % 100 == 0 && number < kBatches) {
        AwaitAllAdvance(&failures, "every scanner to finish a scan while the run is part way",
                        scans);
      }
    }
  };

  auto scan_repeatedly = [&](size_t scanner) {
    int last_value = 0;
    Entries entries;
    ReadOptions read_options;
    // A scan of the whole database is what turns the cache off, and one scanner keeps it on.
    read_options.fill_cache = scanner == 0;
    while (!writer_done.load() && !failures.Any()) {
      auto where = [&] {
        return "scanner " + std::to_string(scanner) + ", scan " +
               std::to_string(scans[scanner].load()) + ": ";
      };
      Counters before;
      Counters after;
      if (!ReadCounters(db_.get(), &before)) {
        failures.Add(where() + "could not read the counters: " + before.text);
        return;
      }
      const Status s = ScanAll(db_.get(), read_options, &entries);
      const int upper_bound = newest_started.load();
      if (!ReadCounters(db_.get(), &after)) {
        failures.Add(where() + "could not read the counters: " + after.text);
        return;
      }
      if (after.compactions > before.compactions) {
        spanned[scanner].fetch_add(1);
      }
      if (!s.ok()) {
        failures.Add(where() + "the iterator failed: " + s.ToString());
        return;
      }
      if (entries.size() != kKeys) {
        failures.Add(where() + "found " + std::to_string(entries.size()) + " keys, not " +
                     std::to_string(kKeys) + ", the first of them " +
                     (entries.empty() ? "none" : entries.front().first));
        return;
      }
      for (size_t i = 0; i < kKeys; ++i) {
        if (entries[i].first != keys[i]) {
          failures.Add(where() + "entry " + std::to_string(i) + " is " + entries[i].first +
                       ", not " + keys[i]);
          return;
        }
        if (entries[i].second != entries[0].second) {
          failures.Add(where() + keys[0] + " is " + entries[0].second + " and " + keys[i] + " is " +
                       entries[i].second + ": a batch was seen in part");
          return;
        }
      }
      const std::optional<int> value = ParseNumber<int>(entries[0].second);
      if (!value.has_value() || *value > upper_bound) {
        failures.Add(where() + "the value is \"" + entries[0].second + "\", and no batch past " +
                     std::to_string(upper_bound) + " had begun");
        return;
      }
      if (*value < last_value) {
        failures.Add(where() + "batch " + std::to_string(*value) + " after batch " +
                     std::to_string(last_value));
        return;
      }
      last_value = *value;
      scans[scanner].fetch_add(1);
    }
  };

  ThreadGroup scanners;
  ThreadGroup writer;
  for (size_t scanner = 0; scanner < kScanners; ++scanner) {
    scanners.Start([&, scanner] { scan_repeatedly(scanner); });
  }
  writer.Start(write_batches);
  writer.Join();
  writer_done.store(true);
  scanners.Join();

  ASSERT_FALSE(failures.Any()) << failures.Report();
  uint64_t total_spanned = 0;
  for (size_t scanner = 0; scanner < kScanners; ++scanner) {
    EXPECT_GT(scans[scanner].load(), 0u) << "scanner " << scanner << " never finished a scan";
    total_spanned += spanned[scanner].load();
  }
  EXPECT_GT(total_spanned, 0u) << "no scan was running when a compaction finished";
  ExpectBackgroundWork(baseline, 5);

  Entries all;
  ASSERT_OK(ScanAll(db_.get(), ReadOptions(), &all));
  ASSERT_EQ(all.size(), kKeys);
  for (const auto& [key, value] : all) {
    EXPECT_EQ(value, std::to_string(kBatches)) << key;
  }
}

// A snapshot is taken after a thousand keys have each been put once. Two writers then overwrite
// every key ten times each, which flushes and compacts under the readers, and two readers read all
// the thousand keys at the snapshot twenty times, and scan at it: every read is what the key held
// when the snapshot was taken, since what a compaction drops is only what no snapshot can see. The
// passes of a reader are spread over the writers' progress, so that they do not all run before the
// first flush. A third reader holds one scan at the snapshot open across compactions, which is what
// would show a file deleted while a scan still needed it. When the writers are done the database is
// compacted whole, and the snapshot is read once more.
TEST_F(ConcurrencyTest, SnapshotReadsAreStable) {
  constexpr size_t kKeys = 1000;
  constexpr size_t kWriters = 2;
  constexpr size_t kReaders = 2;
  constexpr int kPasses = 20;
  constexpr int kRounds = 10;

  std::vector<std::string> keys;
  std::vector<std::string> at_snapshot;
  const WriteOptions write_options = NoSync();
  for (size_t i = 0; i < kKeys; ++i) {
    keys.push_back("key" + std::to_string(1000 + i));
    at_snapshot.push_back("first-" + std::to_string(i));
    ASSERT_OK(db_->Put(write_options, keys.back(), at_snapshot.back()));
  }
  ScopedSnapshot snapshot(db_.get());
  ReadOptions at;
  at.snapshot = snapshot.get();
  Counters baseline;
  ASSERT_TRUE(ReadCounters(db_.get(), &baseline)) << baseline.text;

  // Whether every key reads, and scans, as it did at the snapshot. Returns what is wrong, or "".
  auto check_snapshot = [&]() -> std::string {
    std::string value;
    for (size_t i = 0; i < kKeys; ++i) {
      const Status s = db_->Get(at, keys[i], &value);
      if (!s.ok() || value != at_snapshot[i]) {
        return "Get(" + keys[i] + ") at the snapshot is " +
               (s.ok() ? "\"" + value + "\"" : s.ToString()) + ", not \"" + at_snapshot[i] + "\"";
      }
    }
    Entries entries;
    const Status s = ScanAll(db_.get(), at, &entries);
    if (!s.ok()) {
      return "the scan at the snapshot failed: " + s.ToString();
    }
    if (entries.size() != kKeys) {
      return "the scan at the snapshot found " + std::to_string(entries.size()) + " keys, not " +
             std::to_string(kKeys);
    }
    for (size_t i = 0; i < kKeys; ++i) {
      if (entries[i].first != keys[i] || entries[i].second != at_snapshot[i]) {
        return "entry " + std::to_string(i) + " of the scan at the snapshot is " +
               entries[i].first + "=" + entries[i].second + ", not " + keys[i] + "=" +
               at_snapshot[i];
      }
    }
    return "";
  };

  std::atomic<int> rounds_done{0};
  Failures failures;

  auto overwrite = [&](size_t w) {
    for (int round = 0; round < kRounds; ++round) {
      for (size_t i = 0; i < kKeys; ++i) {
        if (failures.Any()) {
          return;
        }
        const Status s = db_->Put(
            write_options, keys[i],
            "w" + std::to_string(w) + "-r" + std::to_string(round) + "-" + std::to_string(i));
        if (!s.ok()) {
          failures.Add("writer " + std::to_string(w) + " Put(" + keys[i] +
                       ") failed: " + s.ToString());
          return;
        }
      }
      rounds_done.fetch_add(1);
    }
  };

  auto read_at_snapshot = [&](size_t r) {
    constexpr int kTotalRounds = static_cast<int>(kWriters) * kRounds;
    for (int pass = 0; pass < kPasses; ++pass) {
      const int rounds_wanted = pass * kTotalRounds / kPasses;
      AwaitProgress(&failures, "the writers to get on",
                    [&] { return rounds_done.load() >= rounds_wanted; });
      if (failures.Any()) {
        return;
      }
      const std::string wrong = check_snapshot();
      if (!wrong.empty()) {
        failures.Add("reader " + std::to_string(r) + ", pass " + std::to_string(pass) + ", after " +
                     std::to_string(rounds_done.load()) + " rounds of writes: " + wrong);
        return;
      }
    }
  };

  // The number of table files below level 0. An iterator opens those when it comes to them, and
  // the tables of level 0 when it is made.
  auto files_below_level0 = [&] {
    int files = 0;
    for (int level = 1; level <= 6; ++level) {
      std::string count;
      if (db_->GetProperty("shale.num-files-at-level" + std::to_string(level), &count)) {
        files += ParseNumber<int>(count).value_or(0);
      }
    }
    return files;
  };

  // One more reader holds a scan open across compactions. It starts when the levels below 0 hold
  // two files at least, reads ten entries, which opens the first file of each level, and waits
  // until two compactions have finished: each is followed by a sweep that deletes the files that no
  // version holds. Then it reads on. The files it comes to now are opened only now, so a sweep that
  // took them while the scan still needed them would fail the scan.
  auto hold_scan_across_compactions = [&] {
    constexpr size_t kHeldAfter = 10;
    AwaitProgress(&failures, "a level below 0 to hold two files",
                  [&] { return files_below_level0() >= 2; });
    if (failures.Any()) {
      return;
    }
    const std::unique_ptr<Iterator> it = db_->NewIterator(at);
    Counters start;
    if (!ReadCounters(db_.get(), &start)) {
      failures.Add("held scan: could not read the counters: " + start.text);
      return;
    }
    size_t seen = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      if (seen >= kKeys || it->key() != keys[seen] || it->value() != at_snapshot[seen]) {
        failures.Add("held scan: entry " + std::to_string(seen) + " is " + std::string(it->key()) +
                     "=" + std::string(it->value()));
        return;
      }
      if (++seen == kHeldAfter) {
        AwaitProgress(&failures, "two compactions to finish while a scan is held", [&] {
          Counters now;
          return ReadCounters(db_.get(), &now) && now.compactions >= start.compactions + 2;
        });
        if (failures.Any()) {
          return;
        }
      }
    }
    if (!it->status().ok() || seen != kKeys) {
      failures.Add("held scan: read " + std::to_string(seen) + " of " + std::to_string(kKeys) +
                   " entries and ended with " + it->status().ToString());
    }
  };

  ThreadGroup writers;
  ThreadGroup readers;
  for (size_t r = 0; r < kReaders; ++r) {
    readers.Start([&, r] { read_at_snapshot(r); });
  }
  readers.Start(hold_scan_across_compactions);
  for (size_t w = 0; w < kWriters; ++w) {
    writers.Start([&, w] { overwrite(w); });
  }
  writers.Join();
  readers.Join();

  ASSERT_FALSE(failures.Any()) << failures.Report();
  ExpectBackgroundWork(baseline, 5);

  // A compaction of everything, with the snapshot still open, keeps the versions it can see.
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  EXPECT_EQ(check_snapshot(), "") << "after the compaction of the whole database";

  // And the newest state is what the last writer of each key left: one of the two last rounds.
  std::string value;
  for (size_t i = 0; i < kKeys; ++i) {
    ASSERT_OK(db_->Get(ReadOptions(), keys[i], &value));
    const std::string last = "-r" + std::to_string(kRounds - 1) + "-" + std::to_string(i);
    EXPECT_TRUE(value == "w0" + last || value == "w1" + last) << keys[i] << " is " << value;
  }
  snapshot.Release();
}

}  // namespace
}  // namespace shale
