// The compaction job on its own: input tables built directly, a plan made by hand, and the outputs
// read back. What the tests pin is what a compaction must never get wrong: an entry that a reader
// can still see is kept, one that no reader can see is dropped, no user key is split between two
// outputs, and a failure anywhere leaves no output behind.

#include "db/compaction_job.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/compaction_picker.h"
#include "core/version_edit.h"
#include "core/version_state.h"
#include "db/builder.h"
#include "db/handle_check_env.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "io/table_builder.h"
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

// A file the tests build is one table of the given entries, which are internal keys in order.
struct Input {
  uint64_t number = 0;
  Entries entries;
};

class CompactionJobTest : public testing::Test {
 protected:
  CompactionJobTest()
      : env_(5),
        checked_env_(&env_),
        block_cache_(size_t{1} << 20),
        build_cache_(kDb, &env_, &block_cache_, 100),
        job_cache_(kDb, &env_, &block_cache_, 100) {}

  void SetUp() override { ASSERT_OK(env_.CreateDir(kDb)); }

  // Every file the job opened for writing was closed, whatever became of the job.
  void TearDown() override { EXPECT_EQ(checked_env_.leaked(), 0) << "a file was not closed"; }

  // Builds the table `input` describes. A failure is fatal to the caller: call it as
  // ASSERT_NO_FATAL_FAILURE(Build(...)).
  void Build(const Input& input, FileMetaData* meta) {
    meta->number = input.number;
    const std::unique_ptr<Iterator> iter = NewVectorIterator(input.entries);
    ASSERT_OK(BuildTable(kDb, &env_, TableBuilderOptions{}, &build_cache_, iter.get(), meta));
  }

  // The context a job is given: its own cache, which starts cold so that the inputs are opened
  // and read as a database that has just opened them would, and file numbers from `next_number_`.
  CompactionJobContext Context(uint64_t target_file_size) {
    CompactionJobContext ctx;
    ctx.dbname = kDb;
    ctx.env = &checked_env_;
    ctx.table_cache = &job_cache_;
    ctx.target_file_size = target_file_size;
    ctx.allocate_file_number = [this] { return next_number_++; };
    ctx.between_outputs = [this] { between_outputs_calls_++; };
    return ctx;
  }

  // A version of `files` by level, and the plan that compacts `inputs` at `level`: the files of
  // `level` are `inputs[0]` and of the level below `inputs[1]`.
  struct Scenario {
    VersionState state;
    CompactionPlan plan;
  };

  // Builds every table of `levels`, lists them in a version, and plans the compaction of all the
  // files of `level` and of the level below it, with the files of the level below that as the
  // grandparents. A failure is fatal to the caller.
  void MakeScenario(const std::vector<std::vector<Input>>& levels, int level, Scenario* scenario) {
    scenario->state = VersionState{};
    scenario->state.next_file_number = 1000;
    scenario->plan = CompactionPlan{};
    scenario->plan.level = level;
    for (size_t l = 0; l < levels.size(); l++) {
      for (const Input& input : levels[l]) {
        FileMetaData meta;
        ASSERT_NO_FATAL_FAILURE(Build(input, &meta));
        scenario->state.files[l].push_back(meta);
        if (static_cast<int>(l) == level) {
          scenario->plan.inputs[0].push_back(meta);
        } else if (static_cast<int>(l) == level + 1) {
          scenario->plan.inputs[1].push_back(meta);
        } else if (static_cast<int>(l) == level + 2) {
          scenario->plan.grandparents.push_back(meta);
        }
      }
    }
    ASSERT_OK(CheckInvariants(scenario->state));
    const std::vector<FileMetaData>& top = scenario->plan.inputs[0];
    ASSERT_FALSE(top.empty());
    scenario->plan.next_compact_pointer = top.back().largest;
  }

  std::unique_ptr<CompactionJob> NewJob(const Scenario& scenario, uint64_t target_file_size,
                                        std::vector<SequenceNumber> snapshots = {}) {
    return std::make_unique<CompactionJob>(
        Context(target_file_size), scenario.plan,
        std::make_shared<const Version>(scenario.state, &job_cache_), std::move(snapshots));
  }

