// The version set: the MANIFEST that makes the table set durable, the CURRENT file that names it,
// and the immutable versions that reads run against.
//
// What the engine promises about its files rests on two orders. An edit is made durable in the
// MANIFEST before the version it makes is installed, so that a read can never see a table set that
// a crash would take back; and CURRENT is switched to a new MANIFEST by a rename, so that it names
// the old one or the new one and never neither. The tests below hold both orders to the Env's
// failures and to its power loss, and then check that what a version answers is what the levels
// say.

#include "db/version_set.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/filename_io.h"
#include "db/handle_check_env.h"
#include "db/table_cache.h"
#include "io/env_util.h"
#include "io/log_reader.h"
#include "io/log_writer.h"
#include "io/merging_iterator.h"
#include "io/table.h"
#include "io/table_builder.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"
#include "util/filename.h"

namespace shale {
namespace {

constexpr char kDb[] = "/db";

using Op = FaultInjectionEnv::Op;

// The name of `path` without its directory.
std::string BareName(const std::string& path) { return path.substr(std::string(kDb).size() + 1); }

std::string ManifestName(uint64_t number) { return BareName(DescriptorFileName(kDb, number)); }

// A file as the version set records it. The keys are those of a table holding one entry for each
// end; whether such a table exists does not matter to the tests that never read it.
FileMetaData Meta(uint64_t number, std::string_view smallest, std::string_view largest,
                  uint64_t size = 1000) {
  FileMetaData f;
  f.number = number;
  f.file_size = size;
  f.smallest = IKey(smallest, 1);
  f.largest = IKey(largest, 1);
  return f;
}

VersionEdit AddFile(int level, const FileMetaData& file) {
  VersionEdit edit;
  edit.new_files.emplace_back(level, file);
  return edit;
}

VersionEdit DeleteFile(int level, uint64_t number) {
  VersionEdit edit;
  edit.deleted_files.emplace(level, number);
  return edit;
}

std::string Encode(const VersionEdit& edit) {
  std::string out;
  edit.EncodeTo(&out);
  return out;
}

// The records of a MANIFEST, decoded, and where each starts in the file.
struct ManifestRecords {
  std::vector<VersionEdit> edits;
  std::vector<uint64_t> offsets;
};

class VersionSetFixture : public testing::Test {
 protected:
  VersionSetFixture()
      : env_(21), block_cache_(size_t{1} << 20), table_cache_(kDb, &env_, &block_cache_, 100) {}

  void SetUp() override { ASSERT_OK(env_.CreateDir(kDb)); }

  // Every file the test opened for writing was closed. The version sets are gone by now.
  void TearDown() override { EXPECT_EQ(checked_env_.leaked(), 0) << "a file was not closed"; }

  std::unique_ptr<VersionSet> NewVersionSet() {
    return std::make_unique<VersionSet>(kDb, &checked_env_, &table_cache_);
  }

  // The bare names in the database directory, sorted.
  std::vector<std::string> DbFiles() {
    std::vector<std::string> names;
    EXPECT_OK(env_.GetChildren(kDb, &names));
    std::sort(names.begin(), names.end());
    return names;
  }

  std::string CurrentContents() { return env_.FileContents(CurrentFileName(kDb)); }

  ManifestRecords ReadManifest(uint64_t number) {
    ManifestRecords records;
    std::unique_ptr<SequentialFile> file;
    EXPECT_OK(env_.NewSequentialFile(DescriptorFileName(kDb, number), &file));
    if (file == nullptr) {
      return records;
    }
    log::Reader reader(file.get(), nullptr, true);
    std::string_view record;
    std::string scratch;
    while (reader.ReadRecord(&record, &scratch)) {
      VersionEdit edit;
      EXPECT_OK(edit.DecodeFrom(record));
      records.edits.push_back(std::move(edit));
      records.offsets.push_back(reader.LastRecordOffset());
    }
    return records;
  }

  // Writes MANIFEST `number` holding `payloads` as records, and points CURRENT at it. What the
  // version set would never write, a test can: a manifest of garbage, or of an edit it refuses.
  void WriteManifest(uint64_t number, const std::vector<std::string>& payloads) {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(env_.NewWritableFile(DescriptorFileName(kDb, number), &file));
    {
      log::Writer writer(file.get());
      for (const std::string& payload : payloads) {
        ASSERT_OK(writer.AddRecord(payload));
      }
    }
    ASSERT_OK(file->Sync());
    ASSERT_OK(file->Close());
    env_.SetFileContents(CurrentFileName(kDb), ManifestName(number) + "\n");
  }

  FaultInjectionEnv env_;
  HandleCheckEnv checked_env_{&env_};  // what the version sets write through; tables use env_
  BlockCache block_cache_;
  TableCache table_cache_;
};

// SetCurrentFile

class SetCurrentFileTest : public VersionSetFixture {};

TEST_F(SetCurrentFileTest, WritesTheManifestNameAndLeavesNoTempFile) {
  ASSERT_OK(SetCurrentFile(&checked_env_, kDb, 7));
  EXPECT_EQ(CurrentContents(), "MANIFEST-000007\n");
  EXPECT_EQ(DbFiles(), std::vector<std::string>{"CURRENT"});
}

TEST_F(SetCurrentFileTest, ANumberWiderThanSixDigitsIsPrintedInFull) {
  ASSERT_OK(SetCurrentFile(&checked_env_, kDb, 1234567));
  EXPECT_EQ(CurrentContents(), "MANIFEST-1234567\n");
}

TEST_F(SetCurrentFileTest, ReplacesAnExistingCurrent) {
  env_.SetFileContents(CurrentFileName(kDb), "MANIFEST-000001\n");
  ASSERT_OK(SetCurrentFile(&checked_env_, kDb, 12));
  EXPECT_EQ(CurrentContents(), "MANIFEST-000012\n");
  EXPECT_EQ(DbFiles(), std::vector<std::string>{"CURRENT"});
}

TEST_F(SetCurrentFileTest, FailedWriteKeepsTheOldCurrentAndLeavesNoTempFile) {
  env_.SetFileContents(CurrentFileName(kDb), "MANIFEST-000001\n");
  for (const Op op : {Op::kNewWritable, Op::kAppend, Op::kSync, Op::kClose, Op::kRename}) {
    SCOPED_TRACE(static_cast<int>(op));
    env_.ClearFaults();
    env_.FailOpAfter(op, 0);
    const Status s = SetCurrentFile(&checked_env_, kDb, 12);
    env_.ClearFaults();
    EXPECT_TRUE(s.IsIOError()) << s.ToString();
    EXPECT_EQ(CurrentContents(), "MANIFEST-000001\n");
    EXPECT_EQ(DbFiles(), std::vector<std::string>{"CURRENT"});
  }
}

TEST_F(SetCurrentFileTest, FailedDirectorySyncIsReported) {
  // The rename happened, and whether it will survive a power loss is not known, so the call has
  // not succeeded. What CURRENT names now is whole either way.
  env_.SetFileContents(CurrentFileName(kDb), "MANIFEST-000001\n");
  env_.FailOpAfter(Op::kSyncDir, 0);
  const Status s = SetCurrentFile(&checked_env_, kDb, 12);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(DbFiles(), std::vector<std::string>{"CURRENT"});
}

TEST_F(SetCurrentFileTest, SyncsTheDirectory) {
  // The harshest power loss keeps only what a SyncDir() made durable. Without one the rename is
  // undone and CURRENT names the old MANIFEST, which a database that had just switched would not
  // survive.
  env_.SetFileContents(CurrentFileName(kDb), "MANIFEST-000001\n");
  ASSERT_OK(SetCurrentFile(&checked_env_, kDb, 12));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(CurrentContents(), "MANIFEST-000012\n");
}

TEST_F(SetCurrentFileTest, CurrentIsAlwaysWholeAfterAFailureAndAPowerLoss) {
  const std::string old_contents = "MANIFEST-000001\n";
  const std::string new_contents = "MANIFEST-000012\n";
  for (const Op op :
       {Op::kNewWritable, Op::kAppend, Op::kSync, Op::kClose, Op::kRename, Op::kSyncDir}) {
    SCOPED_TRACE(static_cast<int>(op));
    for (const bool drop_all : {true, false}) {
      env_.ClearFaults();
      env_.SetFileContents(CurrentFileName(kDb), old_contents);
      env_.FailOpAfter(op, 0);
      EXPECT_FALSE(SetCurrentFile(&checked_env_, kDb, 12).ok());
      env_.ClearFaults();
      if (drop_all) {
        env_.SimulatePowerLossDropUnsynced();
      } else {
        env_.SimulatePowerLoss();
      }
      const std::string after = CurrentContents();
      EXPECT_TRUE(after == old_contents || after == new_contents) << "CURRENT is: " << after;
    }
  }
}

// VersionSet: creating, applying and recovering

class VersionSetTest : public VersionSetFixture {};

TEST_F(VersionSetTest, CreateNewWritesManifestAndCurrent) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());

