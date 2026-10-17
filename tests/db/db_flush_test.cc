// Writing the memtable out as a level 0 table: when the memtable is switched, what the flush does
// and leaves behind, which files are deleted and which are never touched, and what a failure
// anywhere in it costs.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/db_impl.h"
#include "db/db_test_util.h"
#include "db/handle_check_env.h"
#include "db/version_set.h"
#include "shale/db.h"
#include "shale/options.h"
#include "shale/status.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/filename.h"

namespace shale {
namespace {

constexpr char kDb[] = "/db";

using Op = FaultInjectionEnv::Op;

class DBFlushTest : public DBTestBase {
 protected:
  // The smallest write buffer there is, so that a few kilobytes of writes fill it.
  Options SmallBuffer() {
    Options options = CurrentOptions();
    options.write_buffer_size = 16384;
    return options;
  }

  // The bare names of the files of `type` in the directory.
  std::vector<std::string> FilesOfType(FileType type) {
    std::vector<std::string> names;
    for (const std::string& name : DbFiles()) {
      uint64_t number = 0;
      FileType found = FileType::kCurrentFile;
      if (ParseFileName(name, &number, &found) && found == type) {
        names.push_back(name);
      }
    }
    return names;
  }

  int CountFiles(FileType type) { return static_cast<int>(FilesOfType(type).size()); }

  // The numbers of the tables that the current version names.
  std::set<uint64_t> VersionTables() {
    std::set<uint64_t> numbers;
    const VersionState state = dbfull()->TEST_VersionState();
    for (const std::vector<FileMetaData>& level : state.files) {
      for (const FileMetaData& file : level) {
        numbers.insert(file.number);
      }
    }
    return numbers;
  }

  // The value that PutUntilSwitched() and the tests that fill the buffer write for key `i`.
  static std::string KiB(int i) { return std::string(1024, 'v') + ValueOf(i); }

  // Writes entries 0, 1, ... without sync, 1 KiB each, until the memtable has been switched, which
  // is when a second log appears, and returns how many were written. The scheduler is not run, so
  // the switched memtable is still waiting to be flushed.
  int PutUntilSwitched() {
    int written = 0;
    while (CountFiles(FileType::kLogFile) < 2) {
      EXPECT_LT(written, 100) << "the memtable never switched";
      if (written >= 100) {
        break;
      }
      EXPECT_OK(Put(UserKey(written), KiB(written), /*sync=*/false));
      written++;
    }
    return written;
  }

  void ExpectPresent(int begin, int end) {
    for (int i = begin; i < end; i++) {
      EXPECT_TRUE(Get(UserKey(i)) == KiB(i)) << "entry " << i;
    }
  }

  // Replaces the Env with a new one with an empty directory, and a database in it.
  void StartOver(uint64_t seed) {
    Close();
    env_ = std::make_unique<FaultInjectionEnv>(seed);
    Reopen();
  }
};

TEST_F(DBFlushTest, FlushCreatesLevel0File) {
  ASSERT_OK(Put("a", "1"));
  const std::vector<std::string> before = FilesOfType(FileType::kLogFile);
  ASSERT_EQ(before.size(), 1u);
  EXPECT_EQ(NumTableFilesAtLevel(0), 0);

  ASSERT_OK(db_->Flush());
  EXPECT_EQ(NumTableFilesAtLevel(0), 1);
  EXPECT_EQ(Get("a"), "1");
  const std::vector<std::string> after = FilesOfType(FileType::kLogFile);
  ASSERT_EQ(after.size(), 1u) << "the log of the flushed memtable should be gone";
  EXPECT_NE(after[0], before[0]) << "the write-ahead log was not switched";

  const VersionState state = dbfull()->TEST_VersionState();
  ASSERT_EQ(state.files[0].size(), 1u);
  const DBCounters counters = dbfull()->TEST_Counters();
  EXPECT_EQ(counters.flushes, 1u);
  EXPECT_EQ(counters.flush_bytes_written, state.files[0][0].file_size);
  // The MANIFEST now names the log that is in use as the oldest that matters.
  uint64_t number = 0;
  FileType type = FileType::kCurrentFile;
  ASSERT_TRUE(ParseFileName(after[0], &number, &type));
  EXPECT_EQ(state.log_number, number);
}

TEST_F(DBFlushTest, FlushOfEmptyMemtableIsNoOp) {
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(CountFiles(FileType::kTableFile), 0);
  EXPECT_EQ(NumTableFilesAtLevel(0), 0);
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  EXPECT_EQ(dbfull()->TEST_Counters().flushes, 0u);
  ASSERT_OK(db_->Flush());  // and again
  EXPECT_EQ(CountFiles(FileType::kTableFile), 0);
}

TEST_F(DBFlushTest, FullMemtableSwitchesAutomatically) {
  Reopen(SmallBuffer());
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
  }
  scheduler_.RunPending();
  EXPECT_GE(NumTableFilesAtLevel(0), 1);
  EXPECT_GE(dbfull()->TEST_Counters().flushes, 1u);
  ExpectPresent(0, 200);
  Reopen(SmallBuffer());
  ExpectPresent(0, 200);
}

TEST_F(DBFlushTest, NewerMemtableValueWinsOverTable) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Put("a", "2"));
  EXPECT_EQ(Get("a"), "2");
  ASSERT_OK(Delete("a"));
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(Get("a"), "NOT_FOUND")
      << "the deletion in the newer table hides the value in the older";
  ASSERT_OK(Put("a", "3"));
  EXPECT_EQ(Get("a"), "3");
}