  // The entries of the table that `meta` describes, read as a fresh reader would.
  Entries ReadTable(const FileMetaData& meta) {
    const std::unique_ptr<Iterator> iter =
        build_cache_.NewIterator(TableReadOptions{}, meta.number, meta.file_size);
    Entries entries;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      entries.emplace_back(std::string(iter->key()), std::string(iter->value()));
    }
    EXPECT_OK(iter->status());
    return entries;
  }

  // The entries of every output, in order.
  Entries ReadOutputs(const CompactionJob& job) {
    Entries all;
    for (const FileMetaData& meta : job.outputs()) {
      const Entries entries = ReadTable(meta);
      all.insert(all.end(), entries.begin(), entries.end());
    }
    return all;
  }

  // The bare names of the table files in the directory.
  std::set<std::string> TableFiles() {
    std::vector<std::string> names;
    EXPECT_OK(env_.GetChildren(kDb, &names));
    std::set<std::string> tables;
    for (const std::string& name : names) {
      uint64_t number = 0;
      FileType type = FileType::kCurrentFile;
      if (ParseFileName(name, &number, &type) && type == FileType::kTableFile) {
        tables.insert(name);
      }
    }
    return tables;
  }

  static std::string Value(size_t bytes, char fill = 'v') { return std::string(bytes, fill); }

  FaultInjectionEnv env_;
  HandleCheckEnv checked_env_;  // what the job writes through
  BlockCache block_cache_;
  TableCache build_cache_;  // the one the inputs are built in and the outputs are read from
  TableCache job_cache_;    // the job's, cold at the start
  uint64_t next_number_ = 100;
  int between_outputs_calls_ = 0;
};

TEST_F(CompactionJobTest, MergesAndDropsShadowedVersions) {
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario(
      {{{2, {{IKey("a", 3), "x"}}}}, {{3, {{IKey("a", 1), "y"}, {IKey("b", 2), "z"}}}}}, 0,
      &scenario));
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 1 << 20);
  ASSERT_OK(job->Run());

  EXPECT_EQ(ReadOutputs(*job), (Entries{{IKey("a", 3), "x"}, {IKey("b", 2), "z"}}));
  ASSERT_EQ(job->outputs().size(), 1u);
  EXPECT_EQ(job->stats().shadowed_dropped, 1u);
  EXPECT_EQ(job->stats().tombstones_dropped, 0u);
  EXPECT_EQ(job->stats().entries_kept, 2u);
  EXPECT_EQ(job->stats().bytes_read,
            scenario.plan.inputs[0][0].file_size + scenario.plan.inputs[1][0].file_size);
  EXPECT_EQ(job->stats().bytes_written, job->outputs()[0].file_size);
  EXPECT_EQ(job->outputs()[0].smallest, IKey("a", 3));
  EXPECT_EQ(job->outputs()[0].largest, IKey("b", 2));
  EXPECT_EQ(between_outputs_calls_, 1);
}

TEST_F(CompactionJobTest, MakeEditDeletesTheInputsAndAddsTheOutputsOneLevelDown) {
  // A compaction of level 1, so that the levels in the edit are not the ones a test of level 0
  // would also pass with.
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario(
      {{}, {{2, {{IKey("a", 5), "new"}, {IKey("b", 5), "new"}}}}, {{3, {{IKey("a", 1), "old"}}}}},
      1, &scenario));
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 1 << 20);
  ASSERT_OK(job->Run());
  ASSERT_EQ(job->outputs().size(), 1u);

  const VersionEdit edit = job->MakeEdit();
  EXPECT_EQ(edit.deleted_files, (std::set<std::pair<int, uint64_t>>{{1, 2}, {2, 3}}));
  ASSERT_EQ(edit.new_files.size(), 1u);
  EXPECT_EQ(edit.new_files[0].first, 2);
  EXPECT_EQ(edit.new_files[0].second, job->outputs()[0]);
  ASSERT_EQ(edit.compact_pointers.size(), 1u);
  EXPECT_EQ(edit.compact_pointers[0].first, 1);
  EXPECT_EQ(edit.compact_pointers[0].second, scenario.plan.next_compact_pointer);

  // The edit applies to the state the plan came from.
  VersionState next;
  ASSERT_OK(ApplyEdit(scenario.state, edit, &next));
  EXPECT_TRUE(next.files[1].empty());
  ASSERT_EQ(next.files[2].size(), 1u);
  EXPECT_EQ(next.files[2][0], job->outputs()[0]);
  EXPECT_EQ(next.compact_pointers[1], scenario.plan.next_compact_pointer);
}

