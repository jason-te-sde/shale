// The iterator over the database: the user view that NewDBIterator() makes of internal entries,
// and DB::NewIterator() that merges every source of a database under it and pins what it reads.

#include "db/db_iter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/version_state.h"
#include "db/db_impl.h"
#include "db/db_test_util.h"
#include "io/iterator_util.h"
#include "io/memtable.h"
#include "io/table.h"
#include "shale/db.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/filename.h"

namespace shale {
namespace {

// Every entry of `it`, from SeekToFirst() to the end.
Entries Scan(Iterator* it) {
  Entries entries;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    entries.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  return entries;
}

// Every entry of `it` from the first key that is not less than `target`.
Entries ScanFrom(Iterator* it, std::string_view target) {
  Entries entries;
  for (it->Seek(target); it->Valid(); it->Next()) {
    entries.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  return entries;
}

// The entries of `model`, in the order a scan returns them: std::string orders by unsigned bytes.
Entries Expected(const std::map<std::string, std::string>& model) {
  return Entries(model.begin(), model.end());
}

// ---- NewDBIterator(), over a memtable, which is a source of internal keys in the right order.

class DBIterUnitTest : public ::testing::Test {
 protected:
  void Put(SequenceNumber seq, std::string_view key, std::string_view value) {
    mem_->Add(seq, ValueType::kValue, key, value);
  }

  void Del(SequenceNumber seq, std::string_view key) {
    mem_->Add(seq, ValueType::kDeletion, key, "");
  }

  std::unique_ptr<Iterator> At(SequenceNumber sequence) {
    return NewDBIterator(mem_->NewIterator(), sequence);
  }

  // a: written at 1, 4 and 7; b: at 2 and 9; c: put at 3, deleted at 6.
  void FillWithThreeKeys() {
    Put(1, "a", "a1");
    Put(4, "a", "a4");
    Put(7, "a", "a7");
    Put(2, "b", "b2");
    Put(9, "b", "b9");
    Put(3, "c", "c3");
    Del(6, "c");
  }

  const std::shared_ptr<MemTable> mem_ = std::make_shared<MemTable>(1);
};

TEST_F(DBIterUnitTest, ShowsTheNewestEntryAtOrBelowTheSequence) {
  FillWithThreeKeys();
  EXPECT_EQ(Scan(At(0).get()), Entries{}) << "no write is numbered 0";
  EXPECT_EQ(Scan(At(1).get()), (Entries{{"a", "a1"}}));
  EXPECT_EQ(Scan(At(2).get()), (Entries{{"a", "a1"}, {"b", "b2"}}));
  EXPECT_EQ(Scan(At(3).get()), (Entries{{"a", "a1"}, {"b", "b2"}, {"c", "c3"}}));
  EXPECT_EQ(Scan(At(5).get()), (Entries{{"a", "a4"}, {"b", "b2"}, {"c", "c3"}}));
  EXPECT_EQ(Scan(At(6).get()), (Entries{{"a", "a4"}, {"b", "b2"}})) << "c is deleted at 6";
  EXPECT_EQ(Scan(At(8).get()), (Entries{{"a", "a7"}, {"b", "b2"}}));
  EXPECT_EQ(Scan(At(9).get()), (Entries{{"a", "a7"}, {"b", "b9"}}));
  EXPECT_EQ(Scan(At(kMaxSequenceNumber).get()), (Entries{{"a", "a7"}, {"b", "b9"}}));
}

TEST_F(DBIterUnitTest, ADeletionHidesEveryOlderEntryOfItsKey) {
  Put(1, "k", "v1");
  Put(2, "k", "v2");
  Del(3, "k");
  Put(4, "z", "last");
  EXPECT_EQ(Scan(At(2).get()), (Entries{{"k", "v2"}}));
  EXPECT_EQ(Scan(At(3).get()), Entries{});
  EXPECT_EQ(Scan(At(4).get()), (Entries{{"z", "last"}}));
}

TEST_F(DBIterUnitTest, ADeletionWithNothingUnderItHidesNothingElse) {
  Del(1, "b");
  Put(2, "a", "1");
  Put(3, "c", "3");
  EXPECT_EQ(Scan(At(3).get()), (Entries{{"a", "1"}, {"c", "3"}}));
}

TEST_F(DBIterUnitTest, AKeyReturnsAfterItIsDeletedAndWrittenAgain) {
  Put(1, "k", "old");
  Del(2, "k");
  Put(3, "k", "new");
  EXPECT_EQ(Scan(At(1).get()), (Entries{{"k", "old"}}));
  EXPECT_EQ(Scan(At(2).get()), Entries{});
  EXPECT_EQ(Scan(At(3).get()), (Entries{{"k", "new"}}));
}

TEST_F(DBIterUnitTest, SeekTakesAUserKeyAndFindsTheNewestVersionItSees) {
  FillWithThreeKeys();
  const std::unique_ptr<Iterator> it = At(5);
  EXPECT_EQ(ScanFrom(it.get(), ""), (Entries{{"a", "a4"}, {"b", "b2"}, {"c", "c3"}}));
  EXPECT_EQ(ScanFrom(it.get(), "a"), (Entries{{"a", "a4"}, {"b", "b2"}, {"c", "c3"}}));
  EXPECT_EQ(ScanFrom(it.get(), "aa"), (Entries{{"b", "b2"}, {"c", "c3"}}));
  EXPECT_EQ(ScanFrom(it.get(), "b"), (Entries{{"b", "b2"}, {"c", "c3"}}));
  EXPECT_EQ(ScanFrom(it.get(), "c"), (Entries{{"c", "c3"}}));
  EXPECT_EQ(ScanFrom(it.get(), "d"), Entries{});
}

TEST_F(DBIterUnitTest, SeekToADeletedKeyLandsOnTheNextLiveOne) {
  Put(1, "a", "1");
  Put(2, "b", "2");
  Del(3, "b");
  Put(4, "c", "4");
  const std::unique_ptr<Iterator> it = At(4);
  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "c");
  EXPECT_EQ(it->value(), "4");
}

TEST_F(DBIterUnitTest, SeekToAKeyWhoseEntriesAreAllNewerSkipsIt) {
  Put(1, "a", "1");
  Put(9, "b", "9");
  Put(2, "c", "2");
  const std::unique_ptr<Iterator> it = At(5);
  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "c");
}

TEST_F(DBIterUnitTest, ASeekMayGoBackwardsAndAFinishedScanMayStartOver) {
  Put(1, "a", "1");
  Put(2, "b", "2");
  Put(3, "c", "3");
  const std::unique_ptr<Iterator> it = At(3);
  it->Seek("c");
  ASSERT_TRUE(it->Valid());
  it->Seek("a");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "a");
  it->Seek("zz");
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
  EXPECT_EQ(Scan(it.get()).size(), 3u) << "SeekToFirst() after the end";
}

