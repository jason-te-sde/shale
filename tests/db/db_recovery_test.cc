// What Open makes of the files a previous run left: replaying the write-ahead log, telling a log
// cut short by a crash from one that is damaged, and the refusals that keep a forged or damaged
// directory from aborting the process.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/version_state.h"
#include "db/db_impl.h"
#include "db/db_test_util.h"
#include "db/version_set.h"
#include "io/write_batch_internal.h"
#include "shale/db.h"
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

constexpr char kDb[] = "/db";

using Op = FaultInjectionEnv::Op;

bool Contains(const Status& s, const std::string& text) {
  return s.ToString().find(text) != std::string::npos;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// The size of a log record's header.
constexpr uint64_t kLogHeaderSize = 7;

class DBRecoveryTest : public DBTestBase {
 protected:
  // Puts the entries `begin` up to `end`: UserKey(i) set to ValueOf(i).
  void PutRange(int begin, int end, bool sync = true) {
    for (int i = begin; i < end; i++) {
      ASSERT_OK(Put(UserKey(i), ValueOf(i), sync));
    }
  }

  void ExpectPresent(int begin, int end) {
    for (int i = begin; i < end; i++) {
      EXPECT_EQ(Get(UserKey(i)), ValueOf(i)) << "entry " << i;
    }
  }

  void ExpectAbsent(int begin, int end) {
    for (int i = begin; i < end; i++) {
      EXPECT_EQ(Get(UserKey(i)), "NOT_FOUND") << "entry " << i;
    }
  }

  // How many files of `type` the directory holds.
  int CountFiles(FileType type) {
    int count = 0;
    for (const std::string& name : DbFiles()) {
      uint64_t number = 0;
      FileType found = FileType::kCurrentFile;
      if (ParseFileName(name, &number, &found) && found == type) {
        count++;
      }
    }
    return count;
  }

  // Changes one bit of the byte at `offset` of `fname`.
  void FlipByte(const std::string& fname, uint64_t offset) {
    std::string contents = env_->FileContents(fname);
    ASSERT_LT(offset, contents.size());
    contents[offset] = static_cast<char>(contents[offset] ^ 0x01);
    env_->SetFileContents(fname, contents);
  }

  // Writes ten 20 KiB entries with synced writes, closes the database, and returns the offsets of
  // the ten records in its log. Each record is longer than half a block, so they straddle block
  // boundaries, which is where a damaged record takes its neighbours with it.
  std::vector<uint64_t> WriteTenBigEntriesAndClose(std::string* log) {
    for (int i = 0; i < 10; i++) {
      EXPECT_OK(Put(UserKey(i), std::string(20 * 1024, static_cast<char>('a' + i))));
    }
    Close();
    *log = NewestLogFile(env_.get(), kDb);
    std::vector<uint64_t> offsets = LogRecordOffsets(env_.get(), *log);
    EXPECT_EQ(offsets.size(), 10u);
    return offsets;
  }

  std::string BigValue(int i) { return std::string(20 * 1024, static_cast<char>('a' + i)); }

  // Whether entry `i` of WriteTenBigEntriesAndClose() is there, compared without printing 20 KiB.
  bool HasBigEntry(int i) { return Get(UserKey(i)) == BigValue(i); }

  // Replaces the database with an empty one, ready for forged logs to be put in its directory. The
  // database is closed.
  void StartOver() {
    Close();
    ASSERT_OK(DestroyDB(dbname_, CurrentOptions()));
    Reopen();
    Close();
  }
};

TEST_F(DBRecoveryTest, ReopenReplaysWal) {
  PutRange(0, 100);
  ASSERT_EQ(dbfull()->TEST_LastSequence(), 100u);
  Reopen();
  ExpectPresent(0, 100);
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 100u);
  ASSERT_OK(Put("after", "1"));
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 101u);
  // The log that was replayed is gone, and a new one is in use, as is a new MANIFEST.
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  EXPECT_EQ(CountFiles(FileType::kDescriptorFile), 1);
}

TEST_F(DBRecoveryTest, RecoveredDataSurvivesRepeatedReopens) {
  // The log is deleted once it has been replayed, so the data it held has to be somewhere else by
  // then: a reopen that kept it only in memory would lose it at the next one.
  PutRange(0, 100);
  for (int i = 0; i < 3; i++) {
    Reopen();
    ExpectPresent(0, 100);
  }
}

