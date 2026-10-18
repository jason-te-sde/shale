// Writing the memtable out as a level 0 table: when the memtable is switched, what the flush does
// and leaves behind, which files are deleted and which are never touched, and what a failure
// anywhere in it costs.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/db_impl.h"
#include "db/db_test_util.h"
#include "db/handle_check_env.h"
#include "db/hook_env.h"
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

TEST_F(DBFlushTest, FailureToCreateTheNextLogStopsWritesAndLeavesNoFileOpen) {
  HandleCheckEnv handle_env(env_.get());
  Options options = CurrentOptions();
  options.env = &handle_env;
  const struct Cleanup {
    std::function<void()> close;
    ~Cleanup() { close(); }
  } cleanup{[this] { Close(); }};
  Reopen(options);
  ASSERT_OK(Put("a", "1"));
  env_->FailOpAfter(Op::kNewWritable, 0);  // the next file made is the log of the switch
  EXPECT_TRUE(db_->Flush().IsIOError());
  env_->ClearFaults();
  EXPECT_TRUE(Put("b", "2").IsIOError()) << "a database that cannot make its next log stops";
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  Close();
  EXPECT_EQ(handle_env.leaked(), 0) << "a file was dropped without Close()";
  EXPECT_GT(handle_env.opened(), 0);
  Reopen();
  EXPECT_EQ(Get("a"), "1");
}

