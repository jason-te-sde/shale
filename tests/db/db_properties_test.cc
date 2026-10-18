// What DB::GetProperty() reports: the files at each level, the counters, the write amplification,
// the memory in use and the error that stopped writes.

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/version_state.h"
#include "db/db_impl.h"
#include "db/db_test_util.h"
#include "shale/db.h"
#include "shale/options.h"
#include "shale/status.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/filename.h"
#include "util/random.h"

namespace shale {
namespace {

using Op = FaultInjectionEnv::Op;

// The value of `text` if it is a decimal number and nothing else: digits, and at least one.
std::optional<uint64_t> ParseDecimal(const std::string& text) {
  if (text.empty() || text.size() > 19) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  return value;
}

// Every field of `counters` as the counters property prints them: in the order of the struct, which
// this spells out so that a property that drifts from it is caught here.
std::string CountersText(const DBCounters& c) {
  std::ostringstream out;
  out << "flushes=" << c.flushes << " compactions=" << c.compactions
      << " trivial_moves=" << c.trivial_moves << " tombstones_dropped=" << c.tombstones_dropped
      << " shadowed_dropped=" << c.shadowed_dropped
      << " user_bytes_written=" << c.user_bytes_written
      << " wal_bytes_written=" << c.wal_bytes_written
      << " flush_bytes_written=" << c.flush_bytes_written
      << " compaction_bytes_read=" << c.compaction_bytes_read
      << " compaction_bytes_written=" << c.compaction_bytes_written
      << " write_groups=" << c.write_groups << " wal_syncs=" << c.wal_syncs
      << " write_slowdowns=" << c.write_slowdowns << " write_stops=" << c.write_stops
      << " bg_errors=" << c.bg_errors;
  return out.str();
}

class DBPropertiesTest : public DBTestBase {
 protected:
  // The value of `name`, which the test requires to be a property.
  std::string Property(std::string_view name) {
    std::string value = "not set";
    EXPECT_TRUE(db_->GetProperty(name, &value)) << name << " is not a property";
    return value;
  }

  // Replaces the Env with a new one and a database in it, so that the counters are of what the test
  // does and not of a recovery.
  void StartOver(uint64_t seed) {
    Close();
    env_ = std::make_unique<FaultInjectionEnv>(seed);
    Reopen();
  }

