// The invariant checkers are what the simulator trusts to say that the database is right, so each
// has to be seen to do both: stay quiet on a database that is right, and alarm on one that is
// wrong. A checker that cannot alarm makes every run of the simulator pass; one that alarms on
// correct behaviour makes every run fail. The wrong database is LyingDB, a real database behind a
// wrapper that can be told to say something false.

#include "testkit/invariants.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_state.h"
#include "db/db_impl.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/fault_env.h"
#include "testkit/model_db.h"
#include "testkit/test_util.h"
#include "util/check.h"
#include "util/dbformat.h"
#include "util/filename.h"
#include "util/random.h"

namespace shale {
namespace {

constexpr const char* kDbName = "/db";

// An iterator that serves its entries in the order it was given, whatever that is, and so can serve
// them out of key order, which NewVectorIterator() may not: its entries must be sorted. Seek()
// takes the first entry, in the order served, whose key is not less than the target.
class UnorderedIterator final : public Iterator {
 public:
  explicit UnorderedIterator(Entries entries)
      : entries_(std::move(entries)), position_(entries_.size()) {}

  bool Valid() const override { return position_ < entries_.size(); }
  void SeekToFirst() override { position_ = 0; }
  void Seek(std::string_view target) override {
    position_ = 0;
    while (position_ < entries_.size() && std::string_view(entries_[position_].first) < target) {
      position_++;
    }
  }
  void Next() override {
    SHALE_CHECK(Valid(), "UnorderedIterator::Next requires Valid");
    position_++;
  }
  std::string_view key() const override {
    SHALE_CHECK(Valid(), "UnorderedIterator::key requires Valid");
    return entries_[position_].first;
  }
  std::string_view value() const override {
    SHALE_CHECK(Valid(), "UnorderedIterator::value requires Valid");
    return entries_[position_].second;
  }
  Status status() const override { return Status::OK(); }

 private:
  const Entries entries_;
  size_t position_;
};

// A database that answers like the real one behind it, except where it has been told to lie. Every
// lie is a single, named way for a database to be wrong, so that a test can say which wrong answer
// a checker is expected to catch.
class LyingDB final : public DB {
 public:
  explicit LyingDB(std::unique_ptr<DB> real) : real_(std::move(real)) {}

  DB* real() { return real_.get(); }

  // Get(key) returns OK and `value`, whatever the database holds.
  void GetReturns(std::string key, std::string value) {
    wrong_values_[std::move(key)] = std::move(value);
  }
  // Every Get fails with `error`.
  void GetFailsWith(Status error) { get_error_ = std::move(error); }
  // Get answers from the newest state when it is asked for a snapshot's.
  void GetIgnoresSnapshot() { get_ignores_snapshot_ = true; }
  // A scan skips `key`.
  void ScanDrops(std::string key) { dropped_.insert(std::move(key)); }
  // A scan reports `value` for `key`.
  void ScanReplaces(std::string key, std::string value) {
    replaced_[std::move(key)] = std::move(value);
  }
  // A scan shows the newest state when it is asked for a snapshot's.
  void ScanIgnoresSnapshot() { scan_ignores_snapshot_ = true; }
  // A scan serves the entries it has in descending key order.
  void ScanReverses() { scan_reverses_ = true; }
  // A scan serves its first `entries` entries and then fails with `error`.
  void ScanFailsAfter(size_t entries, Status error) {
    scan_fails_after_ = entries;
    scan_error_ = std::move(error);
  }

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value) override {
    return real_->Put(options, key, value);
  }
  Status Delete(const WriteOptions& options, std::string_view key) override {
    return real_->Delete(options, key);
  }
  Status Write(const WriteOptions& options, WriteBatch* updates) override {
    return real_->Write(options, updates);
  }

  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override {
    if (get_error_.has_value()) {
      return *get_error_;
    }
    const auto wrong = wrong_values_.find(std::string(key));
    if (wrong != wrong_values_.end()) {
      *value = wrong->second;
      return Status::OK();
    }
    ReadOptions actual = options;
    if (get_ignores_snapshot_) {
      actual.snapshot = nullptr;
    }
    return real_->Get(actual, key, value);
  }

  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override {
    ReadOptions actual = options;
    if (scan_ignores_snapshot_) {
      actual.snapshot = nullptr;
    }
    // The lies are told over the whole scan, which is read up front so that the real iterator is
    // gone, and its snapshot free to be released, before the caller has the wrapper.
    Entries entries;
    {
      const std::unique_ptr<Iterator> it = real_->NewIterator(actual);
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::string key(it->key());
        if (dropped_.count(key) != 0) {
          continue;
        }
        std::string value(it->value());
        const auto replaced = replaced_.find(key);
        if (replaced != replaced_.end()) {
          value = replaced->second;
        }
        entries.emplace_back(std::move(key), std::move(value));
      }
    }
    if (scan_reverses_) {
      std::reverse(entries.begin(), entries.end());
      return std::make_unique<UnorderedIterator>(std::move(entries));
    }
    if (scan_fails_after_.has_value()) {
      entries.resize(std::min(entries.size(), *scan_fails_after_));
      return NewFailingIterator(std::move(entries), *scan_error_);
    }
    return NewVectorIterator(std::move(entries));
  }

