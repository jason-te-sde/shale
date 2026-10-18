// Iterators at a snapshot, and what closing the database refuses while an iterator is alive. The
// snapshots themselves, reading at one and giving one back, are tested with the rest of the point
// reads in db_basic_test.cc and with compaction in db_compaction_test.cc.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include "db/db_impl.h"
#include "db/db_test_util.h"
#include "shale/db.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"

namespace shale {
namespace {

class DBSnapshotTest : public DBTestBase {};

// A snapshot that the test releases on its way out, whether or not it fails: one that is still
// open aborts the close of the database, which would hide the failure. A test that gives it back
// earlier calls Release(), and the destructor then has nothing to do: a snapshot is released once.
class ScopedSnapshot {
 public:
  explicit ScopedSnapshot(DB* db) : db_(db), snapshot_(db->GetSnapshot()) {}
  ~ScopedSnapshot() { Release(); }
  ScopedSnapshot(const ScopedSnapshot&) = delete;
  ScopedSnapshot& operator=(const ScopedSnapshot&) = delete;

  const Snapshot* get() const { return snapshot_; }

  // Gives the snapshot back now, if it is still held.
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

TEST_F(DBSnapshotTest, IteratorAtSnapshot) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "1"));
  ASSERT_OK(Put("gone", "x"));
  const ScopedSnapshot snapshot(db_.get());
  ASSERT_OK(Put("a", "2"));
  ASSERT_OK(Delete("gone"));
  ASSERT_OK(Put("c", "new"));

  EXPECT_EQ(Contents(snapshot.get()), "a=1,b=1,gone=x");
  EXPECT_EQ(Contents(), "a=2,b=1,c=new");

  ReadOptions at_snapshot;
  at_snapshot.snapshot = snapshot.get();
  const std::unique_ptr<Iterator> it = db_->NewIterator(at_snapshot);
  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "b");
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "gone") << "a key deleted after the snapshot is still there at it";
  it->Next();
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

TEST_F(DBSnapshotTest, SnapshotIteratorSurvivesFlushAndCompaction) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "1"));
  ASSERT_OK(Put("gone", "x"));
  const ScopedSnapshot snapshot(db_.get());
  ASSERT_OK(Put("a", "2"));
  ASSERT_OK(Delete("gone"));
  ASSERT_OK(Put("c", "new"));
  EXPECT_EQ(Contents(snapshot.get()), "a=1,b=1,gone=x") << "from the memtable";
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(Contents(snapshot.get()), "a=1,b=1,gone=x") << "from a level 0 table";
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  EXPECT_EQ(Contents(snapshot.get()), "a=1,b=1,gone=x")
      << "from level 1: the compaction dropped what the snapshot reads";
  EXPECT_EQ(Contents(), "a=2,b=1,c=new");
}

TEST_F(DBSnapshotTest, TwoSnapshotsAndTheNewestStateEachSeeTheirOwn) {
  ASSERT_OK(Put("k", "v1"));
  const ScopedSnapshot first(db_.get());
  ASSERT_OK(Put("k", "v2"));
  const ScopedSnapshot second(db_.get());
  ASSERT_OK(Delete("k"));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  EXPECT_EQ(Contents(first.get()), "k=v1");
  EXPECT_EQ(Contents(second.get()), "k=v2");
  EXPECT_EQ(Contents(), "");
}

TEST_F(DBSnapshotTest, ASnapshotOfAnEmptyDatabaseSeesNothingOfWhatIsWrittenLater) {
  const ScopedSnapshot snapshot(db_.get());
  ASSERT_OK(Put("a", "1"));
  EXPECT_EQ(Contents(snapshot.get()), "");
  EXPECT_EQ(Contents(), "a=1");
}

TEST_F(DBSnapshotTest, AnIteratorKeepsWhatItReadsWhenItsSnapshotIsReleased) {
  // The iterator is made at the snapshot, which is then given back and the database compacted:
  // what the iterator holds are the tables of the version it was made in, and not the snapshot,
  // so the versions that the snapshot kept are still there to read. The tables are in level 1,
  // which an iterator opens as it comes to them: a table that it did not hold would be gone.
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("gone", "x"));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  ASSERT_EQ(NumTableFilesAtLevel(1), 1);
  ScopedSnapshot snapshot(db_.get());
  ASSERT_OK(Put("a", "2"));
  ASSERT_OK(Delete("gone"));
  ReadOptions at_snapshot;
  at_snapshot.snapshot = snapshot.get();
  std::unique_ptr<Iterator> it = db_->NewIterator(at_snapshot);
  snapshot.Release();
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  ASSERT_OK(Put("a", "3"));
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));

  std::string seen;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    seen.append(it->key()).append("=").append(it->value()).append(";");
  }
  EXPECT_OK(it->status());
  EXPECT_EQ(seen, "a=1;gone=x;");
}

TEST_F(DBSnapshotTest, ClosingAfterAnIteratorIsDestroyedDoesNotAbort) {
  // The check must also be able to stay quiet. Each of these ends with the iterator gone, whatever
  // it was used for, and the database closes and opens again.
  {
    const std::unique_ptr<Iterator> unused = db_->NewIterator({});
  }
  {
    const std::unique_ptr<Iterator> scanned = db_->NewIterator({});
    scanned->SeekToFirst();
  }
  {
    ASSERT_OK(Put("a", "1"));
    std::unique_ptr<Iterator> moved = db_->NewIterator({});
    std::unique_ptr<Iterator> other = std::move(moved);  // moved, and destroyed once
  }
  Reopen();
  EXPECT_EQ(Contents(), "a=1");
}

// Closing with something alive aborts, so each of these runs in a child process.
class DBSnapshotDeathTest : public DBTestBase {};

TEST_F(DBSnapshotDeathTest, ClosingWithLiveIteratorAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        auto it = db_->NewIterator({});
        db_.reset();
      },
      "1 live iterator");
}

TEST_F(DBSnapshotDeathTest, ClosingWithAnIteratorAndASnapshotNamesTheIteratorFirst) {
  // Both are alive, and the close checks the iterator first: it points into what is about to be
  // freed, and a snapshot is only a number. The message is the first check's alone, so a pattern
  // that names the iterator fails if the snapshot is checked before it.
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        const Snapshot* snapshot = db_->GetSnapshot();
        auto it = db_->NewIterator({});
        static_cast<void>(snapshot);
        db_.reset();
      },
      "1 live iterator");
}

TEST_F(DBSnapshotDeathTest, ClosingWithTwoLiveIteratorsCountsBoth) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        auto first = db_->NewIterator({});
        auto second = db_->NewIterator({});
        db_.reset();
      },
      "2 live iterator");
}

TEST_F(DBSnapshotDeathTest, ANewIteratorAtAReleasedSnapshotAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  const Snapshot* snapshot = db_->GetSnapshot();
  db_->ReleaseSnapshot(snapshot);
  ReadOptions options;
  options.snapshot = snapshot;
  EXPECT_DEATH(static_cast<void>(db_->NewIterator(options)), "released twice");
}

TEST_F(DBSnapshotDeathTest, ANewIteratorAtASnapshotOfAnotherDatabaseAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  Options options = CurrentOptions();
  options.scheduler = nullptr;
  std::unique_ptr<DB> other;
  ASSERT_OK(DB::Open(options, "/other", &other));
  const Snapshot* snapshot = other->GetSnapshot();
  ReadOptions read;
  read.snapshot = snapshot;
  EXPECT_DEATH(static_cast<void>(db_->NewIterator(read)), "did not create it");
  other->ReleaseSnapshot(snapshot);
}

}  // namespace
}  // namespace shale