TEST_F(DBFlushTest, RecoveredMemtableIsFlushedAtOpen) {
  for (int i = 0; i < 20; i++) {
    ASSERT_OK(Put(UserKey(i), ValueOf(i)));
  }
  Close();
  const std::vector<std::string> old_logs = FilesOfType(FileType::kLogFile);
  ASSERT_EQ(old_logs.size(), 1u);
  Reopen();
  EXPECT_EQ(NumTableFilesAtLevel(0), 1);
  const std::vector<std::string> logs = FilesOfType(FileType::kLogFile);
  ASSERT_EQ(logs.size(), 1u) << "a log older than the new one remains";
  EXPECT_NE(logs[0], old_logs[0]);
  for (int i = 0; i < 20; i++) {
    EXPECT_EQ(Get(UserKey(i)), ValueOf(i));
  }
  EXPECT_EQ(dbfull()->TEST_Counters().flushes, 1u);
}

TEST_F(DBFlushTest, ObsoleteFilesAreDeleted) {
  for (int round = 0; round < 5; round++) {
    for (int i = 0; i < 10; i++) {
      ASSERT_OK(Put(UserKey(round * 10 + i), ValueOf(round * 10 + i)));
    }
    ASSERT_OK(db_->Flush());
  }
  const std::set<uint64_t> tables = VersionTables();
  EXPECT_EQ(tables.size(), 5u);
  for (const std::string& name : FilesOfType(FileType::kTableFile)) {
    uint64_t number = 0;
    FileType type = FileType::kCurrentFile;
    ASSERT_TRUE(ParseFileName(name, &number, &type));
    EXPECT_EQ(tables.count(number), 1u) << name << " is in no version";
  }
  EXPECT_EQ(CountFiles(FileType::kTableFile), 5);
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  EXPECT_EQ(CountFiles(FileType::kDescriptorFile), 1);
  for (int i = 0; i < 50; i++) {
    EXPECT_EQ(Get(UserKey(i)), ValueOf(i));
  }
}

TEST_F(DBFlushTest, OrphanTableIsRemovedAtOpenUnknownFileIsKept) {
  ASSERT_OK(Put("a", "1"));
  Close();
  env_->SetFileContents("/db/000999.sst", "x");
  env_->SetFileContents("/db/notes.txt", "y");
  Reopen();
  EXPECT_FALSE(env_->FileExists("/db/000999.sst")) << "an orphan table survived the open";
  EXPECT_EQ(env_->FileContents("/db/notes.txt"), "y");
  EXPECT_EQ(Get("a"), "1");
}

TEST_F(DBFlushTest, ReopenCyclesDoNotAccumulateFiles) {
  int next = 0;
  for (int cycle = 0; cycle < 50; cycle++) {
    Reopen();
    for (int i = 0; i < 10; i++, next++) {
      ASSERT_OK(Put(UserKey(next), ValueOf(next)));
    }
  }
  // CURRENT, LOCK, LOG, LOG.old, one MANIFEST and one log, and the tables the version names.
  const size_t tables = VersionTables().size();
  EXPECT_LE(DbFiles().size(), tables + 6) << "files accumulate over reopens";
  EXPECT_EQ(static_cast<size_t>(CountFiles(FileType::kTableFile)), tables);
  for (int i = 0; i < next; i++) {
    EXPECT_EQ(Get(UserKey(i)), ValueOf(i)) << "entry " << i;
  }
}

TEST_F(DBFlushTest, FlushFailureSetsBackgroundError) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  env_->FailOpAfter(Op::kNewWritable, 0);
  const Status s = db_->Flush();
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  const Status put = Put("c", "3");
  EXPECT_TRUE(put.IsIOError()) << put.ToString();
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "2");
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
}