  const Snapshot* GetSnapshot() override { return real_->GetSnapshot(); }
  void ReleaseSnapshot(const Snapshot* snapshot) override { real_->ReleaseSnapshot(snapshot); }
  Status Flush() override { return real_->Flush(); }
  Status CompactRange(std::optional<std::string_view> begin,
                      std::optional<std::string_view> end) override {
    return real_->CompactRange(begin, end);
  }
  bool GetProperty(std::string_view property, std::string* value) override {
    return real_->GetProperty(property, value);
  }

 private:
  std::unique_ptr<DB> real_;
  std::map<std::string, std::string> wrong_values_;
  std::optional<Status> get_error_;
  bool get_ignores_snapshot_ = false;
  std::set<std::string> dropped_;
  std::map<std::string, std::string> replaced_;
  bool scan_ignores_snapshot_ = false;
  bool scan_reverses_ = false;
  std::optional<size_t> scan_fails_after_;
  std::optional<Status> scan_error_;
};

// Succeeds if `s` is an alarm of invariant `id`: a Corruption whose message starts with "<id>: ".
testing::AssertionResult IsAlarm(const Status& s, const std::string& id) {
  const std::string prefix = "Corruption: " + id + ": ";
  if (s.ToString().compare(0, prefix.size(), prefix) == 0) {
    if (Classify(s) != CheckOutcome::kInvariantViolation) {
      return testing::AssertionFailure()
             << "an alarm that Classify() does not call a violation: " << s.ToString();
    }
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure()
         << "expected an alarm starting \"" << id << ": \", got " << s.ToString();
}

// Succeeds if `text` contains `part`.
testing::AssertionResult Contains(const std::string& text, const std::string& part) {
  if (text.find(part) != std::string::npos) {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure() << "\"" << text << "\" does not contain \"" << part << "\"";
}

WriteBatch PutBatch(const std::string& key, const std::string& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return batch;
}

class InvariantsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = std::make_unique<FaultInjectionEnv>(1);
    options_.env = env_.get();
    options_.scheduler = &scheduler_;
    options_.create_if_missing = true;
    Reopen();
  }

  // Closes the database, if it is open, and opens it again with options_. Snapshots and iterators
  // that it made must be given back first.
  void Reopen() {
    db_.reset();
    std::unique_ptr<DB> real;
    ASSERT_OK(DB::Open(options_, kDbName, &real));
    db_ = std::make_unique<LyingDB>(std::move(real));
  }

  // Writes `key` = `value` to the database and to the model.
  void Put(const std::string& key, const std::string& value) {
    ASSERT_OK(db_->Put(WriteOptions(), key, value));
    model_.Put(key, value);
  }

  // Deletes `key` from the database and from the model.
  void Delete(const std::string& key) {
    ASSERT_OK(db_->Delete(WriteOptions(), key));
    model_.Delete(key);
  }

  // Writes UserKey(0) through UserKey(n - 1), each with ValueOf(i).
  void Fill(int n) {
    for (int i = 0; i < n; i++) {
      Put(UserKey(i), ValueOf(i));
    }
  }

  // Gives the database a table in level 1 and one in level 0, so that the version has both a level
  // whose files must be disjoint and one whose files may overlap.
  void BuildTwoLevels() {
    Fill(40);
    ASSERT_OK(db_->Flush());
    ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
    Put(UserKey(7), "rewritten");
    Put(UserKey(41), "late");
    ASSERT_OK(db_->Flush());
    const VersionState state = impl()->TEST_VersionState();
    ASSERT_FALSE(state.files[0].empty());
    ASSERT_FALSE(state.files[1].empty());
  }

  DBImpl* impl() { return static_cast<DBImpl*>(db_->real()); }

  // The checks of the version, with what the database says now about itself.
  Status CheckVersionNow(const VersionState& state, const std::set<uint64_t>& live,
                         SequenceNumber previous_last_sequence) {
    return CheckVersion(env_.get(), kDbName, state, live, impl()->TEST_LastSequence(),
                        previous_last_sequence);
  }

  // Creates an empty-looking table file that no version names.
  void CreateTableFile(uint64_t number, const std::string& contents = "x") {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(env_->NewWritableFile(TableFileName(kDbName, number), &file));
    ASSERT_OK(file->Append(contents));
    ASSERT_OK(file->Close());
  }

  Options options_;
  std::unique_ptr<FaultInjectionEnv> env_;  // without a fault armed, a plain in-memory Env
  ManualScheduler scheduler_;
  std::unique_ptr<LyingDB> db_;  // before the scheduler and the Env go: it is closed first
  ModelDB model_;
};

// ---------------------------------------------------------------------------------------------
// Classify

TEST(ClassifyTest, OkIsOk) { EXPECT_EQ(Classify(Status::OK()), CheckOutcome::kOk); }

TEST(ClassifyTest, ACorruptionWithAnInvariantIdIsAViolation) {
  EXPECT_EQ(Classify(Status::Corruption("I1: key 'a' is missing")),
            CheckOutcome::kInvariantViolation);
  EXPECT_EQ(Classify(Status::Corruption("I4: no prefix")), CheckOutcome::kInvariantViolation);
  EXPECT_EQ(Classify(Status::Corruption("I12: more than one digit")),
            CheckOutcome::kInvariantViolation);
}