TEST_F(CompactionJobTest, SplitsAtTargetSizeOnUserKeyBoundaries) {
  Input input;
  input.number = 2;
  for (int i = 0; i < 1000; i++) {
    input.entries.emplace_back(IKey(UserKey(i), static_cast<SequenceNumber>(i) + 1), Value(200));
  }
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{input}}, 0, &scenario));
  constexpr uint64_t kTarget = 16384;
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, kTarget);
  ASSERT_OK(job->Run());

  const std::vector<FileMetaData>& outputs = job->outputs();
  EXPECT_GE(outputs.size(), 10u);
  EXPECT_EQ(ReadOutputs(*job), input.entries) << "the outputs together are the input, in order";
  for (size_t i = 0; i < outputs.size(); i++) {
    if (i + 1 < outputs.size()) {
      EXPECT_GE(outputs[i].file_size, kTarget) << "output " << i << " was closed early";
      EXPECT_LT(CompareInternalKey(outputs[i].largest, outputs[i + 1].smallest), 0);
      EXPECT_LT(BytewiseCompare(ExtractUserKey(outputs[i].largest),
                                ExtractUserKey(outputs[i + 1].smallest)),
                0)
          << "outputs " << i << " and " << i + 1 << " share a user key";
    }
    EXPECT_LT(outputs[i].file_size, 2 * kTarget) << "output " << i << " is far over the target";
  }
  EXPECT_EQ(between_outputs_calls_, static_cast<int>(outputs.size()));
  uint64_t written = 0;
  for (const FileMetaData& meta : outputs) {
    written += meta.file_size;
    EXPECT_TRUE(env_.FileExists(TableFileName(kDb, meta.number)));
  }
  EXPECT_EQ(job->stats().bytes_written, written);
}

TEST_F(CompactionJobTest, KeepsAllVersionsOfOneKeyInOneOutput) {
  // 200 versions of "k", a kilobyte each, every one kept because a snapshot sits on each. They are
  // twelve times the target, and the one place the job must not cut is between them.
  Input input;
  input.number = 2;
  input.entries.emplace_back(IKey("a", 300), Value(1024));
  for (SequenceNumber seq = 200; seq >= 1; seq--) {
    input.entries.emplace_back(IKey("k", seq), Value(1024, 'k'));
  }
  input.entries.emplace_back(IKey("z", 301), Value(1024));
  std::vector<SequenceNumber> snapshots;
  for (SequenceNumber seq = 1; seq <= 200; seq++) {
    snapshots.push_back(seq);
  }
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{input}}, 0, &scenario));
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 16384, snapshots);
  ASSERT_OK(job->Run());

  EXPECT_EQ(ReadOutputs(*job), input.entries) << "every version is kept";
  int outputs_with_k = 0;
  for (const FileMetaData& meta : job->outputs()) {
    const bool holds_k = BytewiseCompare(ExtractUserKey(meta.smallest), "k") <= 0 &&
                         BytewiseCompare(ExtractUserKey(meta.largest), "k") >= 0;
    if (holds_k) {
      outputs_with_k++;
      EXPECT_GT(meta.file_size, 200u * 1024) << "the output with k should hold all of it";
    }
  }
  EXPECT_EQ(outputs_with_k, 1);
  EXPECT_GE(job->outputs().size(), 2u) << "the neighbours of k should be in other outputs";
}

TEST_F(CompactionJobTest, GrandparentOverlapClosesAnOutput) {
  // 100 small keys, which are one output of their own accord. The files two levels down that they
  // overlap are what cut it: each is three targets in size, ten targets is the limit, and the
  // fourth that the keys pass takes the total over it.
  constexpr uint64_t kTarget = 16384;
  Input input;
  input.number = 2;
  for (int i = 0; i < 100; i++) {
    input.entries.emplace_back(IKey(UserKey(i), static_cast<SequenceNumber>(i) + 1), Value(100));
  }
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{input}}, 0, &scenario));
  // Files that exist only as descriptions: the job never opens a grandparent, it only measures it.
  for (int i = 0; i < 10; i++) {
    FileMetaData gp;
    gp.number = static_cast<uint64_t>(500 + i);
    gp.file_size = 3 * kTarget;
    gp.smallest = IKey(UserKey(i * 10), kMaxSequenceNumber);
    gp.largest = IKey(UserKey(i * 10 + 5), 1);
    scenario.plan.grandparents.push_back(gp);
  }
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, kTarget);
  ASSERT_OK(job->Run());

  EXPECT_EQ(ReadOutputs(*job), input.entries);
  const std::vector<FileMetaData>& outputs = job->outputs();
  ASSERT_EQ(outputs.size(), 3u);
  EXPECT_EQ(ExtractUserKey(outputs[0].largest), UserKey(35));
  EXPECT_EQ(ExtractUserKey(outputs[1].smallest), UserKey(36));
  EXPECT_EQ(ExtractUserKey(outputs[1].largest), UserKey(75));
  EXPECT_EQ(ExtractUserKey(outputs[2].smallest), UserKey(76));
}