TEST_F(DBFlushTest, FailureToWriteTheTableIsABackgroundError) {
  // The new log is the first file the flush creates and the table the second: this fails the table.
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  env_->FailOpAfter(Op::kNewWritable, 1);
  const Status s = db_->Flush();
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
  EXPECT_EQ(dbfull()->TEST_Counters().flushes, 0u);
  EXPECT_EQ(CountFiles(FileType::kTableFile), 0) << "a partial table was left";
  const Status put = Put("c", "3");
  EXPECT_TRUE(put.IsIOError()) << "writes must stop: " << put.ToString();
  EXPECT_EQ(put.ToString(), s.ToString()) << "every write returns the first error";
  // The memtable that failed to flush is still read.
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "2");
  EXPECT_TRUE(db_->Flush().IsIOError()) << "Flush returns the error that stopped writes";

  // And nothing was lost: the logs are still there for the next open.
  env_->ClearFaults();
  Reopen();
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "2");
  ASSERT_OK(Put("c", "3"));
}

TEST_F(DBFlushTest, ABackgroundErrorStopsTheSchedulingOfMoreWork) {
  ASSERT_OK(Put("a", "1"));
  env_->FailOpAfter(Op::kNewWritable, 1);
  EXPECT_TRUE(db_->Flush().IsIOError());
  env_->ClearFaults();
  EXPECT_EQ(scheduler_.pending(), 0u);
  EXPECT_EQ(scheduler_.RunPending(), 0u);
  EXPECT_EQ(NumTableFilesAtLevel(0), 0) << "the database retried the flush by itself";
}

TEST_F(DBFlushTest, AFailureAnywhereInAFlushLosesNothing) {
  // Every call that a flush makes through the Env fails in turn, and the process or the machine
  // then dies. The flush that failed may have left its table, and the MANIFEST that was being
  // appended to may or may not hold the edit that names it: a table that the edit names has to be
  // there at the next open, so a failed flush must not delete what it wrote. And whatever else
  // happens, the writes that were acknowledged are in the next open.
  const std::vector<Op> ops = {Op::kNewWritable,     Op::kAppend, Op::kFlush,        Op::kSync,
                               Op::kClose,           Op::kRename, Op::kSyncDir,      Op::kRemove,
                               Op::kNewRandomAccess, Op::kRead,   Op::kNewSequential};
  enum class Crash { kProcess, kPowerRandom, kPowerDropUnsynced };
  int failed_flushes = 0;
  for (const Crash crash : {Crash::kProcess, Crash::kPowerRandom, Crash::kPowerDropUnsynced}) {
    for (const Op op : ops) {
      for (int n = 0; n < 12; n++) {
        StartOver(static_cast<uint64_t>(n) + 1);
        for (int i = 0; i < 5; i++) {
          ASSERT_OK(Put(UserKey(i), ValueOf(i)));
        }
        ASSERT_OK(db_->Flush());
        for (int i = 5; i < 10; i++) {
          ASSERT_OK(Put(UserKey(i), ValueOf(i)));
        }
        env_->FailOpAfter(op, n);
        const uint64_t injected_before = env_->injected_errors();
        const Status s = db_->Flush();
        const bool fired = env_->injected_errors() > injected_before;
        env_->ClearFaults();  // one that did not fire would fail the reads below instead
        if (fired && !s.ok()) {
          failed_flushes++;
          EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
          EXPECT_FALSE(Put("more", "x").ok());
        }
        // Whether it failed or not, what was acknowledged is readable now.
        for (int i = 0; i < 10; i++) {
          EXPECT_EQ(Get(UserKey(i)), ValueOf(i));
        }
        switch (crash) {
          case Crash::kProcess:
            env_->SimulateProcessCrash();
            break;
          case Crash::kPowerRandom:
            env_->SimulatePowerLoss();
            break;
          case Crash::kPowerDropUnsynced:
            env_->SimulatePowerLossDropUnsynced();
            break;
        }
        Reopen();
        for (int i = 0; i < 10; i++) {
          EXPECT_EQ(Get(UserKey(i)), ValueOf(i))
              << "entry " << i << " after the failure of call " << n;
        }
        // The open's sweep leaves exactly the tables the version names.
        const std::set<uint64_t> tables = VersionTables();
        for (const std::string& name : FilesOfType(FileType::kTableFile)) {
          uint64_t number = 0;
          FileType type = FileType::kCurrentFile;
          ASSERT_TRUE(ParseFileName(name, &number, &type));
          EXPECT_EQ(tables.count(number), 1u) << name << " is in no version";
        }
        for (const uint64_t number : tables) {
          EXPECT_TRUE(env_->FileExists(TableFileName(kDb, number)))
              << "the version names table " << number << ", which is not there";
        }
        if (!fired) {
          break;  // a flush makes fewer calls of this kind than n + 1: they have all been tried
        }
      }
    }
  }
  EXPECT_GT(failed_flushes, 20) << "the sweep hardly made a flush fail";
}