  // The write amplification after a database that is new has been given one put, as text.
  std::string AmplificationAfterOnePut(uint64_t seed, std::string_view key,
                                       std::string_view value) {
    StartOver(seed);
    EXPECT_OK(Put(key, value));
    return Property("shale.write-amplification");
  }
};

TEST_F(DBPropertiesTest, UnknownPropertyIsFalse) {
  const std::vector<std::string> names = {
      "shale.nope",
      "shale.num-files-at-level7",
      "shale.num-files-at-level-1",
      "shale.num-files-at-levelx",
      "",
      "shale",
      "shale.",
      "nope",
      "num-files-at-level0",
      "leveldb.num-files-at-level0",
      "shale.num-files-at-level",
      "shale.num-files-at-level00",
      "shale.num-files-at-level01",
      "shale.num-files-at-level10",
      "shale.num-files-at-level+1",
      "shale.num-files-at-level 1",
      "shale.num-files-at-level1 ",
      "shale.num-files-at-level1x",
      "shale.num-files-at-level99999999999999999999",
      "shale.Counters",
      "shale.counters ",
      "shale.counters.",
      "shale.total-sst-bytes0",
      "SHALE.STATS",
  };
  for (const std::string& name : names) {
    std::string value = "untouched";
    EXPECT_FALSE(db_->GetProperty(name, &value)) << "\"" << name << "\"";
    EXPECT_EQ(value, "untouched") << "\"" << name << "\" changed the value it was refused";
  }
  const std::string with_nul("shale.counters\0", 15);
  std::string value;
  EXPECT_FALSE(db_->GetProperty(with_nul, &value));
}

TEST_F(DBPropertiesTest, EveryPropertyIsTrueAtOpen) {
  for (const char* name :
       {"shale.num-files-at-level0", "shale.num-files-at-level6", "shale.total-sst-bytes",
        "shale.counters", "shale.write-amplification", "shale.stats",
        "shale.approximate-memory-usage", "shale.background-error"}) {
    std::string value;
    EXPECT_TRUE(db_->GetProperty(name, &value)) << name;
  }
}

TEST_F(DBPropertiesTest, FilesAtLevel) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(Property("shale.num-files-at-level0"), "1");
  EXPECT_EQ(Property("shale.num-files-at-level1"), "0");
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(Property("shale.num-files-at-level0"), "2");
  // Compaction moves them to level 1, and every level that exists is one that can be asked.
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  EXPECT_EQ(Property("shale.num-files-at-level0"), "0");
  EXPECT_EQ(Property("shale.num-files-at-level1"), std::to_string(NumTableFilesAtLevel(1)));
  EXPECT_NE(Property("shale.num-files-at-level1"), "0");
  for (int level = 0; level < kNumLevels; level++) {
    EXPECT_EQ(Property("shale.num-files-at-level" + std::to_string(level)),
              std::to_string(NumTableFilesAtLevel(level)))
        << "level " << level;
  }
}

TEST_F(DBPropertiesTest, TotalSstBytesIsTheSizeOfTheTables) {
  EXPECT_EQ(Property("shale.total-sst-bytes"), "0");
  for (int i = 0; i < 50; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
  }
  ASSERT_OK(db_->Flush());
  uint64_t expected = 0;
  for (const std::vector<FileMetaData>& level : dbfull()->TEST_VersionState().files) {
    for (const FileMetaData& file : level) {
      uint64_t size = 0;
      ASSERT_OK(env_->GetFileSize(TableFileName(dbname_, file.number), &size));
      EXPECT_EQ(size, file.file_size);
      expected += size;
    }
  }
  ASSERT_GT(expected, 0u);
  EXPECT_EQ(Property("shale.total-sst-bytes"), std::to_string(expected));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  uint64_t after = 0;
  for (const std::vector<FileMetaData>& level : dbfull()->TEST_VersionState().files) {
    for (const FileMetaData& file : level) {
      after += file.file_size;
    }
  }
  EXPECT_EQ(Property("shale.total-sst-bytes"), std::to_string(after));
}

TEST_F(DBPropertiesTest, CountersListEveryFieldInOrder) {
  const std::string text = Property("shale.counters");
  const std::regex pattern(
      "^flushes=\\d+ compactions=\\d+ trivial_moves=\\d+ tombstones_dropped=\\d+ "
      "shadowed_dropped=\\d+ user_bytes_written=\\d+ wal_bytes_written=\\d+ "
      "flush_bytes_written=\\d+ compaction_bytes_read=\\d+ compaction_bytes_written=\\d+ "
      "write_groups=\\d+ wal_syncs=\\d+ write_slowdowns=\\d+ write_stops=\\d+ bg_errors=\\d+$");
  EXPECT_TRUE(std::regex_match(text, pattern)) << text;
}

TEST_F(DBPropertiesTest, EveryCounterIsPrintedFromItsOwnMember) {
  // Every counter is made to count, and to a value of its own: the writes, flushes and compactions
  // of a database that is also slowed, stopped and moved along by trivial moves, and then an error.
  // With no two of them equal and none zero, a name printed from the wrong member shows.
  Options options = SmallBuffer();
  options.l0_compaction_trigger = 3;
  options.l0_slowdown_trigger = 2;
  options.max_bytes_for_level_base = 65536;  // the smallest: a level 1 of 70 KiB is over it
  Reopen(options);
  // A deletion and what it hides, and an overwrite: a compaction drops all of it.
  ASSERT_OK(Put("dead1", "x"));
  ASSERT_OK(Put("dead2", "y"));
  ASSERT_OK(Put("k", "1"));
  ASSERT_OK(Put("k", "2"));
  ASSERT_OK(Delete("dead1"));
  ASSERT_OK(Delete("dead2"));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  // Two tables in level 0, which is the slowdown trigger: the writes that follow are slowed.
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Put("c", "3"));
  ASSERT_OK(Put("d", "4", /*sync=*/false));
  // Enough to fill the memtable twice with a flush that is not done: the write waits for it.
  for (int i = 0; i < 40; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
  }
  // Level 1 grows over its size, and a table of it moves down whole.
  for (int i = 0; i < 90; i++) {
    ASSERT_OK(Put(UserKey(1000 + i), KiB(i), /*sync=*/false));
  }
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  env_->FailOpAfter(Op::kSync, 0);
  ASSERT_TRUE(Put("lost", "1").IsIOError());