  // The directory holds the MANIFEST and CURRENT and nothing else: no ".dbtmp" left behind.
  EXPECT_EQ(DbFiles(), (std::vector<std::string>{"CURRENT", "MANIFEST-000001"}));
  EXPECT_EQ(CurrentContents(), "MANIFEST-000001\n");

  EXPECT_EQ(versions->ManifestFileNumber(), 1u);
  EXPECT_EQ(versions->NextFileNumber(), 2u);
  EXPECT_EQ(versions->LogNumber(), 0u);
  EXPECT_EQ(versions->LastSequence(), 0u);
  EXPECT_EQ(versions->current()->state(), VersionState{});

  // The MANIFEST is one record, the snapshot of the empty state.
  const ManifestRecords records = ReadManifest(1);
  ASSERT_EQ(records.edits.size(), 1u);
  EXPECT_EQ(records.edits[0], SnapshotEdit(VersionState{}));
}

TEST_F(VersionSetTest, CreateNewSurvivesPowerLoss) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  env_.SimulatePowerLossDropUnsynced();
  const auto recovered = NewVersionSet();
  ASSERT_OK(recovered->Recover());
  EXPECT_EQ(recovered->current()->state(), VersionState{});
}

TEST_F(VersionSetTest, CreateNewFailureCreatesNoDatabase) {
  // Fail each call CreateNew makes in turn. A database that failed to be created must not look like
  // one that was: no CURRENT, so that the next Open creates it again, and no partial MANIFEST.
  for (const Op op : {Op::kNewWritable, Op::kAppend, Op::kSync, Op::kClose, Op::kRename}) {
    SCOPED_TRACE(static_cast<int>(op));
    env_.ClearFaults();
    env_.FailOpAfter(op, 0);
    const auto versions = NewVersionSet();
    const Status s = versions->CreateNew();
    env_.ClearFaults();
    EXPECT_TRUE(s.IsIOError()) << s.ToString();
    EXPECT_TRUE(DbFiles().empty());
  }
}

TEST_F(VersionSetTest, CreateNewWithAFailedDirectorySyncLeavesAWholeDatabaseOrNone) {
  // The rename happened and was not made durable. A power loss keeps it or not; what it keeps is a
  // database that opens, and if it takes the rename it takes the CURRENT with it.
  for (const bool drop_all : {true, false}) {
    env_.FailOpAfter(Op::kSyncDir, 0);
    const auto versions = NewVersionSet();
    EXPECT_TRUE(versions->CreateNew().IsIOError());
    env_.ClearFaults();
    if (drop_all) {
      env_.SimulatePowerLossDropUnsynced();
    } else {
      env_.SimulatePowerLoss();
    }
    if (env_.FileExists(CurrentFileName(kDb))) {
      const auto recovered = NewVersionSet();
      ASSERT_OK(recovered->Recover());
      EXPECT_EQ(recovered->current()->state(), VersionState{});
    }
    for (const std::string& name : DbFiles()) {
      ASSERT_OK(env_.RemoveFile(std::string(kDb) + "/" + name));
    }
  }
}

TEST_F(VersionSetTest, LogAndApplyThenRecover) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  const uint64_t level0_file = versions->NewFileNumber();
  const uint64_t level1_file = versions->NewFileNumber();
  const uint64_t log_number = versions->NewFileNumber();
  EXPECT_EQ(level0_file, 2u);
  EXPECT_EQ(level1_file, 3u);
  EXPECT_EQ(log_number, 4u);

  VersionEdit first = AddFile(0, Meta(level0_file, "a", "m"));
  first.log_number = log_number;
  versions->SetLastSequence(10);
  ASSERT_OK(versions->LogAndApply(first));

  VersionEdit second = AddFile(1, Meta(level1_file, "n", "z"));
  second.compact_pointers.emplace_back(1, IKey("n", 5));
  versions->SetLastSequence(20);
  ASSERT_OK(versions->LogAndApply(second));

  const VersionState& state = versions->current()->state();
  ASSERT_EQ(state.files[0].size(), 1u);
  ASSERT_EQ(state.files[1].size(), 1u);
  EXPECT_EQ(state.files[0][0], Meta(level0_file, "a", "m"));
  EXPECT_EQ(state.files[1][0], Meta(level1_file, "n", "z"));
  EXPECT_EQ(state.compact_pointers[1], IKey("n", 5));
  EXPECT_EQ(versions->current()->NumFiles(0), 1);
  EXPECT_EQ(versions->current()->NumFiles(1), 1);
  EXPECT_EQ(versions->current()->NumFiles(2), 0);

  const auto recovered = NewVersionSet();
  ASSERT_OK(recovered->Recover());
  EXPECT_EQ(recovered->current()->state(), state);
  EXPECT_EQ(recovered->LogNumber(), log_number);
  EXPECT_EQ(recovered->NextFileNumber(), 5u);
  EXPECT_EQ(recovered->NextFileNumber(), versions->NextFileNumber());
  EXPECT_EQ(recovered->LastSequence(), 20u);
  EXPECT_EQ(recovered->ManifestFileNumber(), 1u);
}

