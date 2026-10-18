// Opening a database, the single-writer write path and point reads, as a caller sees them.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "db/db_impl.h"
#include "db/db_test_util.h"
#include "io/write_batch_internal.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/options.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/filename.h"
#include "util/random.h"

namespace shale {
namespace {

using Op = FaultInjectionEnv::Op;

bool Contains(const Status& s, const std::string& text) {
  return s.ToString().find(text) != std::string::npos;
}

class DBBasicTest : public DBTestBase {};

TEST_F(DBBasicTest, OpenMissingWithoutCreate) {
  FaultInjectionEnv fresh(2);
  Options options = CurrentOptions();
  options.env = &fresh;
  options.create_if_missing = false;
  std::unique_ptr<DB> db;
  const Status s = DB::Open(options, "/db", &db);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_TRUE(Contains(s, "does not exist")) << s.ToString();
  EXPECT_EQ(db, nullptr);
  EXPECT_FALSE(fresh.FileExists("/db")) << "an open that may not create must create nothing";
}

TEST_F(DBBasicTest, OpenInADirectoryThatIsNotADatabaseCreatesNothingInIt) {
  FaultInjectionEnv fresh(2);
  ASSERT_OK(fresh.CreateDir("/db"));
  Options options = CurrentOptions();
  options.env = &fresh;
  options.create_if_missing = false;
  std::unique_ptr<DB> db;
  const Status s = DB::Open(options, "/db", &db);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_TRUE(Contains(s, "does not exist")) << s.ToString();
  std::vector<std::string> names;
  ASSERT_OK(fresh.GetChildren("/db", &names));
  EXPECT_TRUE(names.empty()) << "the refused open left " << names.size() << " file(s)";
}

TEST_F(DBBasicTest, OpenExistingWithErrorIfExists) {
  Close();
  Options options = CurrentOptions();
  options.error_if_exists = true;
  const Status s = TryReopen(options);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_TRUE(Contains(s, "exists")) << s.ToString();
  EXPECT_EQ(db_, nullptr);
  // The refused open took the lock and gave it back: the database opens as usual afterwards.
  Reopen();
}

TEST_F(DBBasicTest, OpenRejectsInvalidOptions) {
  // The database is open and holds the lock, so an open that got as far as the lock would report
  // that instead: the options are checked before anything else.
  Options options = CurrentOptions();
  options.l0_slowdown_trigger = 13;
  options.l0_stop_trigger = 12;
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, dbname_, &db);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_TRUE(Contains(s, "l0_stop_trigger")) << s.ToString();

  options = CurrentOptions();
  options.block_size = 100;
  s = DB::Open(options, dbname_, &db);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_TRUE(Contains(s, "block_size")) << s.ToString();
  EXPECT_EQ(db, nullptr);
}

TEST_F(DBBasicTest, OpenSamePathTwiceFails) {
  ASSERT_OK(Put("a", "1"));
  const std::string log_before = env_->FileContents(InfoLogFileName(dbname_));
  std::unique_ptr<DB> second;
  const Status s = DB::Open(CurrentOptions(), dbname_, &second);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_TRUE(Contains(s, "LOCK")) << s.ToString();
  EXPECT_EQ(second, nullptr);
  // The refused open did not touch what the running one is using: its LOG is not rotated away.
  EXPECT_FALSE(env_->FileExists(OldInfoLogFileName(dbname_)));
  EXPECT_EQ(env_->FileContents(InfoLogFileName(dbname_)), log_before);
  EXPECT_EQ(Get("a"), "1");
}

TEST_F(DBBasicTest, OpenWithGarbageCurrentIsCorruption) {
  Close();
  env_->SetFileContents("/db/CURRENT", "garbage\n");
  const Status s = TryReopen(CurrentOptions());
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(db_, nullptr);
}

TEST_F(DBBasicTest, PutGetOverwriteDelete) {
  ASSERT_OK(Put("a", "1"));
  EXPECT_EQ(Get("a"), "1");
  ASSERT_OK(Put("a", "2"));
  EXPECT_EQ(Get("a"), "2");
  ASSERT_OK(Delete("a"));
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_EQ(Get("missing"), "NOT_FOUND");
  ASSERT_OK(Put("a", "3"));  // a deleted key can be written again
  EXPECT_EQ(Get("a"), "3");
}

TEST_F(DBBasicTest, EmptyKeyAndEmptyValue) {
  ASSERT_OK(Put("", ""));
  EXPECT_EQ(Get(""), "");
  EXPECT_EQ(Get("x"), "NOT_FOUND");
  ASSERT_OK(Put("", "x"));
  EXPECT_EQ(Get(""), "x");
  Reopen();
  EXPECT_EQ(Get(""), "x");
}

TEST_F(DBBasicTest, KeyAndValueSizeLimits) {
  const std::string key_at_limit(kMaxKeySize, 'k');
  ASSERT_OK(Put(key_at_limit, "v"));
  EXPECT_EQ(Get(key_at_limit), "v");

  const SequenceNumber before = dbfull()->TEST_LastSequence();
  const std::string key_over_limit(kMaxKeySize + 1, 'k');
  Status s = Put(key_over_limit, "v");
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(dbfull()->TEST_LastSequence(), before);
  EXPECT_TRUE(Delete(key_over_limit).IsInvalidArgument());
  EXPECT_EQ(dbfull()->TEST_LastSequence(), before);

  const std::string value_at_limit(kMaxValueSize, 'v');
  ASSERT_OK(Put("big", value_at_limit));
  EXPECT_TRUE(Get("big") == value_at_limit);
  EXPECT_EQ(dbfull()->TEST_LastSequence(), before + 1);

  s = Put("bigger", std::string(kMaxValueSize + 1, 'v'));
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(Get("bigger"), "NOT_FOUND");
  EXPECT_EQ(dbfull()->TEST_LastSequence(), before + 1);
}

TEST_F(DBBasicTest, AnOversizedEntryRefusesTheWholeBatch) {
  const SequenceNumber before = dbfull()->TEST_LastSequence();
  WriteBatch batch;
  batch.Put("fine", "1");
  batch.Put(std::string(kMaxKeySize + 1, 'k'), "2");
  const Status s = db_->Write(WriteOptions(), &batch);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_TRUE(Contains(s, "exceeds the size limit")) << s.ToString();
  EXPECT_EQ(Get("fine"), "NOT_FOUND");
  EXPECT_EQ(dbfull()->TEST_LastSequence(), before);
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 0u) << "a refused batch is not a background error";
  ASSERT_OK(Put("later", "ok"));  // and writes go on
}