TEST(ClassifyTest, EverythingElseThatFailedIsAReadFailure) {
  EXPECT_EQ(Classify(Status::IOError("injected", "read")), CheckOutcome::kReadFailure);
  EXPECT_EQ(Classify(Status::NotFound("no such file")), CheckOutcome::kReadFailure);
  EXPECT_EQ(Classify(Status::Corruption("block checksum mismatch", "000007.sst")),
            CheckOutcome::kReadFailure)
      << "the engine's own Corruption is a read that failed, not a wrong answer";
  EXPECT_EQ(Classify(Status::IOError("I1: looks like one")), CheckOutcome::kReadFailure)
      << "an alarm is a Corruption";
  EXPECT_EQ(Classify(Status::Corruption("I: no number")), CheckOutcome::kReadFailure);
  EXPECT_EQ(Classify(Status::Corruption("I1 no colon")), CheckOutcome::kReadFailure);
  EXPECT_EQ(Classify(Status::Corruption("not I1: in the middle")), CheckOutcome::kReadFailure);
  EXPECT_EQ(Classify(Status::InvalidArgument("key too large")), CheckOutcome::kReadFailure)
      << "the database refusing a read is the database's answer, not the caller's mistake in "
         "using a check";
}

TEST(ClassifyTest, OnlyACheckRefusingItsArgumentsIsACallerBug) {
  EXPECT_EQ(Classify(Status::InvalidArgument("caller bug: min_prefix 3 is above 2")),
            CheckOutcome::kCallerBug);
  EXPECT_EQ(Classify(Status::Corruption("caller bug: not an InvalidArgument")),
            CheckOutcome::kReadFailure);
}

// ---------------------------------------------------------------------------------------------
// I1: reads match the model

TEST_F(InvariantsTest, ReadsMatchPassesOnHonestDB) {
  EXPECT_OK(CheckReadsMatch(db_.get(), model_.state(), nullptr)) << "empty and empty";
  Fill(30);
  Delete(UserKey(3));
  Put(UserKey(4), "overwritten");
  Put("", "the empty key");
  Put(UserKey(9), "");
  EXPECT_OK(CheckReadsMatch(db_.get(), model_.state(), nullptr)) << "from the memtable";
  ASSERT_OK(db_->Flush());
  Put(UserKey(5), "newer than the table");
  EXPECT_OK(CheckReadsMatch(db_.get(), model_.state(), nullptr)) << "from a table and a memtable";
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  EXPECT_OK(CheckReadsMatch(db_.get(), model_.state(), nullptr)) << "after compaction";
}

TEST_F(InvariantsTest, ReadsMatchPassesOnBinaryKeysAndValues) {
  Random rnd(301);
  for (int i = 0; i < 20; i++) {
    Put(RandomBinaryString(&rnd, 1 + rnd.Uniform(8)), RandomBinaryString(&rnd, rnd.Uniform(40)));
  }
  EXPECT_OK(CheckReadsMatch(db_.get(), model_.state(), nullptr));
}

TEST_F(InvariantsTest, ReadsMatchAlarmsOnWrongValue) {
  Fill(10);
  db_->GetReturns(UserKey(6), "a value the database never held");
  const Status s = CheckReadsMatch(db_.get(), model_.state(), nullptr);
  EXPECT_TRUE(IsAlarm(s, "I1")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), UserKey(6))) << "the message names the key";
}

TEST_F(InvariantsTest, ReadsMatchAlarmsOnAKeyTheDatabaseLost) {
  Fill(10);
  // The model holds a key the database never got, which is what a lost write looks like.
  model_.Put("lost", "write");
  const Status s = CheckReadsMatch(db_.get(), model_.state(), nullptr);
  EXPECT_TRUE(IsAlarm(s, "I1")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "lost"));
}

TEST_F(InvariantsTest, ReadsMatchAlarmsOnMissingScanKey) {
  Fill(10);
  db_->ScanDrops(UserKey(3));
  const Status s = CheckReadsMatch(db_.get(), model_.state(), nullptr);
  EXPECT_TRUE(IsAlarm(s, "I1")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), UserKey(3)));
}

TEST_F(InvariantsTest, ReadsMatchAlarmsWhenTheScanLosesTheLastKey) {
  Fill(10);
  db_->ScanDrops(UserKey(9));
  const Status s = CheckReadsMatch(db_.get(), model_.state(), nullptr);
  EXPECT_TRUE(IsAlarm(s, "I1")) << "a scan that ends early is a scan that lost keys: "
                                << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), UserKey(9)));
}

TEST_F(InvariantsTest, ReadsMatchAlarmsOnAnExtraScanKey) {
  Fill(10);
  // The database holds a key the model has deleted, which is what a resurrected delete looks like.
  ASSERT_OK(db_->Put(WriteOptions(), "resurrected", "x"));
  const Status s = CheckReadsMatch(db_.get(), model_.state(), nullptr);
  EXPECT_TRUE(IsAlarm(s, "I1")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "resurrected"));
}

TEST_F(InvariantsTest, ReadsMatchAlarmsOnAnExtraKeyWhateverItsValueAndPlace) {
  // Every value is the same, so that an extra key cannot be told from the key the model expects by
  // its value, as it could if the values differed: only the keys give it away. One at the start,
  // one among the others and one past the end.
  for (const char* key : {"b", "d", "f"}) {
    Put(key, "same");
  }
  for (const char* extra : {"a", "c", "g"}) {
    ASSERT_OK(db_->Put(WriteOptions(), extra, "same"));
    const Status s = CheckReadsMatch(db_.get(), model_.state(), nullptr);
    EXPECT_TRUE(IsAlarm(s, "I1")) << extra << ": " << s.ToString();
    EXPECT_TRUE(Contains(s.ToString(), std::string("'") + extra + "'"));
    ASSERT_OK(db_->Delete(WriteOptions(), extra));
  }
  EXPECT_OK(CheckReadsMatch(db_.get(), model_.state(), nullptr));
}