TEST_F(VersionSetTest, EveryRecordCarriesTheCounters) {
  // A record that lacks next_file_number would let a recovery that stopped there hand out a number
  // that a file in the state already has.
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  const uint64_t a = versions->NewFileNumber();
  versions->SetLastSequence(7);
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(a, "a", "b"))));
  const uint64_t b = versions->NewFileNumber();
  versions->SetLastSequence(9);
  // The counters the caller put in the edit are replaced: these are lower than the version set's,
  // and an edit that kept them would be refused as moving a counter back.
  VersionEdit edit = AddFile(0, Meta(b, "c", "d"));
  edit.next_file_number = 1;
  edit.last_sequence = 0;
  ASSERT_OK(versions->LogAndApply(edit));

  const ManifestRecords records = ReadManifest(1);
  ASSERT_EQ(records.edits.size(), 3u);
  for (const VersionEdit& record : records.edits) {
    ASSERT_TRUE(record.next_file_number.has_value());
    ASSERT_TRUE(record.last_sequence.has_value());
  }
  EXPECT_EQ(*records.edits[1].next_file_number, a + 1);
  EXPECT_EQ(*records.edits[1].last_sequence, 7u);
  EXPECT_EQ(*records.edits[2].next_file_number, b + 1);
  EXPECT_EQ(*records.edits[2].last_sequence, 9u);
}

TEST_F(VersionSetTest, FileNumbersNeverRepeat) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  EXPECT_EQ(versions->NewFileNumber(), 2u);
  EXPECT_EQ(versions->NewFileNumber(), 3u);
  versions->MarkFileNumberUsed(20);
  EXPECT_EQ(versions->NextFileNumber(), 21u);
  versions->MarkFileNumberUsed(5);  // already below: no effect
  EXPECT_EQ(versions->NextFileNumber(), 21u);
  versions->MarkFileNumberUsed(21);
  EXPECT_EQ(versions->NewFileNumber(), 22u);
}

TEST_F(VersionSetTest, LastSequenceMovesOnlyForward) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  versions->SetLastSequence(5);
  versions->SetLastSequence(5);
  versions->SetLastSequence(6);
  EXPECT_EQ(versions->LastSequence(), 6u);
}

TEST_F(VersionSetTest, WriteNewManifestSwitchesCurrent) {
  const auto first = NewVersionSet();
  ASSERT_OK(first->CreateNew());
  const uint64_t level0_file = first->NewFileNumber();
  ASSERT_OK(first->LogAndApply(AddFile(0, Meta(level0_file, "a", "m"))));

  const auto versions = NewVersionSet();
  ASSERT_OK(versions->Recover());
  ASSERT_EQ(versions->ManifestFileNumber(), 1u);
  const uint64_t level1_file = versions->NewFileNumber();
  versions->SetLastSequence(30);
  ASSERT_OK(versions->WriteNewManifest(AddFile(1, Meta(level1_file, "n", "z"))));

  // CURRENT names a MANIFEST with a number above every one used so far, and the version set says
  // the same.
  const uint64_t switched = versions->ManifestFileNumber();
  EXPECT_GT(switched, level1_file);
  EXPECT_EQ(CurrentContents(), ManifestName(switched) + "\n");
  EXPECT_GT(versions->NextFileNumber(), switched);

  // The new MANIFEST is a snapshot of the new state and nothing else.
  const ManifestRecords records = ReadManifest(switched);
  ASSERT_EQ(records.edits.size(), 1u);
  EXPECT_EQ(records.edits[0], SnapshotEdit(versions->current()->state()));
  ASSERT_TRUE(records.edits[0].next_file_number.has_value());
  EXPECT_GT(*records.edits[0].next_file_number, switched)
      << "the snapshot's counter is above its own MANIFEST";
  EXPECT_TRUE(env_.FileExists(DescriptorFileName(kDb, 1)))
      << "the old MANIFEST is the database's to delete";

  // Recovering from it gives the state that was installed, the edit and the old files included.
  const auto recovered = NewVersionSet();
  ASSERT_OK(recovered->Recover());
  EXPECT_EQ(recovered->current()->state(), versions->current()->state());
  EXPECT_EQ(recovered->current()->NumFiles(0), 1);
  EXPECT_EQ(recovered->current()->NumFiles(1), 1);
  EXPECT_EQ(recovered->LastSequence(), 30u);
  EXPECT_EQ(recovered->NextFileNumber(), versions->NextFileNumber());
  EXPECT_EQ(recovered->ManifestFileNumber(), switched);

  // And the version set goes on appending to the new MANIFEST, not the old.
  const uint64_t later = versions->NewFileNumber();
  ASSERT_OK(versions->LogAndApply(AddFile(2, Meta(later, "a", "z"))));
  EXPECT_EQ(ReadManifest(switched).edits.size(), 2u);
  EXPECT_EQ(ReadManifest(1).edits.size(), 2u);
  const auto again = NewVersionSet();
  ASSERT_OK(again->Recover());
  EXPECT_EQ(again->current()->state(), versions->current()->state());
}

TEST_F(VersionSetTest, WriteNewManifestSurvivesPowerLoss) {
  // The edit given to WriteNewManifest lives only in the new MANIFEST, so a CURRENT switch that was
  // not durable would take it back, and with it the files the edit adds.
  const auto first = NewVersionSet();
  ASSERT_OK(first->CreateNew());
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->Recover());
  const uint64_t file = versions->NewFileNumber();
  ASSERT_OK(versions->WriteNewManifest(AddFile(0, Meta(file, "a", "m"))));
  env_.SimulatePowerLossDropUnsynced();

  const auto recovered = NewVersionSet();
  ASSERT_OK(recovered->Recover());
  EXPECT_EQ(recovered->current()->state(), versions->current()->state());
  EXPECT_EQ(recovered->current()->NumFiles(0), 1);
}

TEST_F(VersionSetTest, WriteNewManifestWithAnInvalidEditChangesNothing) {
  const auto first = NewVersionSet();
  ASSERT_OK(first->CreateNew());
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->Recover());
  const std::string current = CurrentContents();
  const std::vector<std::string> files = DbFiles();
  const Status s = versions->WriteNewManifest(DeleteFile(0, 99));
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(CurrentContents(), current);
  EXPECT_EQ(DbFiles(), files);
}

TEST_F(VersionSetTest, FailedWriteNewManifestLeavesTheVersionSetAsItWas) {
  // Each call the switch makes, failed in turn, before CURRENT is replaced: the new MANIFEST is not
  // kept, CURRENT names the old one, and the version set goes on appending to the old one.
  for (const Op op : {Op::kNewWritable, Op::kAppend, Op::kSync, Op::kClose, Op::kRename}) {
    SCOPED_TRACE(static_cast<int>(op));
    env_.ClearFaults();
    for (const std::string& name : DbFiles()) {
      ASSERT_OK(env_.RemoveFile(std::string(kDb) + "/" + name));
    }
    const auto versions = NewVersionSet();
    ASSERT_OK(versions->CreateNew());
    const VersionState before = versions->current()->state();

    env_.FailOpAfter(op, 0);
    const Status s =
        versions->WriteNewManifest(AddFile(0, Meta(versions->NewFileNumber(), "a", "b")));
    env_.ClearFaults();
    EXPECT_TRUE(s.IsIOError()) << s.ToString();
    EXPECT_EQ(CurrentContents(), "MANIFEST-000001\n");
    EXPECT_EQ(DbFiles(), (std::vector<std::string>{"CURRENT", "MANIFEST-000001"}))
        << "no new MANIFEST and no temporary file is left";
    EXPECT_EQ(versions->current()->state(), before) << "nothing is installed after a failure";
    EXPECT_EQ(versions->ManifestFileNumber(), 1u);

    // The old MANIFEST is still the open one.
    ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "c", "d"))));
    const auto recovered = NewVersionSet();
    ASSERT_OK(recovered->Recover());
    EXPECT_EQ(recovered->current()->state(), versions->current()->state());

    // And a switch that is tried again goes through.
    ASSERT_OK(versions->WriteNewManifest(AddFile(0, Meta(versions->NewFileNumber(), "e", "f"))));
    const auto switched = NewVersionSet();
    ASSERT_OK(switched->Recover());
    EXPECT_EQ(switched->current()->state(), versions->current()->state());
    EXPECT_EQ(switched->current()->NumFiles(0), 2);
  }
}