TEST_F(DBBasicTest, WriteBatchIsAtomicAndOrdered) {
  const SequenceNumber before = dbfull()->TEST_LastSequence();
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  batch.Delete("a");
  ASSERT_OK(db_->Write(WriteOptions(), &batch));
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_EQ(Get("b"), "2");
  EXPECT_EQ(dbfull()->TEST_LastSequence(), before + 3);

  // The batch took the next three numbers, in order, and the next write takes the one after.
  EXPECT_EQ(WriteBatchInternal::Sequence(&batch), before + 1);
  ASSERT_OK(Put("c", "3"));
  EXPECT_EQ(dbfull()->TEST_LastSequence(), before + 4);
}

TEST_F(DBBasicTest, EmptyBatchIsAcceptedAndWritesNothing) {
  const SequenceNumber before = dbfull()->TEST_LastSequence();
  WriteBatch empty;
  EXPECT_OK(db_->Write(WriteOptions(), &empty));
  EXPECT_EQ(dbfull()->TEST_LastSequence(), before);
  // Nothing was logged either, so the log holds no record that a later open would have to make
  // sense of.
  ASSERT_OK(Put("a", "1"));
  Reopen();
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(dbfull()->TEST_LastSequence(), before + 1);
}

TEST_F(DBBasicTest, NullBatchIsAnInvalidArgument) {
  const Status s = db_->Write(WriteOptions(), nullptr);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

TEST_F(DBBasicTest, NonSyncWritesAreReadable) {
  ASSERT_OK(Put("a", "1", /*sync=*/false));
  EXPECT_EQ(Get("a"), "1");
}

TEST_F(DBBasicTest, WriteThatWouldPassTheLargestSequenceNumberIsRefused) {
  // A database whose MANIFEST says it is two writes from the end of the sequence numbers. No test
  // can write 2^56 times, so the MANIFEST is written as if a database had. A write that would need
  // a number beyond the end is refused instead of persisting a last sequence number that Recover()
  // refuses to open.
  Close();
  ASSERT_OK(DestroyDB(dbname_, CurrentOptions()));
  ASSERT_OK(ForgeEmptyDatabase(env_.get(), dbname_, kMaxSequenceNumber - 2, 2));
  Reopen();
  ASSERT_EQ(dbfull()->TEST_LastSequence(), kMaxSequenceNumber - 2);

  WriteBatch three;
  three.Put("a", "1");
  three.Put("b", "2");
  three.Put("c", "3");
  Status s = db_->Write(WriteOptions(), &three);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s, "sequence number")) << s.ToString();
  EXPECT_EQ(dbfull()->TEST_LastSequence(), kMaxSequenceNumber - 2);
  EXPECT_EQ(Get("a"), "NOT_FOUND") << "none of a refused batch is applied";
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 0u);

  // The refusal is of that batch: one that fits goes through, up to the last number.
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  EXPECT_EQ(dbfull()->TEST_LastSequence(), kMaxSequenceNumber);
  s = Put("c", "3");
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(dbfull()->TEST_LastSequence(), kMaxSequenceNumber);
  EXPECT_EQ(Get("c"), "NOT_FOUND");

  // What was written reopens: the last sequence number on disk is one that Recover() accepts.
  Reopen();
  EXPECT_EQ(dbfull()->TEST_LastSequence(), kMaxSequenceNumber);
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "2");
  EXPECT_TRUE(Put("c", "3").IsCorruption());
}