TEST_F(DBRecoveryTest, RecoveredDataSurvivesPowerLossAfterTheReopen) {
  PutRange(0, 20);
  Reopen();
  env_->SimulatePowerLossDropUnsynced();
  Reopen();
  ExpectPresent(0, 20);
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 20u);
}

TEST_F(DBRecoveryTest, HugeValueSurvivesReopen) {
  Random rnd(301);
  const std::string value = RandomBinaryString(&rnd, size_t{5} << 20);
  ASSERT_OK(Put("huge", value));
  Reopen();
  EXPECT_TRUE(Get("huge") == value);
}

TEST_F(DBRecoveryTest, TornWalTailIsTruncated) {
  PutRange(0, 10);
  Close();
  const std::string log = NewestLogFile(env_.get(), kDb);
  std::string contents = env_->FileContents(log);
  ASSERT_GT(contents.size(), 3u);
  contents.resize(contents.size() - 3);
  env_->SetFileContents(log, contents);
  Reopen();
  ExpectPresent(0, 9);
  ExpectAbsent(9, 10);
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 9u);
  // What the torn record would have had as its sequence number is free to be used again.
  ASSERT_OK(Put("next", "1"));
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 10u);
}

TEST_F(DBRecoveryTest, MidWalCorruptionFailsParanoidOpen) {
  std::string log;
  const std::vector<uint64_t> offsets = WriteTenBigEntriesAndClose(&log);
  ASSERT_EQ(offsets.size(), 10u);
  FlipByte(log, offsets[2] + kLogHeaderSize + 20);

  const Status s = TryReopen(CurrentOptions());
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s, "WAL corruption in")) << s.ToString();
  EXPECT_TRUE(Contains(s, log)) << "the error does not name the log: " << s.ToString();
  EXPECT_EQ(db_, nullptr);

  // A refused open changes nothing: the log is as it was, so a less strict open finds the two
  // records before the damage.
  Options lenient = CurrentOptions();
  lenient.paranoid_checks = false;
  Reopen(lenient);
  EXPECT_TRUE(HasBigEntry(0));
  EXPECT_TRUE(HasBigEntry(1));
}

TEST_F(DBRecoveryTest, MidWalCorruptionStopsReplayWhenNotParanoid) {
  std::string log;
  const std::vector<uint64_t> offsets = WriteTenBigEntriesAndClose(&log);
  ASSERT_EQ(offsets.size(), 10u);
  FlipByte(log, offsets[2] + kLogHeaderSize + 20);

  Options lenient = CurrentOptions();
  lenient.paranoid_checks = false;
  Reopen(lenient);
  EXPECT_TRUE(HasBigEntry(0));
  EXPECT_TRUE(HasBigEntry(1));
  ExpectAbsent(2, 10);
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 2u);

  // The loss is written to the LOG, with the file and the number of bytes the reader dropped.
  const std::string info = env_->FileContents(InfoLogFileName(kDb));
  EXPECT_TRUE(Contains(info, log)) << info;
  EXPECT_TRUE(Contains(info, "dropped")) << info;

  // Recovery stopped at the damage and the log is gone, so what is written now is not lost behind
  // it at the next open, which is strict again.
  ASSERT_OK(Put("after", "1"));
  Reopen();
  EXPECT_EQ(Get("after"), "1");
  EXPECT_TRUE(HasBigEntry(1));
  ExpectAbsent(2, 10);
}

TEST_F(DBRecoveryTest, CorruptionInTheLastBlockIsNotMistakenForATornTail) {
  PutRange(0, 3);  // three small records, all in the one block
  Close();
  const std::string log = NewestLogFile(env_.get(), kDb);
  const std::vector<uint64_t> offsets = LogRecordOffsets(env_.get(), log);
  ASSERT_EQ(offsets.size(), 3u);
  FlipByte(log, offsets[0] + kLogHeaderSize + 14);

  // The reader drops the rest of the block after a bad checksum, and with it records 2 and 3, which
  // were acknowledged. A report with nothing valid after it is still not "the log ends here".
  const Status s = TryReopen(CurrentOptions());
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s, "WAL corruption in")) << s.ToString();
}

