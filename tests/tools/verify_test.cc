// Tests of the offline check behind `shalectl verify`. The database is on the in-memory Env, so
// that damage is made by changing the bytes of a file and the check is compared with the whole
// directory before and after it.

#include "verify.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "db/builder.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "io/env_util.h"
#include "io/iterator_util.h"
#include "io/table_builder.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/filename.h"

namespace shale {
namespace {

constexpr char kDb[] = "/db";

bool Contains(const std::string& text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

// ---- CheckTableEntries: the rules for the entries of one table. -----------------------------

// The recorded range of a table, as the MANIFEST keeps it.
FileMetaData Range(std::string_view smallest, SequenceNumber smallest_sequence,
                   std::string_view largest, SequenceNumber largest_sequence) {
  FileMetaData file;
  file.number = 7;
  file.file_size = 1000;
  file.smallest = IKey(smallest, smallest_sequence);
  file.largest = IKey(largest, largest_sequence);
  return file;
}

constexpr char kPath[] = "/db/000007.sst";

// What CheckTableEntries() says of an iterator over `entries`, which are taken in the order given
// whether or not it is the right one.
Status Check(Entries entries, const FileMetaData& file) {
  const std::unique_ptr<Iterator> iter = NewVectorIterator(std::move(entries));
  return CheckTableEntries(iter.get(), file, kPath);
}

TEST(CheckTableEntriesTest, AcceptsEntriesInOrderAndInsideTheRange) {
  EXPECT_OK(Check({{IKey("b", 2), "x"}, {IKey("c", 3), "y"}, {IKey("d", 4), "z"}},
                  Range("b", 2, "d", 4)));
}

TEST(CheckTableEntriesTest, AcceptsARecordedRangeWiderThanTheEntries) {
  // The MANIFEST may say more than the table holds: only an entry outside the range is a lie that a
  // read can fall for, because it skips a table whose range does not cover the key.
  EXPECT_OK(Check({{IKey("c", 3), "y"}}, Range("a", 1, "z", 9)));
}

TEST(CheckTableEntriesTest, AcceptsATableWithNoEntries) {
  EXPECT_OK(Check({}, Range("a", 1, "z", 9)));
}

TEST(CheckTableEntriesTest, AcceptsEntriesAtBothEndsOfTheRange) {
  EXPECT_OK(Check({{IKey("a", 1), "x"}, {IKey("z", 9), "y"}}, Range("a", 1, "z", 9)));
}

TEST(CheckTableEntriesTest, SeveralVersionsOfOneKeyAreInOrderNewestFirst) {
  EXPECT_OK(Check(
      {{IKey("k", 9), "new"}, {IKey("k", 5, ValueType::kDeletion), ""}, {IKey("k", 2), "old"}},
      Range("k", 9, "k", 2)));
}

TEST(CheckTableEntriesTest, RejectsAnEntryBelowTheSmallestKey) {
  const Status s = Check({{IKey("a", 1), "x"}, {IKey("c", 3), "y"}}, Range("b", 2, "d", 4));
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "smallest")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), kPath)) << s.ToString();
}

TEST(CheckTableEntriesTest, RejectsTheSameUserKeyNewerThanTheSmallestKey) {
  // Inside the user-key range, but the table's smallest key is the entry at sequence 2, and the
  // entry at 5 sorts before it.
  const Status s = Check({{IKey("b", 5), "x"}}, Range("b", 2, "d", 4));
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "smallest")) << s.ToString();
}

TEST(CheckTableEntriesTest, RejectsAnEntryAboveTheLargestKey) {
  const Status s = Check({{IKey("b", 2), "x"}, {IKey("e", 5), "y"}}, Range("b", 2, "d", 4));
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "largest")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), kPath)) << s.ToString();
}

TEST(CheckTableEntriesTest, RejectsEntriesOutOfOrder) {
  const Status s = Check({{IKey("c", 3), "y"}, {IKey("b", 2), "x"}}, Range("b", 2, "d", 4));
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "not after")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), kPath)) << s.ToString();
}

TEST(CheckTableEntriesTest, RejectsOlderVersionBeforeNewerOfOneKey) {
  const Status s = Check({{IKey("k", 2), "old"}, {IKey("k", 9), "new"}}, Range("k", 9, "k", 2));
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "not after")) << s.ToString();
}