TEST_F(DBIterUnitTest, ValuesMayBeEmptyOrBinary) {
  Put(1, "empty", "");
  Put(2, "zeros", std::string("a\0b\0", 4));
  const Entries scanned = Scan(At(2).get());
  ASSERT_EQ(scanned.size(), 2u);
  EXPECT_EQ(scanned[0], (std::pair<std::string, std::string>{"empty", ""}));
  EXPECT_EQ(scanned[1].second, std::string("a\0b\0", 4));
}

TEST_F(DBIterUnitTest, AnEmptyIteratorIsNotValidAndNotAnError) {
  const std::unique_ptr<Iterator> it = At(5);
  EXPECT_FALSE(it->Valid()) << "before any seek";
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
  it->Seek("a");
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

// A key shorter than the 8-byte tag.
const char kShortKey[] = "abc";

// The internal key for `user_key` at 1, with its type byte set to a value that is no type.
std::string KeyWithBadType(std::string_view user_key) {
  std::string key = IKey(user_key, 1);
  key[key.size() - kInternalKeyTagSize] = '\x7f';  // the type is the low byte of the tag
  return key;
}

TEST(DBIterCorruptionTest, AKeyShorterThanTheTagEndsTheIterationWithCorruption) {
  const std::unique_ptr<Iterator> it = NewDBIterator(
      NewVectorIterator({{IKey("a", 1), "1"}, {kShortKey, "x"}, {IKey("z", 2), "2"}}), 10);
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "a");
  EXPECT_OK(it->status());

  it->Next();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
  EXPECT_NE(it->status().ToString().find("corrupted internal key in DB iterator"),
            std::string::npos)
      << it->status().ToString();
}

TEST(DBIterCorruptionTest, ATypeThatDoesNotExistEndsTheIterationWithCorruption) {
  const std::unique_ptr<Iterator> it =
      NewDBIterator(NewVectorIterator({{KeyWithBadType("a"), "1"}, {IKey("b", 1), "2"}}), 10);
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid()) << "the key is not skipped, and the one after it is not returned";
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
}

TEST(DBIterCorruptionTest, ACorruptKeyAmongTheOlderEntriesIsFoundWhileSkippingThem) {
  // It is an older entry of "a", which the scan has to read past to get to "b".
  const std::unique_ptr<Iterator> it = NewDBIterator(
      NewVectorIterator({{IKey("a", 5), "new"}, {KeyWithBadType("a"), "old"}, {IKey("b", 1), "b"}}),
      10);
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  it->Next();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
}

TEST(DBIterCorruptionTest, ACorruptionIsStuckAndASeekDoesNotStepOverIt) {
  const std::unique_ptr<Iterator> it =
      NewDBIterator(NewVectorIterator({{IKey("a", 1), "1"}, {kShortKey, "x"}}), 10);
  it->SeekToFirst();
  it->Next();
  ASSERT_TRUE(it->status().IsCorruption());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
  it->Seek("a");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(DBIterCorruptionTest, ACorruptKeyIsFoundWhateverTheSequenceIs) {
  // A corrupt key is a corrupt key whatever its sequence number would have been: it cannot be
  // known, so it is never taken for an entry that is above it and does not matter.
  const std::unique_ptr<Iterator> it =
      NewDBIterator(NewVectorIterator({{kShortKey, "x"}, {IKey("a", 1), "1"}}), 0);
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(DBIterCorruptionTest, AFailureOfTheInternalIteratorIsReportedAtTheEnd) {
  const std::unique_ptr<Iterator> it = NewDBIterator(
      NewFailingIterator({{IKey("a", 1), "1"}, {IKey("b", 2), "2"}}, Status::IOError("disk")), 10);
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_OK(it->status());
  it->Next();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsIOError())
      << "a failure is not the end of the data: " << it->status().ToString();
}

TEST(DBIterCorruptionTest, AnInternalIteratorThatNeverStartedReportsItsError) {
  const std::unique_ptr<Iterator> it =
      NewDBIterator(NewErrorIterator(Status::Corruption("a table")), 10);
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
  it->Seek("k");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(DBIterCorruptionTest, AFailureWhileSkippingHiddenEntriesIsNotTakenForTheEnd) {
  // After "a" there are only older entries of it to skip, and the source fails among them.
  const std::unique_ptr<Iterator> it = NewDBIterator(
      NewFailingIterator({{IKey("a", 3), "new"}, {IKey("a", 2), "old"}, {IKey("a", 1), "older"}},
                         Status::IOError("disk")),
      10);
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  it->Next();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsIOError()) << it->status().ToString();
}

TEST(DBIterDeathTest, CallsThatNeedAnEntryAbortWithoutOne) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  const auto make = [] {
    auto mem = std::make_shared<MemTable>(1);
    mem->Add(1, ValueType::kValue, "a", "1");
    return NewDBIterator(mem->NewIterator(), 5);
  };
  EXPECT_DEATH(make()->Next(), "requires Valid");
  EXPECT_DEATH(static_cast<void>(make()->key()), "requires Valid");
  EXPECT_DEATH(static_cast<void>(make()->value()), "requires Valid");
}