TEST_F(DBFlushTest, ASyncedWriteMakesTheWritesInTheOldLogDurable) {
  // Writes that were not synced are in the old log, and a synced write in the new one promises
  // that they survive a power loss with it.
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

// The calls that sync something, in order, as "sync <file>" and "sync_dir", with the directory
// left out of the file names.
class SyncRecorder {
 public:
  explicit SyncRecorder(HookEnv* env) : env_(env) {
    env_->SetHook([this](std::string_view call, const std::string& path) {
      if (call == "sync" || call == "sync_dir") {
        const std::lock_guard<std::mutex> lock(mu_);
        calls_.push_back(call == "sync" ? "sync " + path.substr(path.rfind('/') + 1)
                                        : std::string(call));
      }
      return Status::OK();
    });
  }
  ~SyncRecorder() { env_->SetHook(nullptr); }
  SyncRecorder(const SyncRecorder&) = delete;
  SyncRecorder& operator=(const SyncRecorder&) = delete;

  // Returns what was recorded since the last call, and forgets it.
  std::vector<std::string> Take() {
    const std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> taken;
    taken.swap(calls_);
    return taken;
  }

 private:
  HookEnv* const env_;
  std::mutex mu_;
  std::vector<std::string> calls_;  // guarded by mu_
};

TEST_F(DBFlushTest, ASwitchSyncsNeitherTheOldLogNorTheDirectory) {
  // The writes before the switch were not synced, and nothing has promised that they are durable:
  // the switch leaves the syncs to the first write that asks for one, which is the only thing that
  // can be told they were made, and to the flush, which makes them durable another way.
  Options options = SmallBuffer();
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  Reopen(options);
  SyncRecorder syncs(hook_env);
  const int written = PutUntilSwitched();
  ASSERT_GT(written, 1);
  EXPECT_TRUE(syncs.Take().empty()) << "the switch synced something";
  ExpectPresent(0, written);
}

TEST_F(DBFlushTest, ASyncedWriteAfterASwitchSyncsTheOldLogAndTheDirectoryFirst) {
  // A write that is acknowledged as synced promises that it and every write before it survive a
  // power loss. After a switch the writes before it are in the old log, unsynced, and the new log's
  // directory entry is not durable either: both are synced before the write's own log is.
  Options options = SmallBuffer();
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  Reopen(options);
  const std::vector<std::string> before = FilesOfType(FileType::kLogFile);
  ASSERT_EQ(before.size(), 1u);
  const int written = PutUntilSwitched();
  ASSERT_EQ(NumTableFilesAtLevel(0), 0) << "the flush ran, which would make the writes durable";
  const std::vector<std::string> after = FilesOfType(FileType::kLogFile);
  ASSERT_EQ(after.size(), 2u);
  const std::string old_log = before[0];
  const std::string new_log = after[0] == old_log ? after[1] : after[0];

  SyncRecorder syncs(hook_env);
  ASSERT_OK(Put("synced", "1", /*sync=*/true));
  EXPECT_EQ(syncs.Take(),
            (std::vector<std::string>{"sync " + old_log, "sync_dir", "sync " + new_log}));
  ASSERT_OK(Put("synced again", "2", /*sync=*/true));
  EXPECT_EQ(syncs.Take(), std::vector<std::string>{"sync " + new_log})
      << "what the first synced write made durable is synced again";

  env_->SimulatePowerLossDropUnsynced();
  Reopen(options);
  ExpectPresent(0, written);
  EXPECT_EQ(Get("synced"), "1");
  EXPECT_EQ(Get("synced again"), "2");
}

TEST_F(DBFlushTest, AFlushLeavesASyncedWriteNothingButItsOwnLogToSync) {
  // The flush of the old memtable makes its writes durable in a table, and syncs the directory
  // after the new log was made, so a synced write after it has nothing of the switch left to do.
  Options options = SmallBuffer();
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  Reopen(options);
  const int written = PutUntilSwitched();
  scheduler_.RunPending();
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  const std::vector<std::string> logs = FilesOfType(FileType::kLogFile);
  ASSERT_EQ(logs.size(), 1u) << "the old log outlived the flush of its memtable";
  SyncRecorder syncs(hook_env);
  ASSERT_OK(Put("synced", "1", /*sync=*/true));
  EXPECT_EQ(syncs.Take(), std::vector<std::string>{"sync " + logs[0]});
  env_->SimulatePowerLossDropUnsynced();
  Reopen(options);
  ExpectPresent(0, written);
  EXPECT_EQ(Get("synced"), "1");
}

TEST_F(DBFlushTest, AFailedSyncOfTheDirectoryForASyncedWriteFailsItAndStopsWrites) {
  Reopen(SmallBuffer());
  const int written = PutUntilSwitched();
  env_->FailOpAfter(Op::kSyncDir, 0);
  EXPECT_TRUE(Put("synced", "1", /*sync=*/true).IsIOError());
  env_->ClearFaults();
  EXPECT_TRUE(Put("after", "2", /*sync=*/false).IsIOError()) << "the error does not stop writes";
  ExpectPresent(0, written);
  Reopen(SmallBuffer());
  ExpectPresent(0, written);
  EXPECT_EQ(Get("after"), "NOT_FOUND");
}

TEST_F(DBFlushTest, AFailedSyncOfTheOldLogForASyncedWriteFailsItAndStopsWrites) {
  Reopen(SmallBuffer());
  const int written = PutUntilSwitched();
  env_->FailOpAfter(Op::kSync, 0);
  EXPECT_TRUE(Put("synced", "1", /*sync=*/true).IsIOError());
  env_->ClearFaults();
  EXPECT_TRUE(Put("after", "2", /*sync=*/false).IsIOError()) << "the error does not stop writes";
  ExpectPresent(0, written);
  Reopen(SmallBuffer());
  ExpectPresent(0, written);
}

TEST_F(DBFlushTest, AFlushRecordsTheNewestWriteOfItsTableAndNotOfTheDatabase) {
  // The MANIFEST's sequence number is what recovery holds the first write of the logs it replays
  // to: every write up to it is in a table, and the logs go on from the one after. A flush that
  // recorded the database's newest write, which may be in the new log already, would put that
  // bound past writes that are only in the log.
  Reopen(SmallBuffer());
  const int written = PutUntilSwitched();  // the last of them is in the new log
  const SequenceNumber switched_at = dbfull()->TEST_LastSequence() - 1;
  ASSERT_OK(Put("later", "1", /*sync=*/false));
  ASSERT_GT(dbfull()->TEST_LastSequence(), switched_at + 1);
  scheduler_.RunPending();
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  EXPECT_EQ(dbfull()->TEST_VersionState().last_sequence, switched_at);
  Reopen(SmallBuffer());
  ExpectPresent(0, written);
  EXPECT_EQ(Get("later"), "1");
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
  ExpectPresent(0, 200);
  EXPECT_EQ(Get("last"), "1");
  ASSERT_OK(Put("after", "1"));
  // A flush returns when its memtable is a table, and the task goes on to delete the logs that
  // this made obsolete: closing waits for it.
  Close();
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  Reopen(options);
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

TEST_F(DBFlushTest, AKeyThatIsOnlyInATableIsFoundWhileAMemtableIsWaiting) {
  // Reads look at `mem_`, then `imm_`, then the tables, and a read that stopped at `imm_` would
  // miss what only a table has.
  Reopen(SmallBuffer());
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 1);
  const int written = PutUntilSwitched();
  ASSERT_GT(written, 1);
  ASSERT_EQ(NumTableFilesAtLevel(0), 1) << "the flush ran";
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "NOT_FOUND");
  ExpectPresent(0, written);
}