TEST_F(CompactionJobTest, TombstoneIsDroppedAtTheBaseAndNothingIsWritten) {
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario(
      {{{3, {{IKey("a", 5, ValueType::kDeletion), ""}}}}, {{2, {{IKey("a", 1), "old"}}}}}, 0,
      &scenario));
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 1 << 20);
  ASSERT_OK(job->Run());
  EXPECT_TRUE(job->outputs().empty()) << "everything was dropped, so no file should be made";
  EXPECT_EQ(job->stats().tombstones_dropped, 1u);
  EXPECT_EQ(job->stats().shadowed_dropped, 1u);
  EXPECT_EQ(job->stats().entries_kept, 0u);
  EXPECT_EQ(job->stats().bytes_written, 0u);
  EXPECT_EQ(between_outputs_calls_, 0);
  const VersionEdit edit = job->MakeEdit();
  EXPECT_TRUE(edit.new_files.empty());
  EXPECT_EQ(edit.deleted_files, (std::set<std::pair<int, uint64_t>>{{0, 3}, {1, 2}}));
  EXPECT_EQ(TableFiles(), (std::set<std::string>{"000002.sst", "000003.sst"}));
}

TEST_F(CompactionJobTest, TombstoneIsKeptWhenALevelBelowTheOutputHoldsTheKey) {
  // The output goes to level 1, and level 2 has an older "a": dropping the deletion would bring
  // that value back.
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{{3, {{IKey("a", 5, ValueType::kDeletion), ""}}}},
                                        {{2, {{IKey("a", 3), "mid"}}}},
                                        {{4, {{IKey("a", 1), "oldest"}}}}},
                                       0, &scenario));
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 1 << 20);
  ASSERT_OK(job->Run());
  EXPECT_EQ(ReadOutputs(*job), (Entries{{IKey("a", 5, ValueType::kDeletion), ""}}));
  EXPECT_EQ(job->stats().tombstones_dropped, 0u);
  EXPECT_EQ(job->stats().shadowed_dropped, 1u);
}

TEST_F(CompactionJobTest, SnapshotKeepsTheVersionItCanSee) {
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(
      MakeScenario({{{3, {{IKey("a", 3), "new"}}}}, {{2, {{IKey("a", 1), "old"}}}}}, 0, &scenario));
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 1 << 20, {2});
  ASSERT_OK(job->Run());
  EXPECT_EQ(ReadOutputs(*job), (Entries{{IKey("a", 3), "new"}, {IKey("a", 1), "old"}}));
  EXPECT_EQ(job->stats().shadowed_dropped, 0u);
}

TEST_F(CompactionJobTest, EqualInternalKeysInTwoLevel0TablesAreWrittenOnce) {
  // A lenient open that crashed between writing its table and recording it leaves the same writes
  // in a second table. The second copy has the key and the sequence number of the first, so it is
  // in the same stripe and the filter drops it, and the table builder, which aborts on a key that
  // is not greater than the one before, never sees both.
  const std::vector<std::vector<SequenceNumber>> snapshot_sets = {{}, {3}, {5}, {5, 5}, {1, 5, 9}};
  const Entries entries = {{IKey("a", 5), "x"}, {IKey("a", 2), "w"}, {IKey("b", 6), "y"}};
  uint64_t input_number = 2;
  for (const std::vector<SequenceNumber>& snapshots : snapshot_sets) {
    SCOPED_TRACE(snapshots.size());
    Scenario scenario;
    ASSERT_NO_FATAL_FAILURE(
        MakeScenario({{{input_number, entries}, {input_number + 1, entries}}}, 0, &scenario));
    input_number += 2;
    const std::unique_ptr<CompactionJob> job = NewJob(scenario, 1 << 20, snapshots);
    ASSERT_OK(job->Run());
    // Whatever the snapshots keep, each internal key is there once.
    std::set<std::string> keys;
    for (const auto& [key, value] : ReadOutputs(*job)) {
      EXPECT_TRUE(keys.insert(key).second) << "an internal key was written twice";
    }
    EXPECT_TRUE(keys.count(IKey("a", 5)) == 1 && keys.count(IKey("b", 6)) == 1);
    EXPECT_GE(job->stats().shadowed_dropped, 3u);
  }
}