TEST(DBIterDeathTest, ASequenceNoWriteCanHaveAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(static_cast<void>(NewDBIterator(NewEmptyIterator(), kMaxSequenceNumber + 1)),
               "sequence");
}

TEST(DBIterDeathTest, ANullInternalIteratorAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(static_cast<void>(NewDBIterator(nullptr, 1)), "internal iterator");
}

// ---- DB::NewIterator(), over a database.

class DBIterTest : public DBTestBase {
 protected:
  std::unique_ptr<Iterator> NewIter(const Snapshot* snapshot = nullptr) {
    ReadOptions options;
    options.snapshot = snapshot;
    return db_->NewIterator(options);
  }

  // The numbers of the tables of the current version.
  std::set<uint64_t> TableNumbers() {
    std::set<uint64_t> numbers;
    for (const std::vector<FileMetaData>& level : dbfull()->TEST_VersionState().files) {
      for (const FileMetaData& file : level) {
        numbers.insert(file.number);
      }
    }
    return numbers;
  }

  // A value of about a hundred bytes that says which round wrote it and for which key.
  static std::string Padded(const std::string& round, int i) {
    return round + ValueOf(i) + std::string(100, 'p');
  }

  Status PutUnsynced(std::string_view key, std::string_view value) {
    WriteOptions options;
    options.sync = false;
    return db_->Put(options, key, value);
  }

  Status DeleteUnsynced(std::string_view key) {
    WriteOptions options;
    options.sync = false;
    return db_->Delete(options, key);
  }

  // Flips the low bit of the byte at `offset` of the file `fname`, as it is at rest, and returns
  // whether there was such a byte. A handle that is already open stays on the file it had, so the
  // database is reopened after this for the damage to be read.
  bool FlipBitAtRest(const std::string& fname, size_t offset) {
    std::string contents = env_->FileContents(fname);
    if (offset >= contents.size()) {
      return false;
    }
    contents[offset] = static_cast<char>(contents[offset] ^ 0x01);
    env_->SetFileContents(fname, std::move(contents));
    return true;
  }

  // Reopens the database, so that no block of any table is cached, and sets `*table` to the table
  // `file` that the database's table cache opens: the one every read of it goes through.
  void ReopenAndFindTable(const FileMetaData& file, std::shared_ptr<Table>* table) {
    ASSERT_OK(TryReopen(CurrentOptions()));
    ASSERT_OK(dbfull()->TEST_TableCache()->FindTable(file.number, file.file_size, table));
  }

  // Makes the same read twice, a Get of UserKey(7) or a scan of every entry, with `options`, on a
  // database opened afresh, and sets `*reads` to how many data blocks the table `file` was read
  // for. A block that went into the cache the first time is not read the second.
  void CountBlockReadsOfTwoReads(const FileMetaData& file, const ReadOptions& options, bool scan,
                                 uint64_t* reads) {
    std::shared_ptr<Table> table;
    ASSERT_NO_FATAL_FAILURE(ReopenAndFindTable(file, &table));
    const uint64_t before = table->data_block_reads();
    for (int round = 0; round < 2; round++) {
      if (scan) {
        const std::unique_ptr<Iterator> it = db_->NewIterator(options);
        ASSERT_EQ(Scan(it.get()).size(), 20u);
        ASSERT_OK(it->status());
      } else {
        std::string value;
        ASSERT_OK(db_->Get(options, UserKey(7), &value));
        ASSERT_EQ(value, ValueOf(7));
      }
    }
    *reads = table->data_block_reads() - before;
  }
};

TEST_F(DBIterTest, EmptyDatabase) {
  const std::unique_ptr<Iterator> it = NewIter();
  EXPECT_FALSE(it->Valid()) << "a new iterator is not positioned";
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
  it->Seek("a");
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
  EXPECT_EQ(Contents(), "");
}