TEST_F(DBBasicTest, WalSyncFailureFailsTheWriteAndEveryOneAfterIt) {
  ASSERT_OK(Put("before", "1"));
  const SequenceNumber before = dbfull()->TEST_LastSequence();
  env_->FailOpAfter(Op::kSync, 0);
  const Status failed = Put("lost", "2");
  EXPECT_TRUE(failed.IsIOError()) << failed.ToString();
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
  EXPECT_EQ(dbfull()->TEST_LastSequence(), before) << "a write that failed took no sequence number";
  EXPECT_EQ(Get("lost"), "NOT_FOUND") << "and is not visible, whatever the log holds";

  // The Env would let the next sync through, but the log is no longer trusted: every write returns
  // the first error until the database is opened again, and reads go on.
  const Status later = Put("after", "3", /*sync=*/false);
  EXPECT_EQ(later.ToString(), failed.ToString());
  EXPECT_TRUE(Delete("before").IsIOError());
  EXPECT_EQ(Get("before"), "1");
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u) << "the error is counted once";
}

TEST_F(DBBasicTest, WalAppendFailureFailsTheWrite) {
  env_->FailOpAfter(Op::kAppend, 0);
  const Status failed = Put("lost", "2", /*sync=*/false);
  EXPECT_TRUE(failed.IsIOError()) << failed.ToString();
  EXPECT_TRUE(Put("after", "3", /*sync=*/false).IsIOError());
  EXPECT_EQ(Get("lost"), "NOT_FOUND");
}

TEST_F(DBBasicTest, AFailureAnywhereInCreatingADatabaseLeavesOneThatOpens) {
  // Every call that creating a database makes through the Env is made to fail in turn. Whatever
  // state the directory is left in, whole or not there, the next open creates or recovers a
  // database that works.
  const std::vector<Op> ops = {Op::kNewWritable, Op::kAppend,       Op::kFlush,   Op::kSync,
                               Op::kClose,       Op::kRename,       Op::kSyncDir, Op::kRemove,
                               Op::kRead,        Op::kNewSequential};
  int failures = 0;
  for (const Op op : ops) {
    for (int n = 0; n < 40; n++) {
      FaultInjectionEnv env(7);
      Options options = CurrentOptions();
      options.env = &env;
      env.FailOpAfter(op, n);
      std::unique_ptr<DB> db;
      const Status first = DB::Open(options, "/fresh", &db);
      const bool fired = env.injected_errors() > 0;
      env.ClearFaults();
      if (fired && !first.ok()) {
        failures++;
        EXPECT_EQ(db, nullptr);
      }
      db.reset();

      ASSERT_OK(DB::Open(options, "/fresh", &db));
      ASSERT_OK(db->Put(WriteOptions(), "k", "v"));
      std::string value;
      ASSERT_OK(db->Get(ReadOptions(), "k", &value));
      EXPECT_EQ(value, "v");
      db.reset();
      if (!fired) {
        break;  // creating makes fewer calls of this kind than n + 1: they have all been tried
      }
    }
  }
  EXPECT_GT(failures, 10) << "the sweep hardly made Open fail";
}