TEST(CheckTableEntriesTest, RejectsAnEntryRepeated) {
  const Status s = Check({{IKey("b", 2), "x"}, {IKey("b", 2), "x"}}, Range("b", 2, "d", 4));
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "not after")) << s.ToString();
}

TEST(CheckTableEntriesTest, RejectsAKeyThatIsNotAnInternalKey) {
  const Status too_short = Check({{"short", "x"}}, Range("b", 2, "d", 4));
  ASSERT_TRUE(too_short.IsCorruption()) << too_short.ToString();
  EXPECT_TRUE(Contains(too_short.ToString(), "internal key")) << too_short.ToString();

  // Eight bytes of tag whose type byte is no type.
  std::string unknown_type = IKey("c", 3);
  unknown_type[unknown_type.size() - 8] = '\x7f';
  const Status bad_type = Check({{unknown_type, "x"}}, Range("b", 2, "d", 4));
  ASSERT_TRUE(bad_type.IsCorruption()) << bad_type.ToString();
  EXPECT_TRUE(Contains(bad_type.ToString(), "internal key")) << bad_type.ToString();
}

TEST(CheckTableEntriesTest, ReturnsTheErrorOfAnIteratorThatStopsEarly) {
  // The iterator fails after the entries it has, which a reader that looks only at Valid() would
  // take for the end of the table.
  const std::unique_ptr<Iterator> iter = NewFailingIterator(
      {{IKey("b", 2), "x"}, {IKey("c", 3), "y"}}, Status::Corruption("bad block", kPath));
  const Status s = CheckTableEntries(iter.get(), Range("b", 2, "d", 4), kPath);
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "bad block")) << s.ToString();
}

TEST(CheckTableEntriesTest, NamesTheTableInTheErrorOfAnIteratorThatDoesNot) {
  // A block of a table that fails its checksum says so and not where, since the block does not
  // know its file; the check is what knows.
  const std::unique_ptr<Iterator> iter =
      NewFailingIterator({{IKey("b", 2), "x"}}, Status::Corruption("block checksum mismatch"));
  const Status s = CheckTableEntries(iter.get(), Range("b", 2, "d", 4), kPath);
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "block checksum mismatch")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), kPath)) << s.ToString();
  // Once, not twice.
  EXPECT_EQ(s.ToString().find(kPath), s.ToString().rfind(kPath)) << s.ToString();
}

TEST(CheckTableEntriesTest, RejectsARecordedRangeThatIsNotMadeOfInternalKeys) {
  FileMetaData file = Range("b", 2, "d", 4);
  file.smallest = "short";
  Status s = Check({{IKey("c", 3), "y"}}, file);
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "MANIFEST")) << s.ToString();

  file = Range("b", 2, "d", 4);
  file.largest = "short";
  s = Check({{IKey("c", 3), "y"}}, file);
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "MANIFEST")) << s.ToString();
}

TEST(CheckTableEntriesTest, ReturnsTheErrorOfAnIteratorThatNeverStarted) {
  const std::unique_ptr<Iterator> iter = NewErrorIterator(Status::IOError("gone", kPath));
  const Status s = CheckTableEntries(iter.get(), Range("b", 2, "d", 4), kPath);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
}

// ---- VerifyDatabase: a whole database directory. ---------------------------------------------

class VerifyTest : public testing::Test {
 protected:
  VerifyTest() : env_(NewMemEnv()) {}

  Options DbOptions() const {
    Options options;
    options.env = env_.get();
    options.create_if_missing = true;
    return options;
  }

  // Creates the database with `tables` tables in it, flushed one at a time, with some keys
  // overwritten and removed, and closes it. A fresh database each call is not made: call it once.
  void MakeDatabase(int tables) {
    std::unique_ptr<DB> db;
    ASSERT_OK(DB::Open(DbOptions(), kDb, &db));
    for (int table = 0; table < tables; table++) {
      for (int i = 0; i < 40; i++) {
        const int key = table * 30 + i;  // each table overlaps the one before it
        ASSERT_OK(db->Put(WriteOptions(), UserKey(key), ValueOf(key + table)));
      }
      ASSERT_OK(db->Delete(WriteOptions(), UserKey(table * 30 + 3)));
      ASSERT_OK(db->Flush());
    }
  }