TEST_F(VersionSetTest, FailedDirectorySyncOfTheSwitchStopsTheVersionSet) {
  // The rename was done and its durability is not known, so CURRENT may name either MANIFEST. The
  // new one holds the whole state and the edit, and the old one holds the state without it: the
  // version set writes to neither until a switch succeeds, since an edit written to the old one
  // would be lost if CURRENT names the new.
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  const std::shared_ptr<const Version> before = versions->current();
  const uint64_t file = versions->NewFileNumber();
  env_.FailOpAfter(Op::kSyncDir, 0);
  const Status s = versions->WriteNewManifest(AddFile(0, Meta(file, "a", "b")));
  env_.ClearFaults();
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(versions->current(), before);

  // The new MANIFEST is kept, and is what CURRENT names now.
  const std::string current = CurrentContents();
  EXPECT_NE(current, "MANIFEST-000001\n");
  const auto recovered = NewVersionSet();
  ASSERT_OK(recovered->Recover());
  EXPECT_EQ(recovered->current()->NumFiles(0), 1) << "the new MANIFEST holds the edit";

  const std::string old_manifest = env_.FileContents(DescriptorFileName(kDb, 1));
  const Status refused =
      versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "c", "d")));
  EXPECT_TRUE(refused.IsIOError()) << refused.ToString();
  EXPECT_EQ(env_.FileContents(DescriptorFileName(kDb, 1)), old_manifest);
  EXPECT_EQ(versions->current(), before);
  EXPECT_EQ(CurrentContents(), current);

  ASSERT_OK(versions->WriteNewManifest(VersionEdit{}));
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "c", "d"))));
  const auto again = NewVersionSet();
  ASSERT_OK(again->Recover());
  EXPECT_EQ(again->current()->state(), versions->current()->state());
}

TEST_F(VersionSetTest, CurrentWithoutNewlineIsCorruption) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  for (const std::string contents : {"MANIFEST-000001", "", "MANIFEST-000001\nMANIFEST"}) {
    SCOPED_TRACE(contents);
    env_.SetFileContents(CurrentFileName(kDb), contents);
    const auto recovered = NewVersionSet();
    const Status s = recovered->Recover();
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    EXPECT_NE(s.ToString().find("CURRENT file does not end with newline"), std::string::npos)
        << s.ToString();
  }
}

TEST_F(VersionSetTest, CurrentNamingGarbageIsCorruption) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  // Not a MANIFEST name, a name of another kind of file, a MANIFEST name with something after it, a
  // name that leaves the directory, and a number spelled another way than SetCurrentFile() does.
  for (const std::string contents :
       {"garbage\n", "000001.sst\n", "CURRENT\n", "MANIFEST-000001\n\n", "MANIFEST-000001 \n",
        "MANIFEST-\n", "\n", "../MANIFEST-000001\n", "/db/MANIFEST-000001\n", "MANIFEST-1\n",
        "MANIFEST-0000001\n"}) {
    SCOPED_TRACE(contents);
    env_.SetFileContents(CurrentFileName(kDb), contents);
    const auto recovered = NewVersionSet();
    const Status s = recovered->Recover();
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    EXPECT_NE(s.ToString().find("CURRENT names an invalid file"), std::string::npos)
        << s.ToString();
  }
}

TEST_F(VersionSetTest, MissingCurrentOrManifestIsAnIOError) {
  const auto recovered = NewVersionSet();
  const Status no_current = recovered->Recover();
  EXPECT_TRUE(no_current.IsIOError()) << no_current.ToString();

  env_.SetFileContents(CurrentFileName(kDb), "MANIFEST-000009\n");
  const auto other = NewVersionSet();
  const Status no_manifest = other->Recover();
  EXPECT_TRUE(no_manifest.IsIOError()) << no_manifest.ToString();
}

TEST_F(VersionSetTest, CorruptManifestRecordIsCorruption) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "a", "b"))));
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "c", "d"))));
  const ManifestRecords records = ReadManifest(1);
  ASSERT_EQ(records.offsets.size(), 3u);

  // A flipped byte in any record is a defect the reader reports, and a defect is not the end of the
  // log wherever it falls: dropping the records after it would lose edits that were acknowledged.
  // The last record is the one a torn tail would be taken for. The first payload byte of a record
  // is a tag, which a flip makes unknown; the last is part of a number or a key, which a flip
  // leaves a valid record with other contents, and only the checksum can tell.
  const std::string intact = env_.FileContents(DescriptorFileName(kDb, 1));
  for (size_t record = 0; record < records.offsets.size(); record++) {
    const size_t start = static_cast<size_t>(records.offsets[record]);
    const size_t end = record + 1 < records.offsets.size()
                           ? static_cast<size_t>(records.offsets[record + 1])
                           : intact.size();
    for (const size_t position : {start + 7, end - 1}) {  // after the 7-byte header; the last byte
      SCOPED_TRACE(testing::Message() << "record " << record << ", byte " << position);
      std::string damaged = intact;
      damaged[position] ^= 0x40;
      env_.SetFileContents(DescriptorFileName(kDb, 1), damaged);
      const auto recovered = NewVersionSet();
      const Status s = recovered->Recover();
      EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    }
  }
}

TEST_F(VersionSetTest, TornManifestTailIsNotCorruption) {
  // A crash while an edit was being appended leaves a record cut short. The edit was never
  // acknowledged, because its Sync() had not returned, so recovery goes on without it.
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "a", "b"))));
  const VersionState after_first = versions->current()->state();
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "c", "d"))));

  std::string contents = env_.FileContents(DescriptorFileName(kDb, 1));
  contents.pop_back();
  env_.SetFileContents(DescriptorFileName(kDb, 1), contents);
  const auto recovered = NewVersionSet();
  ASSERT_OK(recovered->Recover());
  EXPECT_EQ(recovered->current()->state(), after_first);
}

