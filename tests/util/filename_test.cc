#include "util/filename.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace shale {
namespace {

TEST(FileNameTest, Construct) {
  EXPECT_EQ(LogFileName("db", 7), "db/000007.log");
  EXPECT_EQ(TableFileName("db", 123456789), "db/123456789.sst");
  EXPECT_EQ(DescriptorFileName("db", 3), "db/MANIFEST-000003");
  EXPECT_EQ(CurrentFileName("db"), "db/CURRENT");
  EXPECT_EQ(LockFileName("db"), "db/LOCK");
  EXPECT_EQ(InfoLogFileName("db"), "db/LOG");
  EXPECT_EQ(OldInfoLogFileName("db"), "db/LOG.old");
  EXPECT_EQ(TempFileName("db", 9), "db/000009.dbtmp");
}

TEST(FileNameTest, NumbersArePaddedToSixDigitsAndNeverTruncated) {
  EXPECT_EQ(LogFileName("d", 0), "d/000000.log");
  EXPECT_EQ(LogFileName("d", 999999), "d/999999.log");
  EXPECT_EQ(LogFileName("d", 1000000), "d/1000000.log");
  EXPECT_EQ(TableFileName("d", std::numeric_limits<uint64_t>::max()), "d/18446744073709551615.sst");
  EXPECT_EQ(DescriptorFileName("d", 1234567), "d/MANIFEST-1234567");
  EXPECT_EQ(TempFileName("d", 1234567), "d/1234567.dbtmp");
}

TEST(FileNameTest, DirectoryNamesAreJoinedAsGiven) {
  EXPECT_EQ(CurrentFileName("/var/lib/shale"), "/var/lib/shale/CURRENT");
  EXPECT_EQ(LogFileName("a/b", 1), "a/b/000001.log");
}

struct Expected {
  std::string_view name;
  uint64_t number;
  FileType type;
};

TEST(FileNameTest, ParseAccepts) {
  const Expected cases[] = {
      {"000007.log", 7, FileType::kLogFile},
      {"123.sst", 123, FileType::kTableFile},
      {"MANIFEST-000003", 3, FileType::kDescriptorFile},
      {"CURRENT", 0, FileType::kCurrentFile},
      {"LOCK", 0, FileType::kLockFile},
      {"LOG", 0, FileType::kInfoLogFile},
      {"LOG.old", 0, FileType::kInfoLogFile},
      {"9.dbtmp", 9, FileType::kTempFile},
      {"18446744073709551615.log", std::numeric_limits<uint64_t>::max(), FileType::kLogFile},
      {"0.sst", 0, FileType::kTableFile},
      {"1234567.sst", 1234567, FileType::kTableFile},
      {"MANIFEST-18446744073709551615", std::numeric_limits<uint64_t>::max(),
       FileType::kDescriptorFile},
  };
  for (const Expected& c : cases) {
    uint64_t number = 12345;
    FileType type = FileType::kTempFile;
    ASSERT_TRUE(ParseFileName(c.name, &number, &type)) << c.name;
    EXPECT_EQ(number, c.number) << c.name;
    EXPECT_EQ(static_cast<int>(type), static_cast<int>(c.type)) << c.name;
  }
}

TEST(FileNameTest, ParseRejects) {
  const std::string_view cases[] = {
      "",
      "foo",
      "000007.log.bak",
      "MANIFEST-",
      "MANIFEST-abc",
      "18446744073709551616.log",
      ".log",
      "-1.sst",
      "7.LOG",
      "CURRENT.bak",
      "MANIFEST-18446744073709551616",
      "MANIFEST-1x",
      "MANIFEST--1",
      "MANIFEST",
      "LOG.OLD",
      "LOG.old.old",
      "LOCK.1",
      "current",
      "7.sst ",
      " 7.sst",
      "7 .sst",
      "+7.sst",
      "7.",
      "7",
      "7.tmp",
      "7.ldb",
      std::string_view("7.log\0", 6),
      std::string_view("CURRENT\0", 8),
  };
  for (std::string_view name : cases) {
    uint64_t number = 12345;
    FileType type = FileType::kTempFile;
    EXPECT_FALSE(ParseFileName(name, &number, &type))
        << "name of " << name.size() << " bytes: " << name;
    EXPECT_EQ(number, 12345u) << "a rejected name must not write its number";
    EXPECT_EQ(static_cast<int>(type), static_cast<int>(FileType::kTempFile));
  }
}

TEST(FileNameTest, ConstructedNamesParseBackToTheirNumberAndType) {
  const uint64_t numbers[] = {0, 1, 7, 999999, 1000000, std::numeric_limits<uint64_t>::max()};
  const auto basename = [](const std::string& path) { return path.substr(path.find('/') + 1); };
  for (uint64_t n : numbers) {
    const struct {
      std::string path;
      FileType type;
    } cases[] = {
        {LogFileName("db", n), FileType::kLogFile},
        {TableFileName("db", n), FileType::kTableFile},
        {DescriptorFileName("db", n), FileType::kDescriptorFile},
        {TempFileName("db", n), FileType::kTempFile},
    };
    for (const auto& c : cases) {
      uint64_t number = 0;
      FileType type = FileType::kCurrentFile;
      ASSERT_TRUE(ParseFileName(basename(c.path), &number, &type)) << c.path;
      EXPECT_EQ(number, n) << c.path;
      EXPECT_EQ(static_cast<int>(type), static_cast<int>(c.type)) << c.path;
    }
  }
  const struct {
    std::string path;
    FileType type;
  } fixed[] = {
      {CurrentFileName("db"), FileType::kCurrentFile},
      {LockFileName("db"), FileType::kLockFile},
      {InfoLogFileName("db"), FileType::kInfoLogFile},
      {OldInfoLogFileName("db"), FileType::kInfoLogFile},
  };
  for (const auto& c : fixed) {
    uint64_t number = 99;
    FileType type = FileType::kTempFile;
    ASSERT_TRUE(ParseFileName(basename(c.path), &number, &type)) << c.path;
    EXPECT_EQ(number, 0u) << c.path;
    EXPECT_EQ(static_cast<int>(type), static_cast<int>(c.type)) << c.path;
  }
}

}  // namespace
}  // namespace shale