  std::string PathOf(const std::string& name) const { return std::string(kDb) + "/" + name; }

  // The names of the files of one type, in the directory, in number order.
  std::vector<std::string> NamesOf(FileType wanted) const {
    std::vector<std::string> children;
    EXPECT_OK(env_->GetChildren(kDb, &children));
    std::vector<std::pair<uint64_t, std::string>> found;
    for (const std::string& name : children) {
      uint64_t number = 0;
      FileType type = FileType::kCurrentFile;
      if (ParseFileName(name, &number, &type) && type == wanted) {
        found.emplace_back(number, name);
      }
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> names;
    for (const auto& entry : found) {
      names.push_back(entry.second);
    }
    return names;
  }

  // Every file of the directory with its bytes: what must not change under a check.
  std::map<std::string, std::string> DirectoryContents() const {
    std::vector<std::string> children;
    EXPECT_OK(env_->GetChildren(kDb, &children));
    std::map<std::string, std::string> contents;
    for (const std::string& name : children) {
      std::string data;
      EXPECT_OK(ReadFileToString(env_.get(), PathOf(name), &data));
      contents[name] = data;
    }
    return contents;
  }

  // Changes the byte of `path` at `offset`, or at the middle of the file for the maximum value.
  void FlipByte(const std::string& path, size_t offset = SIZE_MAX) {
    std::string data;
    ASSERT_OK(ReadFileToString(env_.get(), path, &data));
    ASSERT_FALSE(data.empty());
    if (offset == SIZE_MAX) {
      offset = data.size() / 2;
    }
    ASSERT_LT(offset, data.size());
    data[offset] = static_cast<char>(data[offset] ^ 0x40);
    ASSERT_OK(WriteStringToFile(env_.get(), data, path, false));
  }

  // Checks the database, which must be one that can be checked, and returns what was found.
  std::vector<std::string> Problems() {
    std::vector<std::string> problems{"left over from before"};
    EXPECT_OK(VerifyDatabase(env_.get(), kDb, &problems));
    return problems;
  }

  // Makes a database of one table that holds the user keys "a" through "e", at sequence numbers 1
  // through 5, and whose MANIFEST records the range from `smallest` to `largest` for it. The
  // database has no LOCK, since no database was opened to make it.
  void MakeDatabaseWithRecordedRange(std::string_view smallest, std::string_view largest) {
    ASSERT_OK(env_->CreateDir(kDb));
    TableCache table_cache(kDb, env_.get(), nullptr, 16);
    VersionSet versions(kDb, env_.get(), &table_cache);
    ASSERT_OK(versions.CreateNew());
    FileMetaData file;
    file.number = versions.NewFileNumber();
    const std::unique_ptr<Iterator> entries = NewVectorIterator({{IKey("a", 1), "1"},
                                                                 {IKey("b", 2), "2"},
                                                                 {IKey("c", 3), "3"},
                                                                 {IKey("d", 4), "4"},
                                                                 {IKey("e", 5), "5"}});
    ASSERT_OK(
        BuildTable(kDb, env_.get(), TableBuilderOptions(), &table_cache, entries.get(), &file));
    ASSERT_GT(file.file_size, 0U);
    file.smallest = IKey(smallest, 1);
    file.largest = IKey(largest, 5);
    VersionEdit edit;
    edit.new_files.emplace_back(0, file);
    ASSERT_OK(versions.LogAndApply(edit));
  }

  std::unique_ptr<Env> env_;
};

TEST_F(VerifyTest, ADatabaseOfSeveralTablesHasNoProblems) {
  MakeDatabase(3);
  ASSERT_EQ(NamesOf(FileType::kTableFile).size(), 3U);
  EXPECT_TRUE(Problems().empty());
}

TEST_F(VerifyTest, ACompactedDatabaseHasNoProblems) {
  MakeDatabase(3);
  {
    std::unique_ptr<DB> db;
    ASSERT_OK(DB::Open(DbOptions(), kDb, &db));
    ASSERT_OK(db->CompactRange(std::nullopt, std::nullopt));
    std::string files;
    ASSERT_TRUE(db->GetProperty("shale.num-files-at-level1", &files));
    ASSERT_NE(files, "0");
  }
  EXPECT_TRUE(Problems().empty());
}

TEST_F(VerifyTest, ADatabaseWithNoTablesHasNoProblems) {
  MakeDatabase(0);
  EXPECT_TRUE(NamesOf(FileType::kTableFile).empty());
  EXPECT_TRUE(Problems().empty());
}

TEST_F(VerifyTest, ChangesNoFileOfAHealthyDatabase) {
  MakeDatabase(3);
  const auto before = DirectoryContents();
  EXPECT_TRUE(Problems().empty());
  // The same names with the same bytes: no new MANIFEST, no new log, nothing rewritten.
  EXPECT_EQ(DirectoryContents(), before);
}

TEST_F(VerifyTest, ChangesNoFileOfADamagedDatabase) {
  MakeDatabase(3);
  FlipByte(PathOf(NamesOf(FileType::kTableFile).front()));
  const auto before = DirectoryContents();
  EXPECT_FALSE(Problems().empty());
  EXPECT_EQ(DirectoryContents(), before);

  FlipByte(PathOf(NamesOf(FileType::kDescriptorFile).back()));
  const auto before_manifest = DirectoryContents();
  EXPECT_FALSE(Problems().empty());
  EXPECT_EQ(DirectoryContents(), before_manifest);
}

TEST_F(VerifyTest, TakesTheLockAndGivesItBack) {
  MakeDatabase(1);
  EXPECT_TRUE(Problems().empty());
  // Had the check kept the lock, this open would fail.
  std::unique_ptr<DB> db;
  EXPECT_OK(DB::Open(DbOptions(), kDb, &db));
}

TEST_F(VerifyTest, RefusesADatabaseThatIsOpen) {
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(DbOptions(), kDb, &db));
  ASSERT_OK(db->Put(WriteOptions(), "a", "1"));