TEST_F(DBFlushTest, NoLogIsSyncedTwiceAndNoSwitchSyncsOne) {
  // A log is synced by the writes that ask for it and by nothing else: a sync is a flush of the
  // disk's cache, and a log whose memtable is flushed to a table is durable without one.
  Options options = CurrentOptions();
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  Reopen(options);
  int log_syncs = 0;
  hook_env->SetHook([&](std::string_view call, const std::string& path) {
    if (call == "sync" && path.ends_with(".log")) {
      log_syncs++;
    }
    return Status::OK();
  });
  ASSERT_OK(Put("a", "1"));  // one sync, its own
  ASSERT_OK(db_->Flush());   // a switch
  EXPECT_EQ(log_syncs, 1) << "the switch synced a log that was synced";

  ASSERT_OK(Put("b", "2", /*sync=*/false));
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(log_syncs, 1) << "the flush made the write that was not synced durable in a table";

  ASSERT_OK(Put("c", "3", /*sync=*/false));
  ASSERT_OK(Put("d", "4", /*sync=*/true));  // makes "c" durable too
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(log_syncs, 2)
      << "the sync of a later write covers the earlier, and the switch has none";

  ASSERT_OK(db_->Flush());  // nothing in the memtable: no switch
  EXPECT_EQ(log_syncs, 2);
  hook_env->SetHook(nullptr);
  // And what was written is where a power loss leaves it.
  env_->SimulatePowerLossDropUnsynced();
  Reopen(options);
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "2");
  EXPECT_EQ(Get("c"), "3");
  EXPECT_EQ(Get("d"), "4");
}

TEST_F(DBFlushTest, AFailureToListTheDirectoryIsLoggedAndTriedAgainLater) {
  Options options = CurrentOptions();
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  Reopen(options);
  hook_env->SetHook([](std::string_view call, const std::string& /*path*/) {
    return call == "get_children" ? Status::IOError("injected listing failure") : Status::OK();
  });
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(db_->Flush());
  hook_env->SetHook(nullptr);
  EXPECT_EQ(dbfull()->TEST_Counters().bg_errors, 0u) << "an obsolete file is not an error";
  EXPECT_NE(env_->FileContents("/db/LOG").find("could not list"), std::string::npos)
      << "the failed listing was not logged";
  EXPECT_EQ(CountFiles(FileType::kLogFile), 2) << "the sweep that could not list deleted a log";
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1) << "the next sweep did not delete it";
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "2");
}