  const DBCounters c = dbfull()->TEST_Counters();
  const std::vector<std::pair<std::string, uint64_t>> values = {
      {"flushes", c.flushes},
      {"compactions", c.compactions},
      {"trivial_moves", c.trivial_moves},
      {"tombstones_dropped", c.tombstones_dropped},
      {"shadowed_dropped", c.shadowed_dropped},
      {"user_bytes_written", c.user_bytes_written},
      {"wal_bytes_written", c.wal_bytes_written},
      {"flush_bytes_written", c.flush_bytes_written},
      {"compaction_bytes_read", c.compaction_bytes_read},
      {"compaction_bytes_written", c.compaction_bytes_written},
      {"write_groups", c.write_groups},
      {"wal_syncs", c.wal_syncs},
      {"write_slowdowns", c.write_slowdowns},
      {"write_stops", c.write_stops},
      {"bg_errors", c.bg_errors},
  };
  std::set<uint64_t> seen;
  for (const auto& [name, value] : values) {
    EXPECT_NE(value, 0u) << name << " did not count: the test does not pin it";
    EXPECT_TRUE(seen.insert(value).second)
        << name << " = " << value << " is the value of another counter: the test does not pin it\n"
        << CountersText(c);
  }
  EXPECT_EQ(Property("shale.counters"), CountersText(c));
}

TEST_F(DBPropertiesTest, CountersFollowTheWrites) {
  StartOver(11);
  ASSERT_OK(Put("ab", "cde"));
  const std::string text = Property("shale.counters");
  EXPECT_NE(text.find("user_bytes_written=5 "), std::string::npos) << text;
  EXPECT_NE(text.find(" write_groups=1 "), std::string::npos) << text;
  EXPECT_NE(text.find(" wal_syncs=1 "), std::string::npos) << text;
  EXPECT_NE(text.find(" bg_errors=0"), std::string::npos) << text;
}

TEST_F(DBPropertiesTest, WriteAmplificationFormatting) {
  StartOver(21);
  EXPECT_EQ(Property("shale.write-amplification"), "0.00") << "no user bytes: no ratio";
  for (int i = 0; i < 1000; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
  }
  ASSERT_OK(db_->Flush());
  const std::string text = Property("shale.write-amplification");
  EXPECT_TRUE(std::regex_match(text, std::regex("^\\d+\\.\\d\\d$"))) << text;
  const double ratio = std::stod(text);
  EXPECT_GT(ratio, 1.0) << text;
  // The ratio of what was written to disk to what the callers wrote.
  const DBCounters c = dbfull()->TEST_Counters();
  const double expected = static_cast<double>(c.wal_bytes_written + c.flush_bytes_written +
                                              c.compaction_bytes_written) /
                          static_cast<double>(c.user_bytes_written);
  EXPECT_NEAR(ratio, expected, 0.0051) << text;
}