TEST_F(DBRecoveryTest, ZeroFilledTailFailsParanoidOpen) {
  PutRange(0, 5);
  Close();
  const std::string log = NewestLogFile(env_.get(), kDb);
  env_->SetFileContents(log, env_->FileContents(log) + std::string(4096, '\0'));

  const Status s = TryReopen(CurrentOptions());
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s, "WAL corruption in")) << s.ToString();

  Options lenient = CurrentOptions();
  lenient.paranoid_checks = false;
  Reopen(lenient);
  ExpectPresent(0, 5);
}

TEST_F(DBRecoveryTest, AReadErrorAnywhereInOpenIsAnIOErrorAndLosesNothing) {
  // A read error is not a defect in the contents of a file: the log may be intact, and a recovery
  // that took the unreadable part for the end of it would give up writes that were acknowledged.
  // Each read that Open makes is made to fail in turn: CURRENT, the MANIFEST, and the log.
  int failures = 0;
  for (int n = 0; n < 30; n++) {
    PutRange(n * 5, n * 5 + 5);
    Close();
    env_->FailOpAfter(Op::kRead, n);
    const uint64_t injected_before = env_->injected_errors();
    const Status s = TryReopen(CurrentOptions());
    const bool fired = env_->injected_errors() > injected_before;
    env_->ClearFaults();
    if (fired) {
      EXPECT_TRUE(s.IsIOError()) << "read number " << n << ": " << s.ToString();
      failures++;
    }
    Reopen();
    ExpectPresent(0, n * 5 + 5);
    if (!fired) {
      break;
    }
  }
  EXPECT_GE(failures, 3) << "Open reads CURRENT, the MANIFEST and at least one log";
}

TEST_F(DBRecoveryTest, PowerLossKeepsSyncedWrites) {
  PutRange(0, 20, /*sync=*/true);
  for (int i = 20; i < 25; i++) {
    ASSERT_OK(Put(UserKey(i), ValueOf(i), /*sync=*/false));
  }
  env_->SimulatePowerLossDropUnsynced();
  Reopen();
  ExpectPresent(0, 20);
}

TEST_F(DBRecoveryTest, ProcessCrashKeepsUnsyncedWrites) {
  for (int i = 0; i < 25; i++) {
    ASSERT_OK(Put(UserKey(i), ValueOf(i), /*sync=*/false));
  }
  env_->SimulateProcessCrash();  // the operating system still has what was handed to it
  Reopen();
  ExpectPresent(0, 25);
}

TEST_F(DBRecoveryTest, ReopenWithoutWritesKeepsFileCountBounded) {
  for (int i = 0; i < 50; i++) {
    Close();
    Reopen();
  }
  // CURRENT, LOCK, LOG and the LOG.old of the run before, one MANIFEST and one log: a reopen
  // replaces its files and does not add to them.
  const std::vector<std::string> files = DbFiles();
  EXPECT_LE(files.size(), 6u);
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  EXPECT_EQ(CountFiles(FileType::kDescriptorFile), 1);
  EXPECT_EQ(CountFiles(FileType::kTableFile), 0);
}

TEST_F(DBRecoveryTest, LogsBelowTheLogNumberAreNotReplayedAndAreRemoved) {
  PutRange(0, 3);
  Reopen();
  Close();
  // A log from before the one the MANIFEST names as the oldest that matters, as a crash between
  // switching to a new log and deleting the old one would leave.
  const std::string old_log = LogFileName(kDb, 1);
  ASSERT_OK(WriteLogFile(env_.get(), old_log, {EncodedBatch(1, {{"old", "1"}})}));
  Reopen();
  EXPECT_EQ(Get("old"), "NOT_FOUND");
  ExpectPresent(0, 3);
  EXPECT_FALSE(env_->FileExists(old_log));
}

TEST_F(DBRecoveryTest, UnknownFilesInTheDirectoryAreLeftAlone) {
  Close();
  env_->SetFileContents("/db/notes.txt", "mine");
  env_->SetFileContents("/db/MANIFEST-oops", "not a manifest name");
  Reopen();
  EXPECT_EQ(env_->FileContents("/db/notes.txt"), "mine");
  EXPECT_EQ(env_->FileContents("/db/MANIFEST-oops"), "not a manifest name");
}