TEST_F(VersionSetTest, ManifestThatHoldsNoRecordIsCorruption) {
  // Recovering "nothing" from an empty file would open an empty database over tables that are all
  // still there, and the first flush would start writing over their numbers.
  WriteManifest(3, {});
  const auto recovered = NewVersionSet();
  const Status s = recovered->Recover();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(VersionSetTest, UndecodableRecordIsCorruption) {
  WriteManifest(3, {Encode(SnapshotEdit(VersionState{})), std::string("\xff\xff\xff", 3)});
  const auto recovered = NewVersionSet();
  const Status s = recovered->Recover();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(VersionSetTest, RecordThatApplyEditRefusesIsCorruption) {
  // Intact and decodable, and a history no database had: it deletes a file that was never added.
  WriteManifest(3, {Encode(SnapshotEdit(VersionState{})), Encode(DeleteFile(0, 99))});
  const auto recovered = NewVersionSet();
  const Status s = recovered->Recover();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(VersionSetTest, LastSequenceBeyondTheLargestIsCorruption) {
  // No write can be given a sequence number above kMaxSequenceNumber, so the next one after this
  // would abort the process.
  VersionState state;
  state.last_sequence = kMaxSequenceNumber + 1;
  WriteManifest(3, {Encode(SnapshotEdit(state))});
  const auto recovered = NewVersionSet();
  const Status s = recovered->Recover();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();

  state.last_sequence = kMaxSequenceNumber;
  WriteManifest(4, {Encode(SnapshotEdit(state))});
  const auto accepted = NewVersionSet();
  ASSERT_OK(accepted->Recover());
  EXPECT_EQ(accepted->LastSequence(), kMaxSequenceNumber);
}

TEST_F(VersionSetTest, RecoverKeepsFileNumbersAboveTheManifestItReadFrom) {
  // The snapshot says the next number is 2, and the MANIFEST that holds it is number 9: one the
  // snapshot's counter does not account for. A number handed out later must still be above it.
  WriteManifest(9, {Encode(SnapshotEdit(VersionState{}))});
  const auto recovered = NewVersionSet();
  ASSERT_OK(recovered->Recover());
  EXPECT_EQ(recovered->ManifestFileNumber(), 9u);
  EXPECT_EQ(recovered->NextFileNumber(), 10u);
  EXPECT_EQ(recovered->NewFileNumber(), 10u);
}

TEST_F(VersionSetTest, RecoverRefusesAReadErrorAsEndOfFile) {
  // A MANIFEST that cannot be read is not a short one. Fail each read in turn: a recovery either
  // fails with the error or has read the whole MANIFEST, and never settles for part of it.
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "a", "b"))));
  ASSERT_OK(versions->LogAndApply(AddFile(1, Meta(versions->NewFileNumber(), "c", "d"))));
  const VersionState expected = versions->current()->state();

  int failures = 0;
  for (int successes = 0; successes < 16; successes++) {
    env_.ClearFaults();
    env_.FailOpAfter(Op::kRead, successes);
    const auto recovered = NewVersionSet();
    const Status s = recovered->Recover();
    env_.ClearFaults();
    if (s.ok()) {
      EXPECT_EQ(recovered->current()->state(), expected) << "read " << successes;
      break;
    }
    failures++;
    EXPECT_TRUE(s.IsIOError()) << s.ToString();
  }
  EXPECT_GT(failures, 0);
}

TEST_F(VersionSetTest, FailedRecoverInstallsNothing) {
  WriteManifest(3, {Encode(SnapshotEdit(VersionState{})), Encode(DeleteFile(0, 99))});
  const auto recovered = NewVersionSet();
  ASSERT_FALSE(recovered->Recover().ok());
  EXPECT_EQ(recovered->current()->state(), VersionState{});
  EXPECT_EQ(recovered->NextFileNumber(), 2u);
  EXPECT_EQ(recovered->LastSequence(), 0u);
}

TEST_F(VersionSetTest, AcknowledgedEditSurvivesPowerLoss) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "a", "b"))));
  env_.SimulatePowerLossDropUnsynced();
  const auto recovered = NewVersionSet();
  ASSERT_OK(recovered->Recover());
  EXPECT_EQ(recovered->current()->state(), versions->current()->state());
  EXPECT_EQ(recovered->current()->NumFiles(0), 1);
}

TEST_F(VersionSetTest, FailedManifestSyncInstallsNothing) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  const std::shared_ptr<const Version> before = versions->current();
  const VersionState state_before = before->state();

  env_.FailOpAfter(Op::kSync, 0);
  const Status s = versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "a", "b")));
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(versions->current(), before);
  EXPECT_EQ(versions->current()->state(), state_before);
  EXPECT_EQ(versions->current()->NumFiles(0), 0);
}

TEST_F(VersionSetTest, FailedManifestAppendInstallsNothing) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  const std::shared_ptr<const Version> before = versions->current();
  env_.FailOpAfter(Op::kAppend, 0);
  const Status s = versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "a", "b")));
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(versions->current(), before);
}

TEST_F(VersionSetTest, FailedManifestWriteIsFinalUntilANewManifest) {
  // After a write or a sync of the MANIFEST fails, what the file holds is unknown, and a later
  // Sync() of it that succeeds proves nothing. So the version set stops writing to it, and every
  // later edit is refused with the first error until a new MANIFEST, with the whole state in it,
  // takes over.
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  env_.FailOpAfter(Op::kSync, 0);
  ASSERT_TRUE(
      versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "a", "b"))).IsIOError());
  env_.ClearFaults();

  const std::string manifest = env_.FileContents(DescriptorFileName(kDb, 1));
  const std::shared_ptr<const Version> before = versions->current();
  const Status again = versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "c", "d")));
  EXPECT_TRUE(again.IsIOError()) << again.ToString();
  EXPECT_EQ(env_.FileContents(DescriptorFileName(kDb, 1)), manifest) << "nothing more was written";
  EXPECT_EQ(versions->current(), before);

  // A new MANIFEST is a fresh file with a full snapshot, so it does not inherit the doubt.
  const uint64_t file = versions->NewFileNumber();
  ASSERT_OK(versions->WriteNewManifest(AddFile(0, Meta(file, "e", "f"))));
  const uint64_t after = versions->NewFileNumber();
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(after, "g", "h"))));
  const auto recovered = NewVersionSet();
  ASSERT_OK(recovered->Recover());
  EXPECT_EQ(recovered->current()->state(), versions->current()->state());
  EXPECT_EQ(recovered->current()->NumFiles(0), 2);
}

TEST_F(VersionSetTest, InvalidEditInstallsNothing) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "a", "b"))));
  const std::shared_ptr<const Version> before = versions->current();
  const VersionState state_before = before->state();
  const std::string manifest = env_.FileContents(DescriptorFileName(kDb, 1));

  const Status s = versions->LogAndApply(DeleteFile(0, 99));
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(versions->current(), before);
  EXPECT_EQ(versions->current()->state(), state_before);
  EXPECT_EQ(env_.FileContents(DescriptorFileName(kDb, 1)).size(), manifest.size());
  EXPECT_EQ(env_.FileContents(DescriptorFileName(kDb, 1)), manifest);

  // A refusal is not a failure of the MANIFEST: nothing was written, and the next edit goes
  // through.
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "c", "d"))));
  EXPECT_EQ(versions->current()->NumFiles(0), 2);
}

TEST_F(VersionSetTest, FileBeyondTheNumbersHandedOutIsRefused) {
  // A file numbered at or above the next number to give would be given again by the next flush.
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  const Status s = versions->LogAndApply(AddFile(0, Meta(versions->NextFileNumber(), "a", "b")));
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(versions->current()->NumFiles(0), 0);
}