TEST_F(DBPropertiesTest, WriteAmplificationIsRoundedToTwoDecimals) {
  // One put of `u` bytes of key and value is a record of 15 + u bytes and a log header of 7 on
  // it, and nothing else is written: the ratio is (22 + u) / u.
  EXPECT_EQ(AmplificationAfterOnePut(31, "a", "b"), "12.00");            // 24 / 2
  EXPECT_EQ(AmplificationAfterOnePut(32, "a", "bc"), "8.33");            // 25 / 3 = 8.3333
  EXPECT_EQ(AmplificationAfterOnePut(33, "ab", "cdef"), "4.67");         // 28 / 6 = 4.6666
  EXPECT_EQ(AmplificationAfterOnePut(34, "abc", "defg"), "4.14");        // 29 / 7 = 4.1428
  EXPECT_EQ(AmplificationAfterOnePut(35, "abcdef", "ghijklm"), "2.69");  // 35 / 13 = 2.6923
  EXPECT_EQ(AmplificationAfterOnePut(36, "abcd", "efghi"), "3.44");      // 31 / 9 = 3.4444
  EXPECT_EQ(AmplificationAfterOnePut(37, "", ""), "0.00") << "a put of no bytes: no ratio";
}

TEST_F(DBPropertiesTest, WriteAmplificationCountsTheBytesOfTheLogFile) {
  // The log's share of the ratio is the size of its file, whatever the writes were: records of all
  // sizes, across blocks, with the headers of their fragments and the padding between blocks.
  StartOver(38);
  Random rnd(5);
  for (int i = 0; i < 40; i++) {
    const size_t size = i % 5 == 0 ? 40000 + rnd.Uniform(100) : 1 + rnd.Uniform(3000);
    ASSERT_OK(Put(UserKey(i), std::string(size, 'v'), /*sync=*/false));
  }
  const DBCounters c = dbfull()->TEST_Counters();
  ASSERT_EQ(c.flushes, 0u) << "no table: the ratio is the log's alone";
  ASSERT_EQ(c.wal_bytes_written, env_->FileContents(NewestLogFile(env_.get(), dbname_)).size());
  const double expected =
      static_cast<double>(c.wal_bytes_written) / static_cast<double>(c.user_bytes_written);
  EXPECT_NEAR(std::stod(Property("shale.write-amplification")), expected, 0.0051);
}

TEST_F(DBPropertiesTest, StatsHasOneLinePerLevel) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  const std::string text = Property("shale.stats");
  for (int level = 0; level < kNumLevels; level++) {
    EXPECT_NE(text.find("Level " + std::to_string(level) + ":"), std::string::npos)
        << "level " << level << " is not in\n"
        << text;
  }
  std::vector<std::string> lines;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  ASSERT_GE(lines.size(), static_cast<size_t>(kNumLevels) + 2) << text;
  EXPECT_EQ(lines[0].find("Level"), std::string::npos) << "the first line is a header";
  const VersionState state = dbfull()->TEST_VersionState();
  for (int level = 0; level < kNumLevels; level++) {
    uint64_t bytes = 0;
    for (const FileMetaData& file : state.files[static_cast<size_t>(level)]) {
      bytes += file.file_size;
    }
    EXPECT_EQ(lines[static_cast<size_t>(level) + 1],
              "Level " + std::to_string(level) + ": " +
                  std::to_string(state.files[static_cast<size_t>(level)].size()) + " files, " +
                  std::to_string(bytes) + " bytes");
  }
  EXPECT_NE(text.find(CountersText(dbfull()->TEST_Counters())), std::string::npos)
      << "the counters are in the stats\n"
      << text;
  EXPECT_EQ(text.back(), '\n') << "every line ends in a newline";
}

TEST_F(DBPropertiesTest, BackgroundErrorProperty) {
  EXPECT_EQ(Property("shale.background-error"), "OK");
  ASSERT_OK(Put("a", "1"));
  env_->FailOpAfter(Op::kSync, 0);
  const Status failed = Put("b", "2");
  ASSERT_TRUE(failed.IsIOError());
  const std::string text = Property("shale.background-error");
  EXPECT_EQ(text.rfind("IO error", 0), 0u) << text;
  EXPECT_EQ(text, failed.ToString()) << "it is the error that every write returns";
  env_->ClearFaults();
  Reopen();
  EXPECT_EQ(Property("shale.background-error"), "OK") << "opening again clears it";
}