TEST_F(DBIterTest, OrderAndSeek) {
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("c", "3"));
  const std::unique_ptr<Iterator> it = NewIter();
  EXPECT_EQ(Scan(it.get()), (Entries{{"a", "1"}, {"b", "2"}, {"c", "3"}}));
  EXPECT_OK(it->status());
  EXPECT_EQ(ScanFrom(it.get(), "b"), (Entries{{"b", "2"}, {"c", "3"}}));
  EXPECT_EQ(ScanFrom(it.get(), "bb"), (Entries{{"c", "3"}}));
  EXPECT_EQ(ScanFrom(it.get(), "d"), Entries{});
  EXPECT_OK(it->status());
  // The scan above ended at the end, and a seek starts it from anywhere again, back included.
  it->Seek("a");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "a");
  EXPECT_EQ(it->value(), "1");
}

TEST_F(DBIterTest, DeletedKeysAreSkipped) {
  for (const char* key : {"a", "b", "c", "d", "e", "f"}) {
    ASSERT_OK(Put(key, std::string("v-") + key));
  }
  ASSERT_OK(Delete("a"));  // the first key
  ASSERT_OK(Delete("c"));  // two in a row in the middle
  ASSERT_OK(Delete("d"));
  ASSERT_OK(Delete("f"));  // the last key
  ASSERT_OK(Delete("never-written"));
  EXPECT_EQ(Contents(), "b=v-b,e=v-e");

  ASSERT_OK(Put("c", "again"));  // a deleted key may come back
  EXPECT_EQ(Contents(), "b=v-b,c=again,e=v-e");

  ASSERT_OK(Delete("b"));
  ASSERT_OK(Delete("c"));
  ASSERT_OK(Delete("e"));
  EXPECT_EQ(Contents(), "") << "every key deleted";
  const std::unique_ptr<Iterator> it = NewIter();
  it->Seek("a");
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

TEST_F(DBIterTest, DeletionsHideValuesInOlderSources) {
  ASSERT_OK(Put("a", "in-level-1"));
  ASSERT_OK(Put("b", "in-level-1"));
  ASSERT_OK(Put("c", "in-level-1"));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  ASSERT_EQ(NumTableFilesAtLevel(1), 1);
  ASSERT_OK(Delete("b"));
  ASSERT_OK(db_->Flush());  // the deletion is in level 0, the value in level 1
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  ASSERT_OK(Delete("c"));  // and this one is in the memtable
  EXPECT_EQ(Contents(), "a=in-level-1");
  const std::unique_ptr<Iterator> it = NewIter();
  it->Seek("b");
  EXPECT_FALSE(it->Valid()) << "a seek to a deleted key found its older value, or c";
  EXPECT_OK(it->status());
}

TEST_F(DBIterTest, IteratorIsAnImplicitSnapshot) {
  ASSERT_OK(Put("a", "old"));
  ASSERT_OK(Put("b", "old"));
  const std::unique_ptr<Iterator> it = NewIter();
  ASSERT_OK(Put("c", "new"));
  ASSERT_OK(Put("a", "newer"));
  ASSERT_OK(Delete("b"));
  EXPECT_EQ(Scan(it.get()), (Entries{{"a", "old"}, {"b", "old"}}));
  EXPECT_EQ(ScanFrom(it.get(), "b"), (Entries{{"b", "old"}}));
  // A second iterator, made now, shows the present.
  EXPECT_EQ(Contents(), "a=newer,c=new");
  EXPECT_EQ(Scan(it.get()), (Entries{{"a", "old"}, {"b", "old"}})) << "the scan changed with it";
}

TEST_F(DBIterTest, EmptyKeyIsFirst) {
  ASSERT_OK(Put("", "e"));
  ASSERT_OK(Put("a", "1"));
  const std::unique_ptr<Iterator> it = NewIter();
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "");
  EXPECT_EQ(it->value(), "e");
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "a");
  it->Seek("");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "");
  ASSERT_OK(Delete(""));
  EXPECT_EQ(Contents(), "a=1");
}

TEST_F(DBIterTest, BinaryKeysIterateInUnsignedOrder) {
  const std::string zero_in_the_middle("a\0b", 3);
  ASSERT_OK(Put("\x80", "high"));
  ASSERT_OK(Put("\x7f", "del"));
  ASSERT_OK(db_->Flush());  // the order is the same out of a table and out of the memtable
  ASSERT_OK(Put(zero_in_the_middle, "zero"));
  ASSERT_OK(Put("a", "a"));
  ASSERT_OK(Put(std::string("\xff\xff", 2), "top"));
  const Entries expected = {{"a", "a"},
                            {zero_in_the_middle, "zero"},
                            {"\x7f", "del"},
                            {"\x80", "high"},
                            {std::string("\xff\xff", 2), "top"}};
  const std::unique_ptr<Iterator> it = NewIter();
  EXPECT_EQ(Scan(it.get()), expected);
  EXPECT_EQ(ScanFrom(it.get(), std::string("a\0", 2)),
            Entries(expected.begin() + 1, expected.end()))
      << "a key is before a longer one it is a prefix of";
  EXPECT_EQ(ScanFrom(it.get(), "\x7f"), Entries(expected.begin() + 2, expected.end()));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  EXPECT_EQ(Scan(NewIter().get()), expected);
}