TEST_F(VersionSetTest, InstalledVersionsAreImmutable) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  const std::shared_ptr<const Version> empty = versions->current();
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(versions->NewFileNumber(), "a", "b"))));
  EXPECT_NE(versions->current(), empty);
  EXPECT_EQ(empty->NumFiles(0), 0);
  EXPECT_EQ(versions->current()->NumFiles(0), 1);
}

TEST_F(VersionSetTest, LiveFilesFollowsHeldVersions) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  const uint64_t two = versions->NewFileNumber();
  const uint64_t three = versions->NewFileNumber();
  VersionEdit add;
  add.new_files.emplace_back(0, Meta(two, "a", "m"));
  add.new_files.emplace_back(1, Meta(three, "n", "z"));
  ASSERT_OK(versions->LogAndApply(add));
  EXPECT_EQ(versions->LiveFiles(), (std::set<uint64_t>{two, three}));

  // Compact #2 away: its output is #4 at level 1, and no current file is #2.
  std::shared_ptr<const Version> old = versions->current();
  const uint64_t four = versions->NewFileNumber();
  VersionEdit compact = DeleteFile(0, two);
  compact.new_files.emplace_back(1, Meta(four, "a", "m"));
  ASSERT_OK(versions->LogAndApply(compact));
  EXPECT_EQ(versions->current()->NumFiles(0), 0);

  // A reader still holds the version that has #2, so #2 must not be deleted.
  EXPECT_EQ(versions->LiveFiles(), (std::set<uint64_t>{two, three, four}));
  old.reset();
  EXPECT_EQ(versions->LiveFiles(), (std::set<uint64_t>{three, four}));
}

TEST_F(VersionSetTest, LiveFilesCountsEveryHeldVersion) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  const uint64_t two = versions->NewFileNumber();
  const uint64_t three = versions->NewFileNumber();
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(two, "a", "b"))));
  std::shared_ptr<const Version> first = versions->current();
  ASSERT_OK(versions->LogAndApply(DeleteFile(0, two)));
  ASSERT_OK(versions->LogAndApply(AddFile(0, Meta(three, "c", "d"))));
  std::shared_ptr<const Version> second = versions->current();
  ASSERT_OK(versions->LogAndApply(DeleteFile(0, three)));
  EXPECT_EQ(versions->current()->NumFiles(0), 0);

  EXPECT_EQ(versions->LiveFiles(), (std::set<uint64_t>{two, three}));
  first.reset();
  EXPECT_EQ(versions->LiveFiles(), (std::set<uint64_t>{three}));
  second.reset();
  EXPECT_TRUE(versions->LiveFiles().empty());
}

TEST_F(VersionSetTest, LiveFilesAfterRecoverAreTheCurrentOnes) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  const uint64_t two = versions->NewFileNumber();
  ASSERT_OK(versions->LogAndApply(AddFile(2, Meta(two, "a", "b"))));
  const auto recovered = NewVersionSet();
  ASSERT_OK(recovered->Recover());
  EXPECT_EQ(recovered->LiveFiles(), std::set<uint64_t>{two});
}

using VersionSetDeathTest = VersionSetTest;

TEST_F(VersionSetDeathTest, LastSequenceCannotGoBack) {
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->CreateNew());
  versions->SetLastSequence(10);
  EXPECT_DEATH(versions->SetLastSequence(9), "sequence");
}

TEST_F(VersionSetDeathTest, LogAndApplyNeedsAnOpenManifest) {
  // After Recover() the MANIFEST that was read cannot be appended to, since it may end in a torn
  // record; WriteNewManifest() opens one that can.
  const auto first = NewVersionSet();
  ASSERT_OK(first->CreateNew());
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->Recover());
  EXPECT_DEATH(static_cast<void>(versions->LogAndApply(VersionEdit{})), "MANIFEST");
}

TEST_F(VersionSetDeathTest, RecoverTwiceAborts) {
  const auto first = NewVersionSet();
  ASSERT_OK(first->CreateNew());
  const auto versions = NewVersionSet();
  ASSERT_OK(versions->Recover());
  EXPECT_DEATH(static_cast<void>(versions->Recover()), "once");
}

// Version: what a read finds

// One entry of a test table.
struct Item {
  std::string user_key;
  SequenceNumber sequence;
  ValueType type;
  std::string value;
};

Item Put(std::string_view key, SequenceNumber sequence, std::string_view value) {
  return {std::string(key), sequence, ValueType::kValue, std::string(value)};
}

Item Del(std::string_view key, SequenceNumber sequence) {
  return {std::string(key), sequence, ValueType::kDeletion, ""};
}

class VersionGetTest : public VersionSetFixture {
 protected:
  // Writes table `number` holding `items`, in any order, and returns its description.
  FileMetaData MakeTable(uint64_t number, std::vector<Item> items) {
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
      return CompareInternalKey(IKey(a.user_key, a.sequence, a.type),
                                IKey(b.user_key, b.sequence, b.type)) < 0;
    });
    std::unique_ptr<WritableFile> file;
    EXPECT_OK(env_.NewWritableFile(TableFileName(kDb, number), &file));
    FileMetaData meta;
    meta.number = number;
    TableBuilder builder(TableBuilderOptions{}, file.get());
    for (const Item& item : items) {
      builder.Add(IKey(item.user_key, item.sequence, item.type), item.value);
    }
    EXPECT_OK(builder.Finish());
    EXPECT_OK(file->Close());
    meta.file_size = builder.FileSize();
    meta.smallest = IKey(items.front().user_key, items.front().sequence, items.front().type);
    meta.largest = IKey(items.back().user_key, items.back().sequence, items.back().type);
    return meta;
  }

  // A version holding `files`, each with its level; checked against the rules of a state.
  std::shared_ptr<const Version> MakeVersion(
      const std::vector<std::pair<int, FileMetaData>>& files) {
    VersionEdit edit;
    edit.next_file_number = 1000;
    edit.new_files = files;
    VersionState state;
    EXPECT_OK(ApplyEdit(VersionState{}, edit, &state));
    return std::make_shared<const Version>(std::move(state), &table_cache_);
  }

  struct Outcome {
    Status status;
    std::string value;
    int probed = -1;
  };

  static Outcome Get(const Version& version, std::string_view key,
                     SequenceNumber sequence = kMaxSequenceNumber) {
    Outcome outcome;
    outcome.status =
        version.Get(TableReadOptions{}, LookupKey(key, sequence), &outcome.value, &outcome.probed);
    return outcome;
  }
};

TEST_F(VersionGetTest, NewerL0Wins) {
  const auto version = MakeVersion(
      {{0, MakeTable(4, {Put("k", 1, "old")})}, {0, MakeTable(5, {Put("k", 2, "new")})}});
  const Outcome got = Get(*version, "k");
  ASSERT_OK(got.status);
  EXPECT_EQ(got.value, "new");
  EXPECT_EQ(got.probed, 1);
}