TEST_F(DBFlushTest, AStalledWriterIsWokenBeforeTheSweepListsTheDirectory) {
  // The memtable is full, a flush is under way, and a write is waiting for it. The flush clears
  // `imm_` and then sweeps, listing and deleting files with the disk to wait for, and the write
  // must not wait for that. The sweep here does not list until the write is through.
  Options options = SmallBuffer();
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  options.scheduler = nullptr;  // the database runs its own: this needs a thread
  Reopen(options);
  std::mutex mu;
  std::condition_variable cv;
  bool table_may_be_built = false;  // guarded by mu
  bool sweep_armed = false;         // guarded by mu
  int puts_done = 0;                // guarded by mu
  int puts_when_released = 0;       // guarded by mu
  bool sweep_timed_out = false;     // guarded by mu
  const auto patience = std::chrono::seconds(10);
  hook_env->SetHook([&](std::string_view call, const std::string& path) {
    std::unique_lock<std::mutex> lock(mu);
    if (call == "new_writable" && path.ends_with(".sst")) {
      cv.wait_for(lock, patience, [&] { return table_may_be_built; });
    } else if (call == "get_children" && sweep_armed) {
      // The write that was stalled goes on once `imm_` is gone, and that is before this.
      if (!cv.wait_for(lock, std::chrono::seconds(3),
                       [&] { return puts_done > puts_when_released; })) {
        sweep_timed_out = true;
      }
    }
    return Status::OK();
  });

  constexpr int kPuts = 40;  // the memtable holds some sixteen: the second switch has to wait
  std::thread writer([&] {
    for (int i = 0; i < kPuts; i++) {
      EXPECT_OK(Put(UserKey(i), KiB(i), /*sync=*/false));
      const std::lock_guard<std::mutex> lock(mu);
      puts_done++;
      cv.notify_all();
    }
  });
  // The writer stops when it has filled the memtable that follows the one being flushed, which is
  // held up at its table: wait until it has not moved for a while.
  int last = -1;
  for (int stable = 0; stable < 20;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const std::lock_guard<std::mutex> lock(mu);
    stable = puts_done == last ? stable + 1 : 0;
    last = puts_done;
  }
  {
    const std::lock_guard<std::mutex> lock(mu);
    puts_when_released = puts_done;
    sweep_armed = true;
    table_may_be_built = true;
  }
  cv.notify_all();
  writer.join();
  hook_env->SetHook(nullptr);
  {
    const std::lock_guard<std::mutex> lock(mu);
    EXPECT_LT(puts_when_released, kPuts) << "the writer was never stalled";
    EXPECT_FALSE(sweep_timed_out) << "the sweep waited for a write that was waiting for the sweep";
  }
  ExpectPresent(0, kPuts);
}

TEST_F(DBFlushTest, WritesAndReadsGoOnWhileTheManifestIsSynced) {
  // The background task gives up the mutex while it appends an edit to the MANIFEST and syncs it.
  // The sync is a flush of the disk's cache, and a write or a read that came meanwhile would
  // otherwise wait for the disk too. Here the sync of the flush's edit is held until a write and a
  // read have been made, or for three seconds, which is what a mutex held through it would cost.
  Options options = CurrentOptions();
  HookEnv* const hook_env = NewHookEnv();
  options.env = hook_env;
  options.scheduler = nullptr;  // the database runs its own: this needs a thread
  Reopen(options);
  ASSERT_OK(Put("a", "1"));
  std::mutex mu;
  std::condition_variable cv;
  bool syncing = false;    // guarded by mu
  bool done = false;       // guarded by mu
  bool timed_out = false;  // guarded by mu
  hook_env->SetHook([&](std::string_view call, const std::string& path) {
    if (call == "sync" && path.find("MANIFEST") != std::string::npos) {
      std::unique_lock<std::mutex> lock(mu);
      syncing = true;
      cv.notify_all();
      if (!cv.wait_for(lock, std::chrono::seconds(3), [&] { return done; })) {
        timed_out = true;
      }
    }
    return Status::OK();
  });
  std::thread flusher([&] { EXPECT_OK(db_->Flush()); });
  bool reached = false;
  {
    std::unique_lock<std::mutex> lock(mu);
    reached = cv.wait_for(lock, std::chrono::seconds(10), [&] { return syncing; });
  }
  EXPECT_TRUE(reached) << "the flush never synced the MANIFEST";
  EXPECT_OK(Put("b", "2", /*sync=*/false));
  EXPECT_EQ(Get("a"), "1");
  {
    const std::lock_guard<std::mutex> lock(mu);
    done = true;
  }
  cv.notify_all();
  flusher.join();
  hook_env->SetHook(nullptr);
  {
    const std::lock_guard<std::mutex> lock(mu);
    EXPECT_FALSE(timed_out) << "the write or the read waited for the MANIFEST to be synced";
  }
  EXPECT_EQ(Get("b"), "2");
  EXPECT_EQ(NumTableFilesAtLevel(0), 1);
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