TEST_F(DBIterTest, MergesAllSources) {
  Reopen(SmallBuffer());
  std::map<std::string, std::string> model;
  // An ASSERT in a lambda returns from the lambda and not from the test, so these check with EXPECT
  // and change the model only for a write that was made; the test stops where it checks
  // HasFailure(), before it compares anything with a model that is out of step.
  const auto put = [&](const std::string& key, const std::string& value) {
    const Status s = PutUnsynced(key, value);
    EXPECT_OK(s);
    if (s.ok()) {
      model[key] = value;
    }
  };
  const auto del = [&](const std::string& key) {
    const Status s = DeleteUnsynced(key);
    EXPECT_OK(s);
    if (s.ok()) {
      model.erase(key);
    }
  };

  // Level 1: 60 keys, spread by a stride so that the next rounds interleave with them.
  for (int i = 0; i < 60; i++) {
    put(UserKey(i * 5), "l1-" + ValueOf(i));
  }
  ASSERT_FALSE(HasFailure()) << "a write failed";
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  ASSERT_GE(NumTableFilesAtLevel(1), 1);
  // Level 0: new keys between them, an overwrite and a deletion of what is below.
  for (int i = 0; i < 40; i++) {
    put(UserKey(i * 5 + 1), "l0-" + ValueOf(i));
  }
  put(UserKey(10), "l0-overwrite");
  del(UserKey(15));
  ASSERT_FALSE(HasFailure()) << "a write failed";
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  // The immutable memtable: written, and switched, and not flushed because no one runs the
  // scheduler.
  put(UserKey(2), "imm-a");
  put(UserKey(5), "imm-overwrite");
  del(UserKey(1));
  const int filler = PutUntilSwitched();
  for (int i = 0; i < filler; i++) {
    model[UserKey(i)] = KiB(i);  // the filler writes keys of its own, over some of these
  }
  ASSERT_EQ(CountFiles(FileType::kLogFile), 2) << "the memtable was not switched";
  ASSERT_EQ(NumTableFilesAtLevel(0), 1) << "the switched memtable was flushed";
  // The memtable.
  put(UserKey(3), "mem-a");
  put(UserKey(10), "mem-overwrite");
  del(UserKey(2));
  del(UserKey(20));
  put(UserKey(1000), "mem-last");
  ASSERT_FALSE(HasFailure()) << "a write failed";

  const std::unique_ptr<Iterator> it = NewIter();
  EXPECT_EQ(Scan(it.get()), Expected(model));
  EXPECT_OK(it->status());
  // Seeks into the middle land where the model says, in every source.
  for (const int probe : {0, 7, 11, 16, 99, 250, 299, 301, 1000, 1001}) {
    const std::string target = UserKey(probe);
    const auto first = model.lower_bound(target);
    const Entries want(first, model.end());
    EXPECT_EQ(ScanFrom(it.get(), target), want) << "Seek(" << target << ")";
  }
  // And once more after everything has been flushed and compacted: the same entries.
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  EXPECT_EQ(Scan(NewIter().get()), Expected(model));
}

TEST_F(DBIterTest, TheNewestVersionWinsWhichSourceItIsIn) {
  Reopen(SmallBuffer());
  ASSERT_OK(Put("a", "a-l1"));
  ASSERT_OK(Put("b", "b-l1"));
  ASSERT_OK(Put("c", "c-l1"));
  ASSERT_OK(Put("e", "e-l1"));
  ASSERT_OK(Put("f", "f-l1"));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  ASSERT_OK(Put("a", "a-l0"));
  ASSERT_OK(Delete("c"));
  ASSERT_OK(Delete("f"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Put("a", "a-imm"));
  ASSERT_OK(Delete("d"));
  ASSERT_OK(Put("d", "d-imm"));
  ASSERT_OK(Put("f", "f-imm"));
  const int filler = PutUntilSwitched();
  ASSERT_EQ(CountFiles(FileType::kLogFile), 2);
  ASSERT_OK(Put("a", "a-mem"));
  ASSERT_OK(Delete("b"));
  ASSERT_OK(Delete("d"));
  ASSERT_OK(Put("d", "d-mem"));

  std::map<std::string, std::string> model = {
      {"a", "a-mem"}, {"d", "d-mem"}, {"e", "e-l1"}, {"f", "f-imm"}};
  for (int i = 0; i < filler; i++) {
    model[UserKey(i)] = KiB(i);
  }
  EXPECT_EQ(Scan(NewIter().get()), Expected(model));
  const std::unique_ptr<Iterator> it = NewIter();
  it->Seek("b");  // deleted in the memtable: the next live key is d
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "d");
  EXPECT_EQ(it->value(), "d-mem");
  it->Seek("ab");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "d");
}

TEST_F(DBIterTest, ManyVersionsOfOneKeyShowOnce) {
  Reopen(SmallBuffer());
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 40; i++) {
      ASSERT_OK(PutUnsynced("key", "version-" + std::to_string(round * 40 + i)));
    }
    ASSERT_OK(db_->Flush());
  }
  ASSERT_OK(PutUnsynced("key", "version-120"));
  ASSERT_OK(PutUnsynced("other", "o"));
  EXPECT_EQ(Contents(), "key=version-120,other=o");
}

TEST_F(DBIterTest, LongRunsOfHiddenEntriesAreSkipped) {
  // 20000 entries between the ends, which a scan reads and does not return. An implementation
  // that recursed to get past each would run out of stack.
  constexpr int kHidden = 10000;
  ASSERT_OK(PutUnsynced("a", "first"));
  for (int i = 0; i < kHidden; i++) {
    ASSERT_OK(PutUnsynced(UserKey(i), "x"));
  }
  for (int i = 0; i < kHidden; i++) {
    ASSERT_OK(DeleteUnsynced(UserKey(i)));
  }
  ASSERT_OK(PutUnsynced("z", "last"));
  const std::unique_ptr<Iterator> it = NewIter();
  EXPECT_EQ(Scan(it.get()), (Entries{{"a", "first"}, {"z", "last"}}));
  it->Seek("k");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "z");
}