  std::vector<std::string> problems{"left over from before"};
  const Status s = VerifyDatabase(env_.get(), kDb, &problems);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "LOCK")) << s.ToString();
  EXPECT_TRUE(problems.empty());

  // The database that holds the lock is as it was.
  std::string value;
  EXPECT_OK(db->Get(ReadOptions(), "a", &value));
  EXPECT_EQ(value, "1");
}

TEST_F(VerifyTest, RefusesAPathWithNoDatabaseAndCreatesNothing) {
  std::vector<std::string> problems{"left over from before"};
  Status s = VerifyDatabase(env_.get(), kDb, &problems);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), kDb)) << s.ToString();
  EXPECT_TRUE(problems.empty());
  EXPECT_FALSE(env_->FileExists(kDb));

  // A directory that is there and empty is no database either, and gets no LOCK.
  ASSERT_OK(env_->CreateDir(kDb));
  s = VerifyDatabase(env_.get(), kDb, &problems);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_FALSE(env_->FileExists(LockFileName(kDb)));
  std::vector<std::string> children;
  ASSERT_OK(env_->GetChildren(kDb, &children));
  EXPECT_TRUE(children.empty());
}

TEST_F(VerifyTest, ReportsADamagedTableByName) {
  MakeDatabase(3);
  const std::vector<std::string> tables = NamesOf(FileType::kTableFile);
  FlipByte(PathOf(tables[1]));

  const std::vector<std::string> problems = Problems();
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_TRUE(Contains(problems[0], "Corruption")) << problems[0];
  EXPECT_TRUE(Contains(problems[0], tables[1])) << problems[0];
}

TEST_F(VerifyTest, ReportsOneLineForEachTableThatIsWrong) {
  MakeDatabase(3);
  const std::vector<std::string> tables = NamesOf(FileType::kTableFile);
  FlipByte(PathOf(tables[0]));
  ASSERT_OK(env_->RemoveFile(PathOf(tables[2])));

  const std::vector<std::string> problems = Problems();
  ASSERT_EQ(problems.size(), 2U);
  EXPECT_TRUE(Contains(problems[0], tables[0])) << problems[0];
  EXPECT_TRUE(Contains(problems[1], tables[2])) << problems[1];
}