TEST_F(DBFlushTest, FailureToSwitchTheLogClosesTheNewFile) {
  HandleCheckEnv handle_env(env_.get());
  Options options = CurrentOptions();
  options.env = &handle_env;
  const struct Cleanup {
    std::function<void()> close;
    ~Cleanup() { close(); }
  } cleanup{[this] { Close(); }};
  Reopen(options);
  ASSERT_OK(Put("a", "1"));
  env_->FailOpAfter(Op::kSyncDir, 0);  // the first directory sync is the one for the new log
  EXPECT_TRUE(db_->Flush().IsIOError());
  env_->ClearFaults();
  EXPECT_EQ(Get("a"), "1");
  Close();
  EXPECT_EQ(handle_env.leaked(), 0) << "a file was dropped without Close()";
  EXPECT_GT(handle_env.opened(), 0);
}

TEST_F(DBFlushTest, TheLogIsSyncedBeforeTheSwitch) {
  // Writes that were not synced are in the old log. If the switch left that log unsynced, a later
  // synced write in the new log could survive a power loss while the earlier ones, in the old,
  // were lost: a hole, which the guarantee for unsynced writes rules out.
  Reopen(SmallBuffer());
  const int written = PutUntilSwitched();
  ASSERT_GT(written, 1);
  ASSERT_EQ(NumTableFilesAtLevel(0), 0) << "the flush ran, which would make the writes durable";
  ASSERT_EQ(CountFiles(FileType::kLogFile), 2);
  ASSERT_OK(Put("synced", "1", /*sync=*/true));
  env_->SimulatePowerLossDropUnsynced();
  Reopen(SmallBuffer());
  EXPECT_EQ(Get("synced"), "1");
  ExpectPresent(0, written);
}

TEST_F(DBFlushTest, FlushedDataSurvivesPowerLoss) {
  for (int i = 0; i < 10; i++) {
    ASSERT_OK(Put(UserKey(i), ValueOf(i)));
  }
  ASSERT_OK(db_->Flush());
  env_->SimulatePowerLossDropUnsynced();
  Reopen();
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(Get(UserKey(i)), ValueOf(i));
  }
}

TEST_F(DBFlushTest, ReadsSeeTheMemtableThatIsWaitingToBeFlushed) {
  Reopen(SmallBuffer());
  const int written = PutUntilSwitched();
  ASSERT_GT(written, 2);
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  ExpectPresent(0, written);
  // Writes to the new memtable go over what the old one holds.
  ASSERT_OK(Put(UserKey(0), "new"));
  ASSERT_OK(Delete(UserKey(1)));
  EXPECT_EQ(Get(UserKey(0)), "new");
  EXPECT_EQ(Get(UserKey(1)), "NOT_FOUND");
  ExpectPresent(2, written);

  scheduler_.RunPending();
  EXPECT_EQ(NumTableFilesAtLevel(0), 1);
  EXPECT_EQ(Get(UserKey(0)), "new");
  EXPECT_EQ(Get(UserKey(1)), "NOT_FOUND");
  ExpectPresent(2, written);
}

TEST_F(DBFlushTest, FlushWaitsForTheMemtableThatIsAlreadyWaiting) {
  Reopen(SmallBuffer());
  const int written = PutUntilSwitched();
  ASSERT_OK(Put("late", "1"));
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(NumTableFilesAtLevel(0), 2) << "both memtables should be tables now";
  EXPECT_EQ(dbfull()->TEST_Counters().flushes, 2u);
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  ExpectPresent(0, written);
  EXPECT_EQ(Get("late"), "1");
}

TEST_F(DBFlushTest, SnapshotsReadTheirOwnStateAcrossAFlush) {
  ASSERT_OK(Put("a", "1"));
  const Snapshot* snapshot = db_->GetSnapshot();
  // EXPECT, not ASSERT: a snapshot that is not released aborts the close.
  EXPECT_OK(Put("a", "2"));
  EXPECT_OK(db_->Flush());
  EXPECT_OK(Put("a", "3"));
  EXPECT_EQ(Get("a", snapshot), "1");
  EXPECT_EQ(Get("a"), "3");
  EXPECT_OK(db_->Flush());
  EXPECT_EQ(Get("a", snapshot), "1");
  EXPECT_EQ(Get("a"), "3");
  db_->ReleaseSnapshot(snapshot);
}