TEST_F(DBPropertiesTest, MemoryUsageIsNumeric) {
  const std::optional<uint64_t> empty = ParseDecimal(Property("shale.approximate-memory-usage"));
  ASSERT_TRUE(empty.has_value()) << Property("shale.approximate-memory-usage");
  ASSERT_OK(Put("big", std::string(200 << 10, 'v'), /*sync=*/false));
  const std::optional<uint64_t> with_memtable =
      ParseDecimal(Property("shale.approximate-memory-usage"));
  ASSERT_TRUE(with_memtable.has_value());
  EXPECT_GE(*with_memtable, *empty + (200 << 10)) << "the memtable is counted";
}

TEST_F(DBPropertiesTest, MemoryUsageCountsTheBlockCache) {
  for (int i = 0; i < 100; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
  }
  ASSERT_OK(db_->Flush());
  Reopen();  // an empty cache, and the new memtable that a reopen makes
  const std::optional<uint64_t> before = ParseDecimal(Property("shale.approximate-memory-usage"));
  ASSERT_TRUE(before.has_value());
  ASSERT_EQ(Get(UserKey(7)), KiB(7));  // reads a data block of the table, and keeps it
  const std::optional<uint64_t> after = ParseDecimal(Property("shale.approximate-memory-usage"));
  ASSERT_TRUE(after.has_value());
  EXPECT_GE(*after, *before + 1024) << "the block that was read is in the cache, and is counted";
}

TEST_F(DBPropertiesTest, PropertiesCanBeReadWhileWriting) {
  Options options = SmallBuffer();
  options.scheduler = nullptr;
  Reopen(options);

  // The reader is up, and has read every property once, before the first write; and the writer
  // goes on only when the reader has read them all again since the last pause: reads and writes
  // alternate whichever thread the machine starts first, so that every property is read while the
  // database is changing under it.
  struct ReaderState {
    std::atomic<bool> stop{false};
    std::atomic<int> passes{0};
    std::atomic<int> unreadable{0};
  };
  const auto state = std::make_shared<ReaderState>();
  std::thread reader([this, state] {
    while (!state->stop.load()) {
      for (const char* name : {"shale.num-files-at-level0", "shale.total-sst-bytes",
                               "shale.counters", "shale.write-amplification", "shale.stats",
                               "shale.approximate-memory-usage", "shale.background-error"}) {
        std::string value;
        if (!db_->GetProperty(name, &value) || value.empty()) {
          state->unreadable++;
        }
      }
      state->passes++;
    }
  });
  // However the test ends, the reader is stopped and joined before what it reads is closed.
  struct JoinReader {
    ReaderState* state;
    std::thread* thread;
    ~JoinReader() {
      state->stop.store(true);
      thread->join();
    }
  } join_reader{state.get(), &reader};

  ASSERT_TRUE(PollUntil([&] { return state->passes.load() >= 1; })) << "the reader never read";
  Status status;
  for (int i = 0; i < 300 && status.ok(); i++) {
    status = Put(UserKey(i), KiB(i), /*sync=*/false);
    if (i % 25 == 24) {
      const int passes = state->passes.load();
      ASSERT_TRUE(PollUntil([&] { return state->passes.load() > passes; }))
          << "the reader stopped reading while the writer wrote";
    }
  }
  ASSERT_OK(status);
  EXPECT_EQ(state->unreadable.load(), 0);
  EXPECT_GE(state->passes.load(), 12) << "at least one read of everything for every 25 writes";
  EXPECT_GE(dbfull()->TEST_Counters().flushes, 1u) << "the memtable was switched under the reads";
}

// A null value aborts, so these run in a child process.
class DBPropertiesDeathTest : public DBPropertiesTest {};

TEST_F(DBPropertiesDeathTest, ANullValueAbortsWhateverTheName) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(static_cast<void>(db_->GetProperty("shale.counters", nullptr)),
               "somewhere to put the value");
  EXPECT_DEATH(static_cast<void>(db_->GetProperty("shale.nope", nullptr)),
               "somewhere to put the value");
}

}  // namespace
}  // namespace shale