TEST_F(InvariantsTest, ReadsMatchAlarmsOnAWrongValueInTheScan) {
  Fill(10);
  db_->ScanReplaces(UserKey(2), "scan only");
  const Status s = CheckReadsMatch(db_.get(), model_.state(), nullptr);
  EXPECT_TRUE(IsAlarm(s, "I1")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), UserKey(2)));
}

TEST_F(InvariantsTest, ReadsMatchAlarmsOnAScanOutOfOrder) {
  // The right keys with the right values, served in the wrong order: nothing is missing and nothing
  // is extra, so only a comparison made in key order can see it.
  Fill(5);
  db_->ScanReverses();
  const Status s = CheckReadsMatch(db_.get(), model_.state(), nullptr);
  EXPECT_TRUE(IsAlarm(s, "I1")) << s.ToString();
}

TEST_F(InvariantsTest, ReadsMatchAlarmsOnStaleSnapshotRead) {
  Put("a", "1");
  Put("b", "1");
  Put("gone", "x");
  const ModelDB::State at_snapshot = model_.state();
  const Snapshot* snapshot = db_->GetSnapshot();
  Put("a", "2");
  Delete("gone");
  Put("c", "new");

  // The honest database passes at the snapshot with the model of the snapshot, and at the newest
  // state with the newest model: this is what makes the alarms below mean something.
  EXPECT_OK(CheckReadsMatch(db_.get(), at_snapshot, snapshot));
  EXPECT_OK(CheckReadsMatch(db_.get(), model_.state(), nullptr));
  // And it does read at the snapshot: the newest model is not what the snapshot shows.
  EXPECT_TRUE(IsAlarm(CheckReadsMatch(db_.get(), model_.state(), snapshot), "I1"));

  db_->GetIgnoresSnapshot();
  const Status s = CheckReadsMatch(db_.get(), at_snapshot, snapshot);
  EXPECT_TRUE(IsAlarm(s, "I1")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "snapshot")) << "the message says the read was at a snapshot";
  db_->ReleaseSnapshot(snapshot);
}

TEST_F(InvariantsTest, ReadsMatchAlarmsOnStaleSnapshotScan) {
  Put("a", "1");
  Put("b", "1");
  const ModelDB::State at_snapshot = model_.state();
  const Snapshot* snapshot = db_->GetSnapshot();
  Put("a", "2");
  Put("c", "new");

  db_->ScanIgnoresSnapshot();
  const Status s = CheckReadsMatch(db_.get(), at_snapshot, snapshot);
  EXPECT_TRUE(IsAlarm(s, "I1")) << "the point reads are right, the scan is not: " << s.ToString();
  db_->ReleaseSnapshot(snapshot);
}

TEST_F(InvariantsTest, ReadsMatchReportsAReadErrorAsItIsNotAsAWrongAnswer) {
  // A read that fails says nothing about what the database holds. While a fault is armed the
  // simulator allows it, so the checker must not dress it up as an I1 alarm.
  Fill(5);
  db_->GetFailsWith(Status::IOError("injected", "read"));
  Status s = CheckReadsMatch(db_.get(), model_.state(), nullptr);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(Classify(s), CheckOutcome::kReadFailure);
}

TEST_F(InvariantsTest, ReadsMatchReportsAFailedScanAsItIsNotAsMissingKeys) {
  Fill(5);
  db_->ScanFailsAfter(2, Status::Corruption("block checksum mismatch", "000007.sst"));
  const Status s = CheckReadsMatch(db_.get(), model_.state(), nullptr);
  EXPECT_TRUE(s.IsCorruption());
  EXPECT_EQ(Classify(s), CheckOutcome::kReadFailure)
      << "the scan stopped because it failed, not because the data ran out: " << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "block checksum mismatch"));
}

// ---------------------------------------------------------------------------------------------
// I2: the version

TEST_F(InvariantsTest, VersionCheckPassesAndAlarms) {
  BuildTwoLevels();
  const VersionState state = impl()->TEST_VersionState();
  const std::set<uint64_t> live = impl()->TEST_LiveFiles();
  const SequenceNumber last = impl()->TEST_LastSequence();

  // A real database's own state is what the check must accept.
  EXPECT_OK(CheckVersion(env_.get(), kDbName, state, live, last, 0));
  EXPECT_OK(CheckVersion(env_.get(), kDbName, state, live, last, last))
      << "the same sequence number twice is not going back";

  // The same state with a second file in level 1 over the range of the first.
  VersionState overlapping = state;
  const FileMetaData first = state.files[1][0];
  FileMetaData twin = first;
  twin.number = state.next_file_number + 1;
  overlapping.next_file_number = twin.number + 1;
  overlapping.files[1].insert(overlapping.files[1].begin() + 1, twin);
  std::set<uint64_t> live_with_twin = live;
  live_with_twin.insert(twin.number);
  CreateTableFile(twin.number);
  const Status overlap = CheckVersion(env_.get(), kDbName, overlapping, live_with_twin, last, 0);
  EXPECT_TRUE(IsAlarm(overlap, "I2")) << overlap.ToString();
  EXPECT_TRUE(Contains(overlap.ToString(), "overlap"))
      << "it was the order of the files that was caught, not the files being absent";
  EXPECT_TRUE(Contains(overlap.ToString(), std::to_string(first.number)));
  EXPECT_TRUE(Contains(overlap.ToString(), std::to_string(twin.number)));

  // A live file that is gone from the Env, which is what deleting a file a reader holds looks like.
  const uint64_t victim = state.files[1][0].number;
  ASSERT_OK(env_->RemoveFile(TableFileName(kDbName, victim)));
  const Status missing = CheckVersionNow(state, live, 0);
  EXPECT_TRUE(IsAlarm(missing, "I2")) << missing.ToString();
  EXPECT_TRUE(Contains(missing.ToString(), TableFileName(kDbName, victim)))
      << "the message names the file: " << missing.ToString();
}