TEST_F(DBFlushTest, ClosingWithBackgroundWorkQueuedDropsItAndLosesNothing) {
  Reopen(SmallBuffer());
  const int written = PutUntilSwitched();
  ASSERT_EQ(scheduler_.pending(), 1u) << "the switch should have queued the flush";
  Close();  // does not flush: the data is in the logs
  EXPECT_EQ(scheduler_.pending(), 0u) << "the queued task still refers to the closed database";
  Reopen(SmallBuffer());
  ExpectPresent(0, written);
}

TEST_F(DBFlushTest, FlushWorksOnAThreadScheduler) {
  Options options = SmallBuffer();
  options.scheduler = nullptr;  // the database runs its own
  Reopen(options);
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
  }
  ASSERT_OK(Put("last", "1"));
  ASSERT_OK(db_->Flush());
  EXPECT_GE(NumTableFilesAtLevel(0), 1);
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  ExpectPresent(0, 200);
  EXPECT_EQ(Get("last"), "1");
  ASSERT_OK(Put("after", "1"));
  Reopen(options);  // closes with background work possibly still queued
  ExpectPresent(0, 200);
  EXPECT_EQ(Get("after"), "1");
}

TEST_F(DBFlushTest, FlushFailureOnAThreadSchedulerWakesTheWaiter) {
  Options options = CurrentOptions();
  options.scheduler = nullptr;
  Reopen(options);
  ASSERT_OK(Put("a", "1"));
  env_->FailOpAfter(Op::kNewWritable, 1);
  EXPECT_TRUE(db_->Flush().IsIOError());
  EXPECT_TRUE(Put("b", "2").IsIOError());
  EXPECT_EQ(Get("a"), "1");
}

TEST_F(DBFlushTest, TheSweepLeavesFilesItDoesNotKnowAndFilesBeyondTheCeilingAlone) {
  const std::vector<std::pair<std::string, std::string>> others = {
      {"/db/notes.txt", "mine"},
      {"/db/MANIFEST-oops", "not a manifest"},
      // A log that a lenient open set aside, and the names a sweep must never read as the
      // database's own: nothing it makes is numbered at or above the ceiling.
      {"/db/000003.log.dropped", "dropped"},
      {TableFileName(kDb, kMaxFileNumber), "beyond"},
      {LogFileName(kDb, kMaxFileNumber), "beyond"},
      {LogFileName(kDb, std::numeric_limits<uint64_t>::max()), "beyond"},
      {DescriptorFileName(kDb, kMaxFileNumber + 7), "beyond"},
      {TempFileName(kDb, kMaxFileNumber), "beyond"},
  };
  for (const auto& [name, contents] : others) {
    env_->SetFileContents(name, contents);
  }
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(db_->Flush());
  for (const auto& [name, contents] : others) {
    EXPECT_EQ(env_->FileContents(name), contents) << name << " was touched by a flush";
  }
  Reopen();  // and by an open
  for (const auto& [name, contents] : others) {
    EXPECT_EQ(env_->FileContents(name), contents) << name << " was touched by an open";
  }
}

TEST_F(DBFlushTest, TheSweepRemovesAStrayTemporaryFile) {
  Close();
  env_->SetFileContents(TempFileName(kDb, 4), "half a CURRENT");
  Reopen();
  EXPECT_FALSE(env_->FileExists(TempFileName(kDb, 4)));
}

TEST_F(DBFlushTest, ExhaustedFileNumbersFailTheFlushInsteadOfAborting) {
  // The log of a switch and the table of a flush take numbers, and a number at the ceiling is one
  // that no MANIFEST holds. The flush that cannot get one fails, as an error that stops writes.
  Close();
  ASSERT_OK(DestroyDB(dbname_, CurrentOptions()));
  ASSERT_OK(ForgeEmptyDatabase(env_.get(), kDb, 0, kMaxFileNumber - 6));
  Reopen();
  ASSERT_OK(Put("a", "1"));
  Status s;
  for (int i = 0; i < 6 && s.ok(); i++) {
    s = db_->Flush();
    if (s.ok()) {
      ASSERT_OK(Put(UserKey(i), ValueOf(i)));
    }
  }
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_FALSE(Put("b", "2").ok());
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 1u);
}

}  // namespace
}  // namespace shale