TEST_F(VerifyTest, ReportsAMissingTable) {
  MakeDatabase(2);
  const std::vector<std::string> tables = NamesOf(FileType::kTableFile);
  ASSERT_OK(env_->RemoveFile(PathOf(tables[0])));

  const std::vector<std::string> problems = Problems();
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_TRUE(Contains(problems[0], tables[0])) << problems[0];
}

TEST_F(VerifyTest, ReportsATableWhoseSizeIsNotTheRecordedOne) {
  MakeDatabase(2);
  const std::vector<std::string> tables = NamesOf(FileType::kTableFile);
  // Bytes after the footer: every block and the footer are where they were, so reading the table
  // as the MANIFEST sizes it finds nothing wrong. Only the size gives it away.
  std::string data;
  ASSERT_OK(ReadFileToString(env_.get(), PathOf(tables[1]), &data));
  data.append("trailing bytes");
  ASSERT_OK(WriteStringToFile(env_.get(), data, PathOf(tables[1]), false));

  const std::vector<std::string> problems = Problems();
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_TRUE(Contains(problems[0], tables[1])) << problems[0];
  EXPECT_TRUE(Contains(problems[0], "size")) << problems[0];
}

TEST_F(VerifyTest, ReportsATruncatedTable) {
  MakeDatabase(2);
  const std::vector<std::string> tables = NamesOf(FileType::kTableFile);
  std::string data;
  ASSERT_OK(ReadFileToString(env_.get(), PathOf(tables[0]), &data));
  data.resize(data.size() / 2);
  ASSERT_OK(WriteStringToFile(env_.get(), data, PathOf(tables[0]), false));

  const std::vector<std::string> problems = Problems();
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_TRUE(Contains(problems[0], tables[0])) << problems[0];
}

TEST_F(VerifyTest, ReportsADamagedManifestAndStopsThere) {
  MakeDatabase(3);
  const std::string manifest = NamesOf(FileType::kDescriptorFile).back();
  FlipByte(PathOf(manifest));
  // A table is damaged as well, but with no MANIFEST there are no tables to look at.
  FlipByte(PathOf(NamesOf(FileType::kTableFile).front()));

  const std::vector<std::string> problems = Problems();
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_TRUE(Contains(problems[0], "Corruption")) << problems[0];
  EXPECT_TRUE(Contains(problems[0], manifest)) << problems[0];
}

TEST_F(VerifyTest, ReportsAMissingManifest) {
  MakeDatabase(1);
  const std::string manifest = NamesOf(FileType::kDescriptorFile).back();
  ASSERT_OK(env_->RemoveFile(PathOf(manifest)));

  const std::vector<std::string> problems = Problems();
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_TRUE(Contains(problems[0], manifest)) << problems[0];
}

TEST_F(VerifyTest, ReportsADamagedCurrent) {
  MakeDatabase(1);
  ASSERT_OK(WriteStringToFile(env_.get(), "garbage\n", CurrentFileName(kDb), false));

  const std::vector<std::string> problems = Problems();
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_TRUE(Contains(problems[0], "Corruption")) << problems[0];
  EXPECT_TRUE(Contains(problems[0], "CURRENT")) << problems[0];
}

TEST_F(VerifyTest, AcceptsARecordedRangeThatIsTheTablesOwn) {
  MakeDatabaseWithRecordedRange("a", "e");
  EXPECT_TRUE(Problems().empty());
}

TEST_F(VerifyTest, AcceptsARecordedRangeWiderThanTheTables) {
  MakeDatabaseWithRecordedRange("a", "z");
  EXPECT_TRUE(Problems().empty());
}

TEST_F(VerifyTest, ReportsATableWithAnEntryAboveItsRecordedRange) {
  MakeDatabaseWithRecordedRange("a", "c");
  const std::vector<std::string> problems = Problems();
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_TRUE(Contains(problems[0], "largest")) << problems[0];
  EXPECT_TRUE(Contains(problems[0], ".sst")) << problems[0];
}

TEST_F(VerifyTest, ReportsATableWithAnEntryBelowItsRecordedRange) {
  MakeDatabaseWithRecordedRange("c", "e");
  const std::vector<std::string> problems = Problems();
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_TRUE(Contains(problems[0], "smallest")) << problems[0];
}

}  // namespace
}  // namespace shale