TEST_F(InvariantsTest, VersionCheckAlarmsOnALiveFileOfAnOlderVersionThatIsGone) {
  BuildTwoLevels();
  const VersionState state = impl()->TEST_VersionState();
  // A reader that holds an older version keeps its files live. The state is the newest version's;
  // the check has to look at the whole live set, which is where a deleted file shows.
  std::set<uint64_t> live = impl()->TEST_LiveFiles();
  live.insert(900);
  const Status s = CheckVersionNow(state, live, 0);
  EXPECT_TRUE(IsAlarm(s, "I2")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), TableFileName(kDbName, 900)));
}

TEST_F(InvariantsTest, VersionCheckOnlyLooksForAFileThatTheStateDoesNotRecord) {
  BuildTwoLevels();
  const VersionState state = impl()->TEST_VersionState();
  std::set<uint64_t> live = impl()->TEST_LiveFiles();
  // A file of an older version that a reader holds: not in the state, so there is no size to
  // compare with, and its being there is all that can be asked.
  CreateTableFile(900, "any size at all");
  live.insert(900);
  EXPECT_OK(CheckVersionNow(state, live, 0));
}

TEST_F(InvariantsTest, VersionCheckAlarmsOnAFileOfTheWrongSize) {
  BuildTwoLevels();
  VersionState state = impl()->TEST_VersionState();
  const std::set<uint64_t> live = impl()->TEST_LiveFiles();
  state.files[0][0].file_size += 1;
  const Status s = CheckVersionNow(state, live, 0);
  EXPECT_TRUE(IsAlarm(s, "I2")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), TableFileName(kDbName, state.files[0][0].number)));
  EXPECT_TRUE(Contains(s.ToString(), "size"));
}

TEST_F(InvariantsTest, VersionCheckAlarmsOnAStateFileThatIsNotLive) {
  BuildTwoLevels();
  const VersionState state = impl()->TEST_VersionState();
  std::set<uint64_t> live = impl()->TEST_LiveFiles();
  const uint64_t forgotten = state.files[1][0].number;
  live.erase(forgotten);
  const Status s = CheckVersionNow(state, live, 0);
  EXPECT_TRUE(IsAlarm(s, "I2")) << "a file the version names that the live set does not is one the "
                                   "next sweep may delete: "
                                << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), TableFileName(kDbName, forgotten)));
}

TEST_F(InvariantsTest, VersionCheckAlarmsWhenTheSequenceNumberGoesBack) {
  BuildTwoLevels();
  const VersionState state = impl()->TEST_VersionState();
  const std::set<uint64_t> live = impl()->TEST_LiveFiles();
  const SequenceNumber last = impl()->TEST_LastSequence();
  ASSERT_GT(last, 0u);
  // The database's sequence number is what it was and the previous reading was above it. Nothing
  // else is wrong, so only the rule that it never goes back can be what alarms.
  const Status back = CheckVersion(env_.get(), kDbName, state, live, last, last + 1);
  EXPECT_TRUE(IsAlarm(back, "I2")) << back.ToString();
  EXPECT_TRUE(Contains(back.ToString(), "went back"));
  EXPECT_OK(CheckVersion(env_.get(), kDbName, state, live, last, last - 1)) << "going forward";
}

TEST_F(InvariantsTest, VersionCheckAlarmsWhenTheTablesHoldWhatTheDatabaseHasNotSeen) {
  BuildTwoLevels();
  VersionState state = impl()->TEST_VersionState();
  const std::set<uint64_t> live = impl()->TEST_LiveFiles();
  state.last_sequence = impl()->TEST_LastSequence() + 1;
  const Status s = CheckVersionNow(state, live, 0);
  EXPECT_TRUE(IsAlarm(s, "I2")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "last_sequence"));
}

TEST_F(InvariantsTest, VersionCheckAcceptsAnEmptyDatabase) {
  const VersionState state = impl()->TEST_VersionState();
  EXPECT_OK(CheckVersionNow(state, impl()->TEST_LiveFiles(), 0));
}

TEST_F(InvariantsTest, VersionCheckAlarmsOnTheRulesOfTheStateItself) {
  BuildTwoLevels();
  VersionState state = impl()->TEST_VersionState();
  const std::set<uint64_t> live = impl()->TEST_LiveFiles();
  // A file at two levels at once breaks the rule that no number is used twice; the check reports
  // what CheckInvariants() says, under its own id.
  state.files[2].push_back(state.files[1][0]);
  const Status s = CheckVersionNow(state, live, 0);
  EXPECT_TRUE(IsAlarm(s, "I2")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "twice"));
}

// ---------------------------------------------------------------------------------------------
// I3: no orphan tables