TEST_F(DBRecoveryTest, StrayTableNumbersAreNotReused) {
  ASSERT_OK(Put("a", "1"));
  Close();
  env_->SetFileContents(TableFileName(kDb, 500), "not a table");
  Reopen();
  const VersionState state = dbfull()->TEST_VersionState();
  ASSERT_EQ(state.files[0].size(), 1u);
  EXPECT_GT(state.files[0][0].number, uint64_t{500})
      << "the table written for the recovered data took the number of a file already there";
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(env_->FileContents(TableFileName(kDb, 500)), "not a table");
}

TEST_F(DBRecoveryTest, StrayLogAtTheFileNumberCeilingIsNeitherReplayedNorDeleted) {
  // MarkFileNumberUsed() ignores a number at or above kMaxFileNumber, so recovery has to decide
  // for itself what such a log is. It cannot be this database's own, which never numbers one so
  // high, and replaying it would take its sequence numbers for the database's.
  for (const uint64_t number :
       {kMaxFileNumber, kMaxFileNumber + 12345, std::numeric_limits<uint64_t>::max()}) {
    const SequenceNumber sequence_before = dbfull()->TEST_LastSequence();
    Close();
    const std::string stray = LogFileName(kDb, number);
    ASSERT_OK(WriteLogFile(env_.get(), stray, {EncodedBatch(1, {{"stray", "1"}})}));
    const std::string bytes = env_->FileContents(stray);

    Reopen();
    EXPECT_EQ(Get("stray"), "NOT_FOUND") << stray;
    EXPECT_EQ(dbfull()->TEST_LastSequence(), sequence_before) << stray;
    EXPECT_EQ(env_->FileContents(stray), bytes) << stray << " was changed or deleted";
    EXPECT_TRUE(Contains(env_->FileContents(InfoLogFileName(kDb)), stray.substr(strlen(kDb) + 1)))
        << "the LOG does not say that " << stray << " was left alone";

    ASSERT_OK(Put("real", "1"));
    Reopen();  // and the next open leaves it alone again
    EXPECT_EQ(Get("stray"), "NOT_FOUND");
    EXPECT_EQ(Get("real"), "1");
    EXPECT_EQ(env_->FileContents(stray), bytes) << stray << " was changed or deleted";

    Close();
    ASSERT_OK(env_->RemoveFile(stray));
    Reopen();
  }
}

TEST_F(DBRecoveryTest, ReplayAcceptsLogsWhoseSequenceNumbersAreBelowTheManifests) {
  // A flush that finishes while writes go on records the sequence number the database has reached,
  // which is above those of the writes that are still in the log. The log is replayed, and the
  // numbers it is held to are those of the replay, not of the MANIFEST.
  Close();
  ASSERT_OK(DestroyDB(dbname_, CurrentOptions()));
  ASSERT_OK(ForgeEmptyDatabase(env_.get(), kDb, 100, 2));
  const std::string log = LogFileName(kDb, 9);
  ASSERT_OK(WriteLogFile(
      env_.get(), log,
      {EncodedBatch(95, {{"a", "1"}, {"b", "2"}, {"c", "3"}}), EncodedBatch(98, {{"d", "4"}})}));
  Reopen();
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("c"), "3");
  EXPECT_EQ(Get("d"), "4");
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 100u);

  // The log that was replayed is gone, and the numbers that came after it are above its own, so
  // that nothing the database makes is numbered like a file it found.
  EXPECT_FALSE(env_->FileExists(log));
  const std::string newest = NewestLogFile(env_.get(), kDb);
  uint64_t number = 0;
  FileType type = FileType::kCurrentFile;
  ASSERT_TRUE(ParseFileName(newest.substr(strlen(kDb) + 1), &number, &type));
  EXPECT_GT(number, uint64_t{9});
  Reopen();
  EXPECT_EQ(Get("d"), "4");
}

TEST_F(DBRecoveryTest, ReplayAcceptsTheLargestSequenceNumber) {
  StartOver();
  ASSERT_OK(WriteLogFile(env_.get(), LogFileName(kDb, 9),
                         {EncodedBatch(kMaxSequenceNumber - 3, {{"a", "1"}, {"b", "2"}}),
                          EncodedBatch(kMaxSequenceNumber - 1, {{"c", "3"}, {"d", "4"}})}));
  Reopen();
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("d"), "4");
  EXPECT_EQ(dbfull()->TEST_LastSequence(), kMaxSequenceNumber);
}