TEST_F(DBBasicTest, ConcurrentWritersAndReadersDoNotInterfere) {
  constexpr int kWriters = 4;
  constexpr int kKeysEach = 200;
  const auto key_of = [](int writer, int i) {
    return "w" + std::to_string(writer) + "-" + std::to_string(i);
  };
  std::atomic<int> writers_done{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; w++) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kKeysEach; i++) {
        const std::string key = key_of(w, i);
        EXPECT_OK(Put(key, key, /*sync=*/false));
      }
      writers_done++;
    });
  }
  for (int r = 0; r < 2; r++) {
    threads.emplace_back([&, r] {
      Random rnd(static_cast<uint64_t>(r) + 1);
      while (writers_done.load() < kWriters) {
        const std::string key = key_of(static_cast<int>(rnd.Uniform(kWriters)),
                                       static_cast<int>(rnd.Uniform(kKeysEach)));
        const std::string value = Get(key);
        EXPECT_TRUE(value == "NOT_FOUND" || value == key) << key << " read as " << value;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(dbfull()->TEST_LastSequence(), SequenceNumber{kWriters * kKeysEach});
  for (int w = 0; w < kWriters; w++) {
    for (int i = 0; i < kKeysEach; i++) {
      EXPECT_EQ(Get(key_of(w, i)), key_of(w, i));
    }
  }
}

TEST_F(DBBasicTest, DestroyDBRemovesEverything) {
  // A database with every kind of file there is: the data in a table, from the second open, two
  // info logs, a log, a MANIFEST, CURRENT and LOCK, and a temporary file that a crash left.
  ASSERT_OK(Put("a", "1"));
  Reopen();
  env_->SetFileContents(TempFileName(dbname_, 77), "half a CURRENT");
  // A log that a lenient open set aside: DestroyDB is a request to delete the database, and it
  // takes these with it.
  env_->SetFileContents(LogFileName(dbname_, 9) + ".dropped", "writes that were dropped");
  Close();
  const std::vector<std::string> files = DbFiles();
  const auto has_type = [&](FileType wanted) {
    return std::any_of(files.begin(), files.end(), [wanted](const std::string& name) {
      uint64_t number = 0;
      FileType type = FileType::kCurrentFile;
      return ParseFileName(name, &number, &type) && type == wanted;
    });
  };
  ASSERT_TRUE(has_type(FileType::kTableFile));
  ASSERT_TRUE(has_type(FileType::kTempFile));
  ASSERT_TRUE(has_type(FileType::kLogFile));
  ASSERT_TRUE(has_type(FileType::kDescriptorFile));
  ASSERT_TRUE(env_->FileExists("/db/LOG.old"));

  ASSERT_OK(DestroyDB(dbname_, CurrentOptions()));
  std::vector<std::string> names;
  EXPECT_TRUE(!env_->GetChildren(dbname_, &names).ok() || names.empty());
  EXPECT_FALSE(env_->FileExists("/db/CURRENT"));
  EXPECT_FALSE(env_->FileExists("/db/LOG.old"));
  EXPECT_FALSE(env_->FileExists("/db/LOG"));
  EXPECT_FALSE(env_->FileExists("/db/LOCK"));
  EXPECT_FALSE(env_->FileExists(TempFileName(dbname_, 77)));
  EXPECT_FALSE(env_->FileExists(LogFileName(dbname_, 9) + ".dropped"));
  EXPECT_FALSE(env_->FileExists("/db"));

  // Destroying what is not there is a success.
  EXPECT_OK(DestroyDB(dbname_, CurrentOptions()));
  EXPECT_OK(DestroyDB("/never-existed", CurrentOptions()));

  // And a database can be created in its place.
  Reopen();
  EXPECT_EQ(Get("a"), "NOT_FOUND");
}

TEST_F(DBBasicTest, DestroyDBRefusesADatabaseThatIsOpen) {
  ASSERT_OK(Put("a", "1"));
  const Status s = DestroyDB(dbname_, CurrentOptions());
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_TRUE(Contains(s, "LOCK")) << s.ToString();
  EXPECT_EQ(Get("a"), "1");
  EXPECT_TRUE(env_->FileExists("/db/CURRENT"));
  Reopen();
  EXPECT_EQ(Get("a"), "1");
}

TEST_F(DBBasicTest, DestroyDBLeavesFilesItDoesNotKnow) {
  Close();
  // Only a log's own name with ".dropped" after it is the database's: anything else that merely
  // looks like it is somebody's file.
  const std::vector<std::string> strangers = {
      "notes.txt", "notes.log.dropped", TableFileName("", 9).substr(1) + ".dropped",
      LogFileName("", 9).substr(1) + ".dropped.bak", ".dropped"};
  for (const std::string& name : strangers) {
    env_->SetFileContents(dbname_ + "/" + name, "mine");
  }
  ASSERT_OK(DestroyDB(dbname_, CurrentOptions()));
  for (const std::string& name : strangers) {
    EXPECT_EQ(env_->FileContents(dbname_ + "/" + name), "mine") << name;
  }
  EXPECT_FALSE(env_->FileExists("/db/CURRENT"));
  std::vector<std::string> expected = strangers;
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(DbFiles(), expected);
}

TEST_F(DBBasicTest, InfoLogIsWritten) { EXPECT_FALSE(env_->FileContents("/db/LOG").empty()); }

TEST_F(DBBasicTest, InfoLogOfThePreviousRunIsKeptAsLogOld) {
  Close();
  const std::string first_run = env_->FileContents("/db/LOG");
  ASSERT_FALSE(first_run.empty());
  Reopen();
  EXPECT_EQ(env_->FileContents("/db/LOG.old"), first_run);
  EXPECT_FALSE(env_->FileContents("/db/LOG").empty());
}

TEST_F(DBBasicTest, AnInfoLogThatCannotBeWrittenDoesNotFailTheOpen) {
  Close();
  env_->SetFileContents("/db/LOG", "");    // exists, so the open will rename it and create another
  env_->FailOpAfter(Op::kNewWritable, 0);  // the first file the open creates is the new LOG
  Reopen();
  EXPECT_EQ(Get("x"), "NOT_FOUND");
  ASSERT_OK(Put("x", "1"));
}

TEST_F(DBBasicTest, GetAtASnapshotSeesTheStateWhenItWasTaken) {
  ASSERT_OK(Put("a", "1"));
  const Snapshot* snapshot = db_->GetSnapshot();
  ASSERT_OK(Put("a", "2"));
  ASSERT_OK(Put("b", "new"));
  EXPECT_EQ(Get("a", snapshot), "1");
  EXPECT_EQ(Get("b", snapshot), "NOT_FOUND");
  EXPECT_EQ(Get("a"), "2");
  ASSERT_OK(Delete("a"));
  EXPECT_EQ(Get("a", snapshot), "1");
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  db_->ReleaseSnapshot(snapshot);
}

TEST(DBBasicDeathTest, ReleasingASnapshotTwiceAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  FaultInjectionEnv env(3);
  Options options;
  options.env = &env;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(options, "/db", &db));
  const Snapshot* snapshot = db->GetSnapshot();
  db->ReleaseSnapshot(snapshot);
  // The pointer is to memory that has been freed: it is compared with the open snapshots and not
  // read, so the second release is reported and not a read of freed memory.
  EXPECT_DEATH(db->ReleaseSnapshot(snapshot), "released twice");
}

TEST(DBBasicDeathTest, ReadingAtAReleasedSnapshotAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  FaultInjectionEnv env(3);
  Options options;
  options.env = &env;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(options, "/db", &db));
  const Snapshot* snapshot = db->GetSnapshot();
  db->ReleaseSnapshot(snapshot);
  ReadOptions read;
  read.snapshot = snapshot;
  std::string value;
  EXPECT_DEATH(static_cast<void>(db->Get(read, "k", &value)), "released twice");
}

TEST(DBBasicDeathTest, ReleasingASnapshotOfAnotherDatabaseAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  FaultInjectionEnv env(3);
  Options options;
  options.env = &env;
  options.scheduler = nullptr;
  options.create_if_missing = true;
  std::unique_ptr<DB> one;
  std::unique_ptr<DB> two;
  ASSERT_OK(DB::Open(options, "/one", &one));
  ASSERT_OK(DB::Open(options, "/two", &two));
  const Snapshot* snapshot = one->GetSnapshot();
  EXPECT_DEATH(two->ReleaseSnapshot(snapshot), "did not create it");
  one->ReleaseSnapshot(snapshot);
}

TEST(DBBasicDeathTest, ReadingAtASnapshotOfAnotherDatabaseAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  FaultInjectionEnv env(3);
  Options options;
  options.env = &env;
  options.create_if_missing = true;
  std::unique_ptr<DB> one;
  std::unique_ptr<DB> two;
  ASSERT_OK(DB::Open(options, "/one", &one));
  ASSERT_OK(DB::Open(options, "/two", &two));
  const Snapshot* snapshot = one->GetSnapshot();
  ReadOptions read;
  read.snapshot = snapshot;
  std::string value;
  EXPECT_DEATH(static_cast<void>(two->Get(read, "k", &value)), "did not create it");
  one->ReleaseSnapshot(snapshot);
}

TEST(DBBasicDeathTest, ClosingWithAnUnreleasedSnapshotAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  FaultInjectionEnv env(3);
  Options options;
  options.env = &env;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(options, "/db", &db));
  const Snapshot* snapshot = db->GetSnapshot();
  EXPECT_DEATH(db.reset(), "1 unreleased snapshot");
  // The statement ran in a child process; here the database is still open.
  db->ReleaseSnapshot(snapshot);
}

}  // namespace
}  // namespace shale