TEST_F(InvariantsTest, OrphanCheckPassesAndAlarms) {
  BuildTwoLevels();
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  const std::set<uint64_t> live = impl()->TEST_LiveFiles();
  EXPECT_OK(CheckNoOrphanTables(env_.get(), kDbName, live));

  CreateTableFile(777);
  const Status s = CheckNoOrphanTables(env_.get(), kDbName, live);
  EXPECT_TRUE(IsAlarm(s, "I3")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "000777.sst"));

  std::set<uint64_t> with_it = live;
  with_it.insert(777);
  EXPECT_OK(CheckNoOrphanTables(env_.get(), kDbName, with_it))
      << "a table that is live is not an orphan";
}

TEST_F(InvariantsTest, OrphanCheckOnlyCountsTables) {
  BuildTwoLevels();
  const std::set<uint64_t> live = impl()->TEST_LiveFiles();
  // The directory holds a log, a MANIFEST, CURRENT and LOCK as it is. Files that are not tables
  // are not the business of this check, whether or not they are the database's.
  for (const char* name : {"000778.dbtmp", "000779.log", "stray.txt", "000780.sst.bak"}) {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(env_->NewWritableFile(std::string(kDbName) + "/" + name, &file));
    ASSERT_OK(file->Close());
  }
  EXPECT_OK(CheckNoOrphanTables(env_.get(), kDbName, live));
}

TEST_F(InvariantsTest, OrphanCheckAlarmsOnATableThatAFailedRemoveLeftUntilTheNextSweep) {
  // I3 needs more than a database that is idle and has no error. The sweep that deletes the tables
  // nothing names any more swallows a RemoveFile that fails: it logs it and tries again at its
  // next run, without setting a background error. A correct database therefore has an orphan for as
  // long as that failure stands, and the check must not be run then. This shows the orphan, and its
  // going at the next sweep, so that the precondition in invariants.h is demonstrated.
  BuildTwoLevels();
  const uint64_t injected_before = env_->injected_errors();
  env_->FailOpAfter(FaultInjectionEnv::Op::kRemove, 0);
  ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
  ASSERT_EQ(env_->injected_errors(), injected_before + 1) << "the failure of RemoveFile happened";

  std::string background_error;
  ASSERT_TRUE(db_->GetProperty("shale.background-error", &background_error));
  EXPECT_EQ(background_error, "OK") << "nothing in the database says that a table was left";
  const Status orphan = CheckNoOrphanTables(env_.get(), kDbName, impl()->TEST_LiveFiles());
  EXPECT_TRUE(IsAlarm(orphan, "I3")) << orphan.ToString();

  // The failure was injected once. The next sweep, which a flush ends with, finds the table again.
  Put("after", "the failure");
  ASSERT_OK(db_->Flush());
  EXPECT_OK(CheckNoOrphanTables(env_.get(), kDbName, impl()->TEST_LiveFiles()));
}

TEST_F(InvariantsTest, OrphanCheckReportsAnUnreadableDirectoryAsItIs) {
  const Status s = CheckNoOrphanTables(env_.get(), "/no-such-dir", {});
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(Classify(s), CheckOutcome::kReadFailure)
      << "an unlistable directory is the Env's error: " << s.ToString();
}

// ---------------------------------------------------------------------------------------------
// I4: the recovered state is a prefix

TEST_F(InvariantsTest, PrefixCheckFindsTheRightPrefix) {
  Put("a", "1");
  Put("b", "2");
  const ModelDB::State durable{{"a", "1"}};
  const std::vector<WriteBatch> uncertain = {PutBatch("b", "2"), PutBatch("c", "3")};
  ModelDB::State matched;
  ASSERT_OK(CheckRecoveredPrefix(db_.get(), durable, uncertain, 0, &matched));
  EXPECT_EQ(matched, (ModelDB::State{{"a", "1"}, {"b", "2"}}));
}

TEST_F(InvariantsTest, PrefixCheckAcceptsEveryPrefixLength) {
  const ModelDB::State durable{{"a", "1"}};
  const std::vector<WriteBatch> uncertain = {PutBatch("b", "2"), PutBatch("c", "3")};
  ModelDB::State matched;

  // Nothing of the uncertain writes survived: the durable state itself.
  Put("a", "1");
  ASSERT_OK(CheckRecoveredPrefix(db_.get(), durable, uncertain, 0, &matched));
  EXPECT_EQ(matched, durable);

  // All of them.
  Put("b", "2");
  Put("c", "3");
  ASSERT_OK(CheckRecoveredPrefix(db_.get(), durable, uncertain, 0, &matched));
  EXPECT_EQ(matched, (ModelDB::State{{"a", "1"}, {"b", "2"}, {"c", "3"}}));
  ASSERT_OK(CheckRecoveredPrefix(db_.get(), durable, uncertain, 2, &matched))
      << "all of them is a prefix of length 2";
}

TEST_F(InvariantsTest, PrefixCheckRespectsMinimum) {
  Put("a", "1");
  Put("b", "2");
  const ModelDB::State durable{{"a", "1"}};
  const std::vector<WriteBatch> uncertain = {PutBatch("b", "2"), PutBatch("c", "3")};
  ModelDB::State matched;
  ASSERT_OK(CheckRecoveredPrefix(db_.get(), durable, uncertain, 1, &matched))
      << "the prefix of length 1 is allowed by a minimum of 1";
  const Status s = CheckRecoveredPrefix(db_.get(), durable, uncertain, 2, &matched);
  EXPECT_TRUE(IsAlarm(s, "I4")) << "write 2 was acknowledged, so a state without it is a loss: "
                                << s.ToString();
}