TEST_F(CompactionJobTest, ReadingAnInputThatIsCorruptFailsAndNamesTheTable) {
  Input input;
  input.number = 2;
  for (int i = 0; i < 50; i++) {
    input.entries.emplace_back(IKey(UserKey(i), static_cast<SequenceNumber>(i) + 1), Value(100));
  }
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{input}}, 0, &scenario));
  // One bit flipped in the first data block, in the file that the job's cold cache will open.
  const std::string path = TableFileName(kDb, 2);
  std::string contents = env_.FileContents(path);
  ASSERT_GT(contents.size(), 100u);
  contents[40] = static_cast<char>(contents[40] ^ 0x04);
  env_.SetFileContents(path, contents);

  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 1 << 20);
  const Status s = job->Run();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_NE(s.ToString().find(path), std::string::npos)
      << "the error should name the table: " << s.ToString();
  EXPECT_TRUE(job->outputs().empty());
  EXPECT_EQ(TableFiles(), (std::set<std::string>{"000002.sst"})) << "an output was left behind";
}

TEST_F(CompactionJobTest, AnInputThatCannotBeOpenedIsNamedOnce) {
  // The cache that opens a table names it when the open fails, and the job names a table that the
  // cache does not: a damaged footer must come out with the path once, and not twice.
  Input input;
  input.number = 2;
  for (int i = 0; i < 20; i++) {
    input.entries.emplace_back(IKey(UserKey(i), static_cast<SequenceNumber>(i) + 1), Value(100));
  }
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{input}}, 0, &scenario));
  const std::string path = TableFileName(kDb, 2);
  std::string contents = env_.FileContents(path);
  contents.back() = static_cast<char>(contents.back() ^ 0x01);  // the magic number of the footer
  env_.SetFileContents(path, contents);

  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 1 << 20);
  const Status s = job->Run();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  const std::string text = s.ToString();
  const size_t first = text.find(path);
  ASSERT_NE(first, std::string::npos) << text;
  EXPECT_EQ(text.find(path, first + 1), std::string::npos) << "the table is named twice: " << text;
}

TEST_F(CompactionJobTest, AFileNumberAtTheCeilingFailsTheJobBeforeAFileIsMade) {
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{{2, {{IKey("a", 1), "x"}}}}}, 0, &scenario));
  CompactionJobContext ctx = Context(1 << 20);
  int calls = 0;
  ctx.allocate_file_number = [&calls] {
    calls++;
    return kMaxFileNumber;
  };
  CompactionJob job(std::move(ctx), scenario.plan,
                    std::make_shared<const Version>(scenario.state, &job_cache_), {});
  const Status s = job.Run();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(TableFiles(), (std::set<std::string>{"000002.sst"}));
}

TEST_F(CompactionJobTest, FailedWriteRemovesOutputs) {
  Input input;
  input.number = 2;
  for (int i = 0; i < 1000; i++) {
    input.entries.emplace_back(IKey(UserKey(i), static_cast<SequenceNumber>(i) + 1), Value(200));
  }
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{input}}, 0, &scenario));
  // The first output is synced, and the second is the one that fails: the first has to go too.
  env_.FailOpAfter(Op::kSync, 1);
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 16384);
  const Status s = job->Run();
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(env_.injected_errors(), 1u);
  EXPECT_TRUE(job->outputs().empty());
  EXPECT_EQ(TableFiles(), (std::set<std::string>{"000002.sst"})) << "an output was left behind";
}