TEST_F(VersionGetTest, ProbedIsOptional) {
  const auto version = MakeVersion({{0, MakeTable(4, {Put("k", 1, "v")})}});
  std::string value;
  ASSERT_OK(version->Get(TableReadOptions{}, LookupKey("k", kMaxSequenceNumber), &value, nullptr));
  EXPECT_EQ(value, "v");
}

TEST_F(VersionGetTest, SnapshotSeesTheOlderEntryBehindANewerFile) {
  // The newer file holds the key only at a sequence the snapshot cannot see. It is consulted, finds
  // nothing it may return, and the older file answers.
  const auto version = MakeVersion(
      {{0, MakeTable(4, {Put("k", 1, "old")})}, {0, MakeTable(5, {Put("k", 7, "new")})}});
  const Outcome got = Get(*version, "k", 3);
  ASSERT_OK(got.status);
  EXPECT_EQ(got.value, "old");
  EXPECT_EQ(got.probed, 2);
}

TEST_F(VersionGetTest, DeletionInNewerFileHides) {
  const auto version =
      MakeVersion({{0, MakeTable(4, {Put("k", 1, "old")})}, {0, MakeTable(5, {Del("k", 3)})}});
  const Outcome got = Get(*version, "k");
  EXPECT_TRUE(got.status.IsNotFound()) << got.status.ToString();
  EXPECT_EQ(got.probed, 1);

  // A snapshot from before the deletion still sees the value.
  const Outcome earlier = Get(*version, "k", 2);
  ASSERT_OK(earlier.status);
  EXPECT_EQ(earlier.value, "old");
}

TEST_F(VersionGetTest, DeletionInShallowLevelHidesDeeperValue) {
  const auto version =
      MakeVersion({{1, MakeTable(6, {Del("k", 5)})}, {2, MakeTable(7, {Put("k", 1, "buried")})}});
  const Outcome got = Get(*version, "k");
  EXPECT_TRUE(got.status.IsNotFound()) << got.status.ToString();
  EXPECT_EQ(got.probed, 1);
}

TEST_F(VersionGetTest, L0FilesThatDoNotCoverTheKeyAreNotProbed) {
  const auto version = MakeVersion({{0, MakeTable(4, {Put("a", 1, "a4"), Put("c", 2, "c4")})},
                                    {0, MakeTable(5, {Put("m", 3, "m5"), Put("p", 4, "p5")})},
                                    {0, MakeTable(6, {Put("x", 5, "x6"), Put("z", 6, "z6")})}});
  const Outcome got = Get(*version, "b");
  EXPECT_TRUE(got.status.IsNotFound()) << got.status.ToString();
  EXPECT_EQ(got.probed, 1) << "only #4 covers b, though it holds no entry for it";

  const Outcome in_5 = Get(*version, "n");
  EXPECT_TRUE(in_5.status.IsNotFound());
  EXPECT_EQ(in_5.probed, 1);

  const Outcome none = Get(*version, "e");
  EXPECT_TRUE(none.status.IsNotFound());
  EXPECT_EQ(none.probed, 0);

  const Outcome at_end = Get(*version, "z");
  ASSERT_OK(at_end.status);
  EXPECT_EQ(at_end.value, "z6");
  EXPECT_EQ(at_end.probed, 1);
}

TEST_F(VersionGetTest, L0FileThatCoversTheKeyButLacksItIsProbedAndPassedOver) {
  const auto version =
      MakeVersion({{0, MakeTable(4, {Put("a", 1, "a4"), Put("m", 2, "m4"), Put("z", 3, "z4")})},
                   {0, MakeTable(5, {Put("a", 4, "a5"), Put("z", 5, "z5")})}});
  const Outcome got = Get(*version, "m");
  ASSERT_OK(got.status);
  EXPECT_EQ(got.value, "m4");
  EXPECT_EQ(got.probed, 2);
}

TEST_F(VersionGetTest, DeeperLevelsBinarySearch) {
  const auto version = MakeVersion({{1, MakeTable(10, {Put("a", 1, "va"), Put("c", 2, "vc")})},
                                    {1, MakeTable(11, {Put("e", 3, "ve"), Put("g", 4, "vg")})},
                                    {1, MakeTable(12, {Put("i", 5, "vi"), Put("k", 6, "vk")})}});
  for (const std::string key : {"a", "c", "e", "g", "i", "k"}) {
    SCOPED_TRACE(key);
    const Outcome got = Get(*version, key);
    ASSERT_OK(got.status);
    EXPECT_EQ(got.value, "v" + key);
    EXPECT_EQ(got.probed, 1) << "one file of a level can hold a key";
  }
  // In a gap between two files, before the first and after the last: no table holds it, so none
  // is read.
  for (const std::string key : {"0", "d", "h", "z"}) {
    SCOPED_TRACE(key);
    const Outcome got = Get(*version, key);
    EXPECT_TRUE(got.status.IsNotFound()) << got.status.ToString();
    EXPECT_EQ(got.probed, 0);
  }
  // Inside a file's range but not in it: the one file is read and has nothing.
  for (const std::string key : {"b", "f", "j"}) {
    SCOPED_TRACE(key);
    const Outcome got = Get(*version, key);
    EXPECT_TRUE(got.status.IsNotFound());
    EXPECT_EQ(got.probed, 1);
  }
}

TEST_F(VersionGetTest, FallsThroughLevelsInOrder) {
  const auto version = MakeVersion({{0, MakeTable(9, {Put("x", 9, "x9")})},
                                    {1, MakeTable(8, {Put("a", 8, "a8"), Put("z", 8, "z8")})},
                                    {2, MakeTable(7, {Put("m", 7, "m7")})},
                                    {3, MakeTable(6, {Put("m", 1, "m6")})}});
  const Outcome shallow = Get(*version, "m");
  ASSERT_OK(shallow.status);
  EXPECT_EQ(shallow.value, "m7") << "the shallower level holds the newer entry";
  // L0's #9 does not cover m, L1's #8 does (a..z) and is read, L2's #7 answers.
  EXPECT_EQ(shallow.probed, 2);

  const Outcome absent = Get(*version, "q");
  EXPECT_TRUE(absent.status.IsNotFound());
  EXPECT_EQ(absent.probed, 1) << "L1's #8 is the only file that covers q";
}

TEST_F(VersionGetTest, MissingTableFileIsAnErrorNotAMiss) {
  // A MANIFEST that names a file the disk does not have must not read as "the key is absent".
  FileMetaData ghost = Meta(5, "a", "z");
  const auto version = MakeVersion({{0, ghost}, {1, MakeTable(6, {Put("k", 1, "v")})}});
  const Outcome got = Get(*version, "k");
  EXPECT_TRUE(got.status.IsIOError()) << got.status.ToString();
}

TEST_F(VersionGetTest, AnEmptyVersionHasNothing) {
  const auto version = MakeVersion({});
  const Outcome got = Get(*version, "k");
  EXPECT_TRUE(got.status.IsNotFound());
  EXPECT_EQ(got.probed, 0);
  std::vector<std::unique_ptr<Iterator>> iters;
  version->AddIterators(TableReadOptions{}, &iters);
  EXPECT_TRUE(iters.empty());
}

// Version::AddIterators