TEST_F(DBIterTest, ContentsSurviveAReopen) {
  // The sequence an iterator reads at is the database's last, which a reopen must restore: with
  // a lower one every write after it is above the iterator and none of them shows.
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Put("c", "3"));  // in the log only
  ASSERT_OK(Delete("a"));
  Reopen();
  EXPECT_EQ(Contents(), "b=2,c=3");
  Reopen();
  EXPECT_EQ(Contents(), "b=2,c=3");
}

TEST_F(DBIterTest, IteratorPinsTheMemtablesAcrossAFlush) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  const std::unique_ptr<Iterator> it = NewIter();  // over the memtable alone
  ASSERT_OK(db_->Flush());                         // which is a table now, and not the memtable
  ASSERT_OK(Put("c", "3"));
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(Scan(it.get()), (Entries{{"a", "1"}, {"b", "2"}}));
  EXPECT_OK(it->status());
}

TEST_F(DBIterTest, IteratorPinsFilesAcrossCompaction) {
  // 1000 keys in level 1, the even ones written again into level 0, so that the iterator is over
  // two sources, one of which it opens only as it reaches the file.
  std::map<std::string, std::string> model;
  for (int i = 0; i < 1000; i++) {
    model[UserKey(i)] = Padded("old-", i);
    ASSERT_OK(PutUnsynced(UserKey(i), model[UserKey(i)]));
  }
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  for (int i = 0; i < 1000; i += 2) {
    model[UserKey(i)] = Padded("mid-", i);
    ASSERT_OK(PutUnsynced(UserKey(i), model[UserKey(i)]));
  }
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  ASSERT_GE(NumTableFilesAtLevel(1), 1);

  std::unique_ptr<Iterator> it = NewIter();
  const std::set<uint64_t> pinned = TableNumbers();
  ASSERT_GE(pinned.size(), 2u);
  const Entries expected = Expected(model);
  Entries seen;
  it->SeekToFirst();
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(it->Valid());
    seen.emplace_back(std::string(it->key()), std::string(it->value()));
    it->Next();
  }

  // Overwrite all of it and compact it all: nothing that the iterator reads is in the tables
  // that the database has now, and a compaction that was free to would drop what it supersedes.
  for (int i = 0; i < 1000; i++) {
    ASSERT_OK(PutUnsynced(UserKey(i), Padded("new-", i)));
  }
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  const std::set<uint64_t> now = TableNumbers();
  for (const uint64_t number : pinned) {
    ASSERT_EQ(now.count(number), 0u) << "table " << number << " was not rewritten";
  }
  const std::set<uint64_t> live = dbfull()->TEST_LiveFiles();
  for (const uint64_t number : pinned) {
    EXPECT_EQ(live.count(number), 1u) << "table " << number << " is not kept for the iterator";
    EXPECT_TRUE(env_->FileExists(TableFileName(dbname_, number)))
        << "table " << number << " was deleted under the iterator";
  }

  for (; it->Valid(); it->Next()) {
    seen.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  EXPECT_OK(it->status());
  EXPECT_EQ(seen.size(), expected.size());
  EXPECT_EQ(seen, expected) << "the scan did not show the state the iterator was made in";
  // The new state is there for whoever asks now.
  EXPECT_EQ(Get(UserKey(7)), Padded("new-", 7));

  // Released, the tables are no one's. They are deleted by the next sweep, which the release
  // does not run: a flush does.
  it.reset();
  const std::set<uint64_t> live_after = dbfull()->TEST_LiveFiles();
  for (const uint64_t number : pinned) {
    EXPECT_EQ(live_after.count(number), 0u) << "table " << number << " is still held";
    EXPECT_TRUE(env_->FileExists(TableFileName(dbname_, number)))
        << "table " << number << " was deleted by the release, which runs no sweep";
  }
  ASSERT_OK(Put("sweep", "now"));
  ASSERT_OK(db_->Flush());
  for (const uint64_t number : pinned) {
    EXPECT_FALSE(env_->FileExists(TableFileName(dbname_, number)))
        << "table " << number << " was not deleted by the sweep that followed the release";
  }
}

TEST_F(DBIterTest, IteratorsHoldingOneVersionKeepItUntilTheLastIsGone) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  const std::set<uint64_t> tables = TableNumbers();
  ASSERT_EQ(tables.size(), 1u);
  std::unique_ptr<Iterator> first = NewIter();
  std::unique_ptr<Iterator> second = NewIter();
  ASSERT_OK(Put("a", "2"));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  first.reset();
  EXPECT_EQ(dbfull()->TEST_LiveFiles().count(*tables.begin()), 1u) << "the second still reads it";
  EXPECT_EQ(Scan(second.get()), (Entries{{"a", "1"}}));
  second.reset();
  EXPECT_EQ(dbfull()->TEST_LiveFiles().count(*tables.begin()), 0u);
}