TEST_F(InvariantsTest, PrefixCheckAlarmsOnHoles) {
  Put("a", "1");
  Put("c", "3");
  const ModelDB::State durable{{"a", "1"}};
  const std::vector<WriteBatch> uncertain = {PutBatch("b", "2"), PutBatch("c", "3")};
  ModelDB::State matched = {{"untouched", "yes"}};
  const Status s = CheckRecoveredPrefix(db_.get(), durable, uncertain, 0, &matched);
  EXPECT_TRUE(IsAlarm(s, "I4")) << s.ToString();
  EXPECT_EQ(matched, (ModelDB::State{{"untouched", "yes"}})) << "nothing matched, nothing is said";
}

TEST_F(InvariantsTest, PrefixCheckAlarmsOnHalfABatch) {
  Put("a", "1");
  Put("b", "2");  // the first half of one batch of two writes
  const ModelDB::State durable{{"a", "1"}};
  WriteBatch both;
  both.Put("b", "2");
  both.Put("c", "3");
  const Status s = CheckRecoveredPrefix(db_.get(), durable, {both}, 0, nullptr);
  EXPECT_TRUE(IsAlarm(s, "I4")) << "a batch is all there or none of it: " << s.ToString();
}

TEST_F(InvariantsTest, PrefixCheckAlarmsOnALostDurableWrite) {
  // The durable state holds a key that the database no longer has, which is what a sync that did
  // not reach the disk looks like.
  Put("b", "2");
  const ModelDB::State durable{{"a", "1"}};
  const Status s = CheckRecoveredPrefix(db_.get(), durable, {PutBatch("b", "2")}, 0, nullptr);
  EXPECT_TRUE(IsAlarm(s, "I4")) << s.ToString();
}

TEST_F(InvariantsTest, PrefixCheckAlarmsOnAnUnexplainedKey) {
  Put("a", "1");
  Put("ghost", "boo");
  const Status s = CheckRecoveredPrefix(db_.get(), {{"a", "1"}}, {PutBatch("b", "2")}, 0, nullptr);
  EXPECT_TRUE(IsAlarm(s, "I4")) << s.ToString();
  EXPECT_TRUE(Contains(s.ToString(), "ghost")) << "the message points at what differs";
}

TEST_F(InvariantsTest, PrefixCheckAlarmsOnAScanOutOfOrder) {
  // The database holds exactly the durable state, but serves it backwards. Collected into a map it
  // would look right, so the order has to be checked on the way in.
  Put("a", "1");
  Put("b", "2");
  Put("c", "3");
  db_->ScanReverses();
  const Status s = CheckRecoveredPrefix(db_.get(), model_.state(), {}, 0, nullptr);
  EXPECT_TRUE(IsAlarm(s, "I4")) << s.ToString();
}

TEST_F(InvariantsTest, PrefixCheckSeesDeletesAndOverwrites) {
  // Different prefixes can give the same state; the check accepts a database in that state for
  // each prefix length that gives it, and the minimum picks among them.
  Put("a", "1");
  const ModelDB::State durable{{"a", "1"}};
  WriteBatch erase;
  erase.Delete("a");
  const std::vector<WriteBatch> uncertain = {PutBatch("a", "2"), erase, PutBatch("a", "1")};
  ModelDB::State matched;
  ASSERT_OK(CheckRecoveredPrefix(db_.get(), durable, uncertain, 0, &matched));
  EXPECT_EQ(matched, durable);
  ASSERT_OK(CheckRecoveredPrefix(db_.get(), durable, uncertain, 3, &matched))
      << "the full prefix leads back to the same state";
  EXPECT_EQ(matched, durable);
  EXPECT_OK(CheckRecoveredPrefix(db_.get(), durable, uncertain, 1, nullptr))
      << "length 3 matches, so a minimum of 1 is met";
  EXPECT_TRUE(IsAlarm(
      CheckRecoveredPrefix(db_.get(), durable, {uncertain[0], uncertain[1]}, 1, nullptr), "I4"))
      << "with only the first two writes no prefix of length 1 or more gives this state";
}

TEST_F(InvariantsTest, PrefixCheckAcceptsAnEmptyDatabase) {
  ModelDB::State matched = {{"x", "y"}};
  ASSERT_OK(CheckRecoveredPrefix(db_.get(), {}, {}, 0, &matched));
  EXPECT_TRUE(matched.empty());
  ASSERT_OK(CheckRecoveredPrefix(db_.get(), {}, {PutBatch("a", "1")}, 0, &matched));
  EXPECT_TRUE(matched.empty());
  EXPECT_TRUE(IsAlarm(CheckRecoveredPrefix(db_.get(), {}, {PutBatch("a", "1")}, 1, nullptr), "I4"));
}

TEST_F(InvariantsTest, PrefixCheckRefusesAMinimumNoPrefixCanMeet) {
  const Status s = CheckRecoveredPrefix(db_.get(), {}, {PutBatch("a", "1")}, 2, nullptr);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(Classify(s), CheckOutcome::kCallerBug)
      << "a caller's mistake, not a defect of the database and not a read that failed: "
      << s.ToString();
}