TEST_F(DBRecoveryTest, ReplayRejectsABatchThatRunsPastTheLargestSequenceNumber) {
  // The last operation of the second batch would be numbered kMaxSequenceNumber + 1, which no tag
  // can hold, and applying it would abort in the memtable.
  StartOver();
  const std::string log = LogFileName(kDb, 9);
  ASSERT_OK(WriteLogFile(env_.get(), log,
                         {EncodedBatch(1, {{"ok", "1"}}),
                          EncodedBatch(kMaxSequenceNumber, {{"p", "1"}, {"q", "2"}})}));

  const Status s = TryReopen(CurrentOptions());
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Contains(s, "WAL corruption in")) << s.ToString();
  EXPECT_TRUE(Contains(s, log)) << s.ToString();

  Options lenient = CurrentOptions();
  lenient.paranoid_checks = false;
  Reopen(lenient);
  EXPECT_EQ(Get("ok"), "1");
  EXPECT_EQ(Get("p"), "NOT_FOUND") << "none of the batch is applied";
  EXPECT_EQ(Get("q"), "NOT_FOUND");
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 1u);
}

TEST_F(DBRecoveryTest, ReplayRejectsSequenceNumbersThatDoNotAdvance) {
  struct Case {
    const char* name;
    // The logs to put in the directory, by number, each a list of batches.
    std::vector<std::pair<uint64_t, std::vector<std::string>>> logs;
    std::vector<std::string> applied;  // keys that the lenient open finds
    std::vector<std::string> dropped;  // keys that it does not
  };
  const std::vector<Case> cases = {
      {"a repeated number",
       {{9, {EncodedBatch(5, {{"a", "1"}}), EncodedBatch(5, {{"b", "2"}})}}},
       {"a"},
       {"b"}},
      {"a range that overlaps the one before",
       {{9,
         {EncodedBatch(5, {{"a1", "1"}, {"a2", "1"}, {"a3", "1"}}),
          EncodedBatch(7, {{"b", "2"}})}}},
       {"a1", "a2", "a3"},
       {"b"}},
      {"a number below the last of the log before",
       {{9, {EncodedBatch(10, {{"a", "1"}})}}, {10, {EncodedBatch(4, {{"b", "2"}})}}},
       {"a"},
       {"b"}},
      {"the same batch in two logs",
       {{9, {EncodedBatch(5, {{"a", "1"}})}}, {10, {EncodedBatch(5, {{"a", "1"}})}}},
       {"a"},
       {}},
      {"sequence number zero", {{9, {EncodedBatch(0, {{"a", "1"}})}}}, {}, {"a"}},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    StartOver();
    for (const auto& [number, records] : c.logs) {
      ASSERT_OK(WriteLogFile(env_.get(), LogFileName(kDb, number), records));
    }
    // Without the check the memtable aborts on the second entry for a key and sequence number.
    const Status s = TryReopen(CurrentOptions());
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    EXPECT_TRUE(Contains(s, "WAL corruption in")) << s.ToString();

    Options lenient = CurrentOptions();
    lenient.paranoid_checks = false;
    Reopen(lenient);
    for (const std::string& key : c.applied) {
      EXPECT_EQ(Get(key), "1") << key;
    }
    for (const std::string& key : c.dropped) {
      EXPECT_EQ(Get(key), "NOT_FOUND") << key;
    }
  }
}