TEST_F(DBIterTest, CorruptTableNeverYieldsWrongData) {
  Entries written;
  for (int i = 0; i < 300; i++) {
    written.emplace_back(UserKey(i), Padded("v-", i));
    ASSERT_OK(PutUnsynced(written.back().first, written.back().second));
  }
  ASSERT_OK(db_->Flush());
  Reopen();  // nothing of the table is cached any more: every read of it goes to the Env
  env_->SetReadCorruptionProbability(1.0);

  const std::unique_ptr<Iterator> it = NewIter();
  const Entries scanned = Scan(it.get());
  const Status status = it->status();
  env_->ClearFaults();
  ASSERT_GT(env_->injected_corruptions(), 0u) << "no read was corrupted: the test proves nothing";
  EXPECT_FALSE(it->Valid());
  for (size_t i = 0; i < scanned.size(); i++) {
    ASSERT_LT(i, written.size());
    EXPECT_EQ(scanned[i], written[i]) << "entry " << i << " is not what was written";
  }
  // Every read had a bit flipped, which the checksums see: the scan cannot have been complete.
  EXPECT_TRUE(status.IsCorruption())
      << status.ToString() << " after " << scanned.size() << " entries";
}

TEST_F(DBIterTest, CorruptionMidScanStopsTheIteratorAndIsReported) {
  Entries written;
  for (int i = 0; i < 1000; i++) {
    written.emplace_back(UserKey(i), Padded("v-", i));
    ASSERT_OK(PutUnsynced(written.back().first, written.back().second));
  }
  ASSERT_OK(db_->Flush());
  Reopen();

  const std::unique_ptr<Iterator> it = NewIter();  // the table is opened here, and read cleanly
  it->SeekToFirst();
  Entries scanned;
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(it->Valid()) << it->status().ToString();
    scanned.emplace_back(std::string(it->key()), std::string(it->value()));
    it->Next();
  }
  env_->SetReadCorruptionProbability(1.0);  // from here on, a block that is read is damaged
  for (; it->Valid(); it->Next()) {
    scanned.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  env_->ClearFaults();
  ASSERT_GT(env_->injected_corruptions(), 0u) << "no read was corrupted: the test proves nothing";
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
  EXPECT_GE(scanned.size(), 5u);
  EXPECT_LT(scanned.size(), written.size()) << "the scan went on past a block that is damaged";
  for (size_t i = 0; i < scanned.size(); i++) {
    EXPECT_EQ(scanned[i], written[i]) << "entry " << i << " is not what was written";
  }
  // It stays stopped: a seek, with the Env well again, does not go past the damage.
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
  it->Seek(UserKey(900));
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST_F(DBIterTest, ATombstoneInADamagedLevel0BlockNeverUncoversTheOlderValueBelow) {
  // Level 1 holds an old value of every key, all of them readable. Level 0 holds new values of the
  // first few keys and then a deletion of every other one, in blocks of about a dozen entries,
  // and one byte of it is damaged where only the deletions are. A scan that was told nothing of
  // the deletions in that block would go on to show the old values from level 1 as if they were
  // live: it has to stop, with the damage as its status, and so does a read of one such key.
  constexpr int kKeys = 300;
  constexpr int kRewritten = 5;
  Options options = CurrentOptions();
  options.block_size = 256;  // the smallest, so that the deletions are in many blocks
  Reopen(options);
  for (int i = 0; i < kKeys; i++) {
    ASSERT_OK(PutUnsynced(UserKey(i), Padded("old-", i)));
  }
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  ASSERT_GE(NumTableFilesAtLevel(1), 1);
  Entries rewritten;
  for (int i = 0; i < kRewritten; i++) {
    rewritten.emplace_back(UserKey(i), Padded("new-", i));
    ASSERT_OK(PutUnsynced(rewritten.back().first, rewritten.back().second));
  }
  for (int i = kRewritten; i < kKeys; i++) {
    ASSERT_OK(DeleteUnsynced(UserKey(i)));
  }
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  const FileMetaData level0 = dbfull()->TEST_VersionState().files[0][0];

  // The middle of the file is inside the deletions: the values come first and take a tenth of it,
  // and the index, the filter and the footer are after the last data block.
  Close();
  ASSERT_TRUE(FlipBitAtRest(TableFileName(dbname_, level0.number), level0.file_size / 2));
  Reopen(options);

  int damaged = 0;
  for (int i = 0; i < kKeys; i++) {
    std::string value;
    const Status s = db_->Get(ReadOptions(), UserKey(i), &value);
    if (s.IsCorruption()) {
      damaged++;
    } else if (i < kRewritten) {
      ASSERT_OK(s);
      EXPECT_EQ(value, Padded("new-", i));
    } else {
      EXPECT_TRUE(s.IsNotFound()) << UserKey(i) << ": " << s.ToString() << " " << value;
    }
  }
  ASSERT_GT(damaged, 0) << "no key was in the damaged block: the test proves nothing";

  const std::unique_ptr<Iterator> it = NewIter();
  const Entries scanned = Scan(it.get());
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
  EXPECT_EQ(scanned, rewritten)
      << "the scan showed a value that a deletion hides, or stopped before the damage";
  it->Seek(UserKey(100));
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption()) << "a seek went past the damage";
}