TEST_F(InvariantsTest, PrefixCheckReportsAFailedScanAsItIsNotAsAMismatch) {
  Put("a", "1");
  Put("b", "2");
  db_->ScanFailsAfter(1, Status::IOError("injected", "read"));
  const Status s = CheckRecoveredPrefix(db_.get(), {{"a", "1"}}, {PutBatch("b", "2")}, 0, nullptr);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(Classify(s), CheckOutcome::kReadFailure);
}

// ---------------------------------------------------------------------------------------------
// All of them together, on a database that is right

// Snapshots taken from the database that `db` holds, each with the model as it was, given back when
// this goes out of scope: a snapshot that is still held when the database closes aborts the
// process, which would hide the failure that ended the test. `db` is held by reference, since the
// test reopens the database and so replaces what it points to; the snapshots must be given back
// before it does.
class HeldSnapshots {
 public:
  explicit HeldSnapshots(const std::unique_ptr<LyingDB>& db) : db_(db) {}
  ~HeldSnapshots() { ReleaseAll(); }
  HeldSnapshots(const HeldSnapshots&) = delete;
  HeldSnapshots& operator=(const HeldSnapshots&) = delete;

  void Take(const ModelDB::State& model) { held_.emplace_back(db_->GetSnapshot(), model); }

  void ReleaseOldest() {
    db_->ReleaseSnapshot(held_.front().first);
    held_.erase(held_.begin());
  }

  void ReleaseAll() {
    for (const auto& entry : held_) {
      db_->ReleaseSnapshot(entry.first);
    }
    held_.clear();
  }

  const std::vector<std::pair<const Snapshot*, ModelDB::State>>& held() const { return held_; }

 private:
  const std::unique_ptr<LyingDB>& db_;
  std::vector<std::pair<const Snapshot*, ModelDB::State>> held_;
};

TEST_F(InvariantsTest, HonestDatabaseNeverAlarms) {
  // A checker that alarms on correct behaviour fails every run of the simulator, so every check is
  // run after every step of a mixed workload that makes the database flush, compact and reopen, on
  // the engine as it is. Small buffers and low triggers put files in several levels.
  options_.write_buffer_size = 16384;
  options_.target_file_size = 16384;
  options_.max_bytes_for_level_base = 65536;
  options_.level_size_multiplier = 4;
  options_.l0_compaction_trigger = 2;
  Reopen();

  constexpr uint32_t kKeys = 300;
  Random rnd(2026);
  HeldSnapshots snapshots(db_);
  SequenceNumber previous_last_sequence = 0;
  bool reached_level_two = false;
  for (int step = 0; step < 1000; step++) {
    switch (rnd.Uniform(12)) {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4:
        Put(UserKey(static_cast<int>(rnd.Uniform(kKeys))),
            RandomString(&rnd, 100 + rnd.Uniform(600)));
        break;
      case 5:
        Delete(UserKey(static_cast<int>(rnd.Uniform(kKeys))));
        break;
      case 6: {
        WriteBatch batch;
        const uint32_t operations = 2 + rnd.Uniform(4);
        for (uint32_t i = 0; i < operations; i++) {
          const std::string key = UserKey(static_cast<int>(rnd.Uniform(kKeys)));
          if (rnd.OneIn(3)) {
            batch.Delete(key);
          } else {
            batch.Put(key, RandomString(&rnd, rnd.Uniform(200)));
          }
        }
        ASSERT_OK(db_->Write(WriteOptions(), &batch));
        model_.Apply(batch);
        break;
      }
      case 7:
        if (snapshots.held().size() >= 3 || (!snapshots.held().empty() && rnd.OneIn(2))) {
          snapshots.ReleaseOldest();
        } else {
          snapshots.Take(model_.state());
        }
        break;
      case 8:
        ASSERT_OK(db_->Flush());
        break;
      case 9:
        scheduler_.RunOne();
        break;
      case 10:
        if (rnd.OneIn(8)) {
          ASSERT_OK(db_->CompactRange(std::nullopt, std::nullopt));
        }
        break;
      default:
        if (rnd.OneIn(40)) {
          snapshots.ReleaseAll();
          Reopen();
          ASSERT_OK(CheckRecoveredPrefix(db_.get(), model_.state(), {}, 0, nullptr))
              << "after a clean reopen the database holds what it held";
          previous_last_sequence = 0;  // the sequence is read again from the MANIFEST
        }
        break;
    }

    ASSERT_OK(CheckReadsMatch(db_.get(), model_.state(), nullptr)) << "step " << step;
    for (const auto& entry : snapshots.held()) {
      ASSERT_OK(CheckReadsMatch(db_.get(), entry.second, entry.first)) << "step " << step;
    }
    const VersionState state = impl()->TEST_VersionState();
    const SequenceNumber last_sequence = impl()->TEST_LastSequence();
    ASSERT_OK(CheckVersion(env_.get(), kDbName, state, impl()->TEST_LiveFiles(), last_sequence,
                           previous_last_sequence))
        << "step " << step;
    previous_last_sequence = last_sequence;
    reached_level_two = reached_level_two || !state.files[2].empty();

    if (step % 25 == 0) {
      scheduler_.RunPending();
      ASSERT_OK(CheckNoOrphanTables(env_.get(), kDbName, impl()->TEST_LiveFiles()))
          << "step " << step;
    }
  }
  EXPECT_TRUE(reached_level_two) << "the run was too gentle to put a file below level 1";
  EXPECT_GT(impl()->TEST_Counters().compactions + impl()->TEST_Counters().trivial_moves, 0u);
}

}  // namespace
}  // namespace shale