class VersionIteratorTest : public VersionGetTest {
 protected:
  static Entries Drain(Iterator* iter) {
    Entries entries;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      entries.emplace_back(std::string(iter->key()), std::string(iter->value()));
    }
    return entries;
  }

  static std::string Ik(std::string_view key, SequenceNumber sequence) {
    return IKey(key, sequence);
  }
};

TEST_F(VersionIteratorTest, OneIteratorPerL0FileThenOnePerNonEmptyLevel) {
  const auto version = MakeVersion({{0, MakeTable(4, {Put("a", 1, "a4"), Put("b", 2, "b4")})},
                                    {0, MakeTable(5, {Put("b", 5, "b5"), Put("d", 6, "d5")})},
                                    {1, MakeTable(6, {Put("e", 1, "e6"), Put("f", 2, "f6")})},
                                    {1, MakeTable(7, {Put("h", 3, "h7"), Put("j", 4, "j7")})},
                                    {3, MakeTable(8, {Put("k", 1, "k8")})}});
  std::vector<std::unique_ptr<Iterator>> iters;
  version->AddIterators(TableReadOptions{}, &iters);
  // Two for the two L0 files, one for level 1, none for the empty level 2, one for level 3.
  ASSERT_EQ(iters.size(), 4u);

  // L0 newest first: the merge breaks ties toward the lower index, so the order is the priority.
  iters[0]->SeekToFirst();
  ASSERT_TRUE(iters[0]->Valid());
  EXPECT_EQ(iters[0]->key(), Ik("b", 5));
  iters[1]->SeekToFirst();
  ASSERT_TRUE(iters[1]->Valid());
  EXPECT_EQ(iters[1]->key(), Ik("a", 1));

  // Level 1 reads as one sequence across its two files; level 3 holds its one.
  EXPECT_EQ(
      Drain(iters[2].get()),
      (Entries{{Ik("e", 1), "e6"}, {Ik("f", 2), "f6"}, {Ik("h", 3), "h7"}, {Ik("j", 4), "j7"}}));
  EXPECT_OK(iters[2]->status());
  EXPECT_EQ(Drain(iters[3].get()), (Entries{{Ik("k", 1), "k8"}}));
}

TEST_F(VersionIteratorTest, MergedIteratorsReadTheWholeDatabaseInOrder) {
  const auto version = MakeVersion({{0, MakeTable(4, {Put("a", 1, "a4"), Put("b", 2, "b4")})},
                                    {0, MakeTable(5, {Put("b", 5, "b5"), Put("d", 6, "d5")})},
                                    {1, MakeTable(6, {Put("e", 1, "e6"), Put("f", 2, "f6")})},
                                    {1, MakeTable(7, {Put("h", 3, "h7"), Put("j", 4, "j7")})},
                                    {3, MakeTable(8, {Put("k", 1, "k8")})}});
  std::vector<std::unique_ptr<Iterator>> iters;
  version->AddIterators(TableReadOptions{}, &iters);
  const std::unique_ptr<Iterator> merged = NewMergingIterator(std::move(iters), CompareInternalKey);
  EXPECT_EQ(Drain(merged.get()), (Entries{{Ik("a", 1), "a4"},
                                          {Ik("b", 5), "b5"},
                                          {Ik("b", 2), "b4"},
                                          {Ik("d", 6), "d5"},
                                          {Ik("e", 1), "e6"},
                                          {Ik("f", 2), "f6"},
                                          {Ik("h", 3), "h7"},
                                          {Ik("j", 4), "j7"},
                                          {Ik("k", 1), "k8"}}));
  EXPECT_OK(merged->status());
}

TEST_F(VersionIteratorTest, LevelIteratorSeeksAcrossFilesAndGaps) {
  const auto version = MakeVersion({{1, MakeTable(6, {Put("e", 1, "e6"), Put("f", 2, "f6")})},
                                    {1, MakeTable(7, {Put("h", 3, "h7"), Put("j", 4, "j7")})}});
  std::vector<std::unique_ptr<Iterator>> iters;
  version->AddIterators(TableReadOptions{}, &iters);
  ASSERT_EQ(iters.size(), 1u);
  Iterator& level = *iters[0];

  const auto seek = [&level](std::string_view user_key, SequenceNumber sequence) {
    level.Seek(IKey(user_key, sequence));
    return level.Valid() ? std::string(level.value()) : std::string("<end>");
  };
  EXPECT_EQ(seek("a", kMaxSequenceNumber), "e6") << "before the first file";
  EXPECT_EQ(seek("e", kMaxSequenceNumber), "e6");
  EXPECT_EQ(seek("f", 2), "f6") << "the last key of a file";
  EXPECT_EQ(seek("f", 1), "h7") << "just past the last key of a file";
  EXPECT_EQ(seek("g", kMaxSequenceNumber), "h7") << "the gap between two files";
  EXPECT_EQ(seek("j", kMaxSequenceNumber), "j7");
  EXPECT_EQ(seek("k", kMaxSequenceNumber), "<end>") << "past the last file";
  EXPECT_OK(level.status());

  // Seeking back is allowed, and finds the same.
  EXPECT_EQ(seek("e", kMaxSequenceNumber), "e6");
  level.Next();
  ASSERT_TRUE(level.Valid());
  EXPECT_EQ(level.value(), "f6");
  level.Next();
  ASSERT_TRUE(level.Valid());
  EXPECT_EQ(level.value(), "h7") << "Next() crosses from one file to the next";
}

TEST_F(VersionIteratorTest, LevelIteratorReportsAMissingFileAsAnError) {
  const auto version = MakeVersion({{1, MakeTable(6, {Put("e", 1, "e6"), Put("f", 2, "f6")})},
                                    {1, MakeTable(7, {Put("h", 3, "h7"), Put("j", 4, "j7")})}});
  ASSERT_OK(env_.RemoveFile(TableFileName(kDb, 7)));
  std::vector<std::unique_ptr<Iterator>> iters;
  version->AddIterators(TableReadOptions{}, &iters);
  ASSERT_EQ(iters.size(), 1u);
  const Entries entries = Drain(iters[0].get());
  EXPECT_EQ(entries, (Entries{{Ik("e", 1), "e6"}, {Ik("f", 2), "f6"}}));
  EXPECT_TRUE(iters[0]->status().IsIOError()) << iters[0]->status().ToString();
}

TEST_F(VersionIteratorTest, L0IteratorOfAMissingFileReportsTheError) {
  const auto version = MakeVersion({{0, Meta(5, "a", "z")}});
  std::vector<std::unique_ptr<Iterator>> iters;
  version->AddIterators(TableReadOptions{}, &iters);
  ASSERT_EQ(iters.size(), 1u);
  iters[0]->SeekToFirst();
  EXPECT_FALSE(iters[0]->Valid());
  EXPECT_TRUE(iters[0]->status().IsIOError()) << iters[0]->status().ToString();
}

TEST_F(VersionIteratorTest, AddIteratorsAppends) {
  const auto version = MakeVersion({{0, MakeTable(4, {Put("a", 1, "a4")})}});
  std::vector<std::unique_ptr<Iterator>> iters;
  iters.push_back(NewVectorIterator({}));
  version->AddIterators(TableReadOptions{}, &iters);
  EXPECT_EQ(iters.size(), 2u);
}

}  // namespace
}  // namespace shale