TEST_F(DBIterTest, FillCacheOfReadOptionsReachesTheBlockReads) {
  for (int i = 0; i < 20; i++) {
    ASSERT_OK(Put(UserKey(i), ValueOf(i)));  // a table of one data block
  }
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  const FileMetaData file = dbfull()->TEST_VersionState().files[0][0];

  for (const bool scan : {false, true}) {
    SCOPED_TRACE(scan ? "a scan" : "a Get");
    uint64_t cached = 0;
    uint64_t not_cached = 0;
    ReadOptions fill;
    ASSERT_NO_FATAL_FAILURE(CountBlockReadsOfTwoReads(file, fill, scan, &cached));
    EXPECT_EQ(cached, 1u) << "the second read did not find the block in the cache";
    ReadOptions no_fill;
    no_fill.fill_cache = false;
    ASSERT_NO_FATAL_FAILURE(CountBlockReadsOfTwoReads(file, no_fill, scan, &not_cached));
    EXPECT_EQ(not_cached, 2u) << "a read with fill_cache off left its block in the cache";
  }
}

TEST_F(DBIterTest, ChecksumsOfReadOptionsDecideWhetherAnUncheckedBlockIsRead) {
  for (int i = 0; i < 20; i++) {
    ASSERT_OK(Put(UserKey(i), ValueOf(i)));
  }
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  const FileMetaData file = dbfull()->TEST_VersionState().files[0][0];

  // A block that nothing has checked is not put in the cache, where a read that does check would
  // find it and take it as checked.
  for (const bool scan : {false, true}) {
    SCOPED_TRACE(scan ? "a scan" : "a Get");
    uint64_t reads = 0;
    ReadOptions unchecked;
    unchecked.verify_checksums = false;
    ASSERT_NO_FATAL_FAILURE(CountBlockReadsOfTwoReads(file, unchecked, scan, &reads));
    EXPECT_EQ(reads, 2u) << "a block that was not verified went into the cache";
  }
}

TEST_F(DBIterTest, ChecksumsOfReadOptionsDecideWhetherADamagedBlockIsCaught) {
  const std::string marker = "marker-value-0123456789";
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", marker));
  ASSERT_OK(Put("c", "3"));
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  const FileMetaData file = dbfull()->TEST_VersionState().files[0][0];
  Close();
  // A byte of the value of "b", which is stored as it is: the block still parses, and only its
  // checksum says it is damaged.
  const std::string fname = TableFileName(dbname_, file.number);
  const size_t at = env_->FileContents(fname).find(marker);
  ASSERT_NE(at, std::string::npos);
  ASSERT_TRUE(FlipBitAtRest(fname, at + 3));
  std::string damaged_value = marker;
  damaged_value[3] = static_cast<char>(damaged_value[3] ^ 0x01);
  Reopen();

  ReadOptions checked;
  ReadOptions unchecked;
  unchecked.verify_checksums = false;
  std::string value;
  EXPECT_TRUE(db_->Get(checked, "b", &value).IsCorruption());
  ASSERT_OK(db_->Get(unchecked, "b", &value));
  EXPECT_EQ(value, damaged_value) << "the read was made without the check";
  EXPECT_TRUE(db_->Get(checked, "b", &value).IsCorruption())
      << "the unchecked read left the damaged block in the cache";

  const std::unique_ptr<Iterator> checking = db_->NewIterator(checked);
  EXPECT_EQ(Scan(checking.get()), Entries{});
  EXPECT_TRUE(checking->status().IsCorruption()) << checking->status().ToString();
  const std::unique_ptr<Iterator> unchecking = db_->NewIterator(unchecked);
  EXPECT_EQ(Scan(unchecking.get()), (Entries{{"a", "1"}, {"b", damaged_value}, {"c", "3"}}));
  EXPECT_OK(unchecking->status());
}

TEST_F(DBIterTest, ConcurrentIteratorsSeeAPrefixOfTheWrites) {
  // The writer writes UserKey(0), UserKey(1), ..., one after another. Whatever sequence number an
  // iterator reads at, what it shows is the keys up to some n, with none missing and none after.
  // Small memtables, so that flushes and compactions run under the readers.
  Reopen(SmallBuffer());
  constexpr int kWrites = 300;
  constexpr int kReaders = 3;
  std::atomic<bool> done{false};
  std::atomic<int> scans{0};
  std::atomic<int> broken{0};
  std::atomic<int> failed{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; r++) {
    readers.emplace_back([&] {
      int mine = 0;
      while (!done.load() || mine < 3) {
        const std::unique_ptr<Iterator> it = db_->NewIterator(ReadOptions());
        int expected_next = 0;
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
          if (it->key() != UserKey(expected_next) || it->value() != KiB(expected_next)) {
            broken.fetch_add(1);
            break;
          }
          expected_next++;
        }
        if (!it->status().ok()) {
          failed.fetch_add(1);
        }
        mine++;
        scans.fetch_add(1);
      }
    });
  }
  // The readers are joinable threads: a fatal failure that returned from here would destroy them
  // and end the process. So the loop only notes a failed write and stops, and the threads are
  // joined before anything is asserted.
  Status write_status;
  for (int i = 0; i < kWrites && write_status.ok(); i++) {
    write_status = PutUnsynced(UserKey(i), KiB(i));
  }
  done.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  ASSERT_OK(write_status);
  EXPECT_EQ(broken.load(), 0) << "an iterator showed something that is not a prefix of the writes";
  EXPECT_EQ(failed.load(), 0);
  EXPECT_GE(scans.load(), 3 * kReaders);
  EXPECT_EQ(Scan(NewIter().get()).size(), static_cast<size_t>(kWrites));
}

}  // namespace
}  // namespace shale