TEST_F(DBRecoveryTest, ABatchThatDoesNotDecodeIsAppliedWholeOrNotAtAll) {
  // The batch's operations are the same unit as they were when written: a batch whose records stop
  // short of its count must not leave its first operations behind.
  WriteBatch short_count;
  short_count.Put("half", "1");
  WriteBatchInternal::SetSequence(&short_count, 2);
  WriteBatchInternal::SetCount(&short_count, 2);

  std::string bad_tag = EncodedBatch(2, {{"h1", "1"}, {"h2", "1"}});
  ASSERT_LT(18u, bad_tag.size());
  bad_tag[18] = '\x07';  // the tag of the second operation: after the 12-byte header and 6 bytes

  const std::vector<std::pair<const char*, std::string>> bad = {
      {"count above the number of records",
       std::string(WriteBatchInternal::Contents(&short_count))},
      {"unknown tag", bad_tag},
      {"shorter than a header", "abc"},
  };
  for (const auto& [name, contents] : bad) {
    SCOPED_TRACE(name);
    StartOver();
    ASSERT_OK(WriteLogFile(env_.get(), LogFileName(kDb, 9),
                           {EncodedBatch(1, {{"good", "1"}}), contents}));

    const Status s = TryReopen(CurrentOptions());
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    EXPECT_TRUE(Contains(s, "WAL corruption in")) << s.ToString();

    Options lenient = CurrentOptions();
    lenient.paranoid_checks = false;
    Reopen(lenient);
    EXPECT_EQ(Get("good"), "1");
    EXPECT_EQ(dbfull()->TEST_LastSequence(), 1u);
    // Operations of the bad batch that were applied anyway would be numbered above the last
    // sequence number, invisible now and visible as soon as later writes take their numbers.
    ASSERT_OK(Put("later1", "1"));
    ASSERT_OK(Put("later2", "2"));
    ASSERT_OK(Put("later3", "3"));
    EXPECT_EQ(Get("half"), "NOT_FOUND");
    EXPECT_EQ(Get("h1"), "NOT_FOUND");
    EXPECT_EQ(Get("h2"), "NOT_FOUND");
  }
}

TEST_F(DBRecoveryTest, ExhaustedFileNumbersFailOpenInsteadOfAborting) {
  // A MANIFEST whose counter is at the ceiling, or a file away from it: no number is left for the
  // new log and the new MANIFEST, and that is a Status, not an abort, and not a log numbered at
  // the ceiling, which the next open would take for a stray file and never replay.
  for (const uint64_t counter : {kMaxFileNumber, kMaxFileNumber - 1}) {
    SCOPED_TRACE(counter);
    Close();
    ASSERT_OK(DestroyDB(dbname_, CurrentOptions()));
    ASSERT_OK(ForgeEmptyDatabase(env_.get(), kDb, 0, counter));
    const Status s = TryReopen(CurrentOptions());
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    EXPECT_EQ(db_, nullptr);
    for (const std::string& name : DbFiles()) {
      uint64_t number = 0;
      FileType type = FileType::kCurrentFile;
      if (ParseFileName(name, &number, &type)) {
        EXPECT_LT(number, kMaxFileNumber) << "Open created " << name;
      }
    }
  }
}

TEST_F(DBRecoveryTest, OpenWithTheLastFileNumbersWorksOnceAndThenFails) {
  Close();
  ASSERT_OK(DestroyDB(dbname_, CurrentOptions()));
  ASSERT_OK(ForgeEmptyDatabase(env_.get(), kDb, 0, kMaxFileNumber - 2));
  Reopen();  // the new log and the new MANIFEST take the last two numbers
  ASSERT_OK(Put("a", "1"));
  EXPECT_EQ(Get("a"), "1");
  const Status s = TryReopen(CurrentOptions());
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(DBRecoveryTest, AFailureAnywhereInOpenLosesNothing) {
  // Every call that Open makes through the Env is made to fail in turn. Whatever happens to the
  // open, the writes that were acknowledged before it are there at the next one: the replayed logs
  // are deleted only after everything that replaces them is durable, and nothing that a failed
  // MANIFEST switch may have named is deleted.
  const std::vector<Op> ops = {Op::kNewWritable,   Op::kAppend,          Op::kFlush,   Op::kSync,
                               Op::kClose,         Op::kRename,          Op::kSyncDir, Op::kRead,
                               Op::kNewSequential, Op::kNewRandomAccess, Op::kRemove};
  int next_key = 0;
  int failures = 0;
  for (const Op op : ops) {
    for (int n = 0; n < 40; n++) {
      ASSERT_OK(Put(UserKey(next_key), ValueOf(next_key)));
      next_key++;
      Close();
      env_->FailOpAfter(op, n);
      const uint64_t injected_before = env_->injected_errors();
      const Status s = TryReopen(CurrentOptions());
      const bool fired = env_->injected_errors() > injected_before;
      env_->ClearFaults();
      if (fired && !s.ok()) {
        failures++;
      }
      Reopen();
      ExpectPresent(0, next_key);
      if (!fired) {
        break;  // Open makes fewer calls of this kind than n + 1: they have all been tried
      }
    }
  }
  EXPECT_GT(failures, 15) << "the sweep hardly made Open fail";
}

}  // namespace
}  // namespace shale
