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
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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

TEST_F(DBPropertiesTest, CountersAreTheDatabaseCounters) {
  // Every field is made to count something, so that a field that is printed from the wrong member
  // shows: the writes, a flush, a compaction that drops a deletion and what it shadows, and a stop.
  Reopen(SmallBuffer());
  ASSERT_OK(Put("gone", "x"));
  ASSERT_OK(Put("k", "1"));
  ASSERT_OK(Put("k", "2"));
  ASSERT_OK(Delete("gone"));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  for (int i = 0; i < 60; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));  // fills the memtable: waits, and flushes
  }
  const DBCounters counters = dbfull()->TEST_Counters();
  ASSERT_GT(counters.flushes, 0u);
  ASSERT_GT(counters.compactions + counters.trivial_moves, 0u);
  ASSERT_GT(counters.user_bytes_written, 0u);
  ASSERT_GT(counters.wal_bytes_written, 0u);
  ASSERT_GT(counters.flush_bytes_written, 0u);
  ASSERT_GT(counters.write_groups, 0u);
  ASSERT_GT(counters.wal_syncs, 0u);
  ASSERT_GT(counters.write_stops, 0u);
  EXPECT_EQ(Property("shale.counters"), CountersText(dbfull()->TEST_Counters()));
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
  // One put of `u` bytes of key and value is a log record of 15 + u bytes, and nothing else is
  // written: the ratio is (15 + u) / u.
  EXPECT_EQ(AmplificationAfterOnePut(31, "a", "b"), "8.50");         // 17 / 2
  EXPECT_EQ(AmplificationAfterOnePut(32, "ab", "cde"), "4.00");      // 20 / 5
  EXPECT_EQ(AmplificationAfterOnePut(33, "abc", "defg"), "3.14");    // 22 / 7 = 3.1428
  EXPECT_EQ(AmplificationAfterOnePut(34, "abcd", "efghi"), "2.67");  // 24 / 9 = 2.6666
  EXPECT_EQ(AmplificationAfterOnePut(35, "", ""), "0.00") << "a put of no bytes: no ratio";
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
  std::atomic<bool> done{false};
  std::atomic<int> unreadable{0};
  std::atomic<int> reads{0};
  std::thread reader([&] {
    while (!done.load()) {
      for (const char* name : {"shale.num-files-at-level0", "shale.total-sst-bytes",
                               "shale.counters", "shale.write-amplification", "shale.stats",
                               "shale.approximate-memory-usage", "shale.background-error"}) {
        std::string value;
        if (!db_->GetProperty(name, &value) || value.empty()) {
          unreadable++;
        }
      }
      reads++;
    }
  });
  Status status;
  for (int i = 0; i < 300 && status.ok(); i++) {
    status = Put(UserKey(i), KiB(i), /*sync=*/false);
  }
  done.store(true);
  reader.join();
  ASSERT_OK(status);
  EXPECT_EQ(unreadable.load(), 0);
  EXPECT_GT(reads.load(), 0);
}

}  // namespace
}  // namespace shale