TEST_F(CompactionJobTest, AFailureAnywhereLeavesNoOutputAndNoOpenFile) {
  // Every call that a job makes through the Env fails in turn. A builder that is dropped without
  // Abandon() ends the process, a file that is not closed leaks, and a partial output that is not
  // removed would stay for ever: none of it may happen on any path.
  const std::vector<Op> ops = {Op::kNewWritable, Op::kAppend,  Op::kFlush,           Op::kSync,
                               Op::kClose,       Op::kSyncDir, Op::kNewRandomAccess, Op::kRead,
                               Op::kRemove};
  Input input;
  input.number = 2;
  for (int i = 0; i < 300; i++) {
    input.entries.emplace_back(IKey(UserKey(i), static_cast<SequenceNumber>(i) + 1), Value(200));
  }
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{input}}, 0, &scenario));
  int failed = 0;
  for (const Op op : ops) {
    for (int n = 0; n < 40; n++) {
      env_.FailOpAfter(op, n);
      const uint64_t before = env_.injected_errors();
      TableCache cold_cache(kDb, &env_, &block_cache_, 100);
      CompactionJobContext ctx = Context(16384);
      ctx.table_cache = &cold_cache;
      CompactionJob job(std::move(ctx), scenario.plan,
                        std::make_shared<const Version>(scenario.state, &cold_cache), {});
      const Status s = job.Run();
      const bool fired = env_.injected_errors() > before;
      env_.ClearFaults();
      if (fired) {
        failed++;
        EXPECT_FALSE(s.ok()) << "an injected failure of call " << n << " went unnoticed";
        EXPECT_TRUE(job.outputs().empty());
        EXPECT_EQ(TableFiles(), (std::set<std::string>{"000002.sst"}))
            << "an output survived the failure of call " << n;
      } else {
        // A job makes fewer calls of this kind than n + 1: they have all been tried. (A remove is
        // only made after a failure, so there is nothing to fail in a run that succeeds.)
        EXPECT_OK(s);
        for (const FileMetaData& meta : job.outputs()) {
          EXPECT_TRUE(env_.FileExists(TableFileName(kDb, meta.number)));
        }
        job.RemoveOutputs();
        EXPECT_EQ(TableFiles(), (std::set<std::string>{"000002.sst"}));
        break;
      }
    }
  }
  EXPECT_GT(failed, 40) << "the sweep hardly made a job fail";
}

TEST_F(CompactionJobTest, RemoveOutputsDeletesTheFilesAndForgetsThem) {
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{{2, {{IKey("a", 1), "x"}}}}}, 0, &scenario));
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 1 << 20);
  ASSERT_OK(job->Run());
  ASSERT_EQ(job->outputs().size(), 1u);
  const std::string path = TableFileName(kDb, job->outputs()[0].number);
  ASSERT_TRUE(env_.FileExists(path));
  job->RemoveOutputs();
  EXPECT_FALSE(env_.FileExists(path));
  EXPECT_TRUE(job->outputs().empty());
  EXPECT_TRUE(job->MakeEdit().new_files.empty());
}

TEST_F(CompactionJobTest, BetweenOutputsRunsAfterEachOutputIsDurableAndNamed) {
  Input input;
  input.number = 2;
  for (int i = 0; i < 200; i++) {
    input.entries.emplace_back(IKey(UserKey(i), static_cast<SequenceNumber>(i) + 1), Value(200));
  }
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{input}}, 0, &scenario));
  CompactionJobContext ctx = Context(16384);
  std::vector<uint64_t> allocated;
  ctx.allocate_file_number = [this, &allocated] {
    allocated.push_back(next_number_);
    return next_number_++;
  };
  int calls = 0;
  int wrong = 0;
  ctx.between_outputs = [&] {
    // The output that was just finished is there, and the next has not been begun.
    calls++;
    if (allocated.size() != static_cast<size_t>(calls) ||
        !env_.FileExists(TableFileName(kDb, allocated.back()))) {
      wrong++;
    }
  };
  CompactionJob job(std::move(ctx), scenario.plan,
                    std::make_shared<const Version>(scenario.state, &job_cache_), {});
  ASSERT_OK(job.Run());
  EXPECT_GE(job.outputs().size(), 2u);
  EXPECT_EQ(calls, static_cast<int>(job.outputs().size()));
  EXPECT_EQ(wrong, 0);
}

using CompactionJobDeathTest = CompactionJobTest;

TEST_F(CompactionJobDeathTest, RunningAJobTwiceAborts) {
  Scenario scenario;
  ASSERT_NO_FATAL_FAILURE(MakeScenario({{{2, {{IKey("a", 1), "x"}}}}}, 0, &scenario));
  const std::unique_ptr<CompactionJob> job = NewJob(scenario, 1 << 20);
  ASSERT_OK(job->Run());
  EXPECT_DEATH(static_cast<void>(job->Run()), "called once");
}

}  // namespace
}  // namespace shale
