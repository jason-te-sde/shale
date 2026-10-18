// What only the fault-injecting Env promises. The behaviour every Env shares is in
// env_contract_test, which runs against this one too, with no fault configured.

#include "testkit/fault_env.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "io/env_util.h"
#include "shale/env.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/random.h"

namespace shale {
namespace {

using Op = FaultInjectionEnv::Op;

// Returns from the enclosing function with the Status of `expr` if it is not OK.
#define RETURN_IF_ERROR(expr)      \
  do {                             \
    const Status status_ = (expr); \
    if (!status_.ok()) {           \
      return status_;              \
    }                              \
  } while (false)

// Creates `path`, appends `synced` and syncs it, appends `unsynced` and does not, and closes the
// handle. Close does not make anything durable, so `unsynced` is exactly what a power loss may
// drop. The directory entry is not made durable either.
Status Put(Env* env, const std::string& path, std::string_view synced, std::string_view unsynced) {
  std::unique_ptr<WritableFile> file;
  RETURN_IF_ERROR(env->NewWritableFile(path, &file));
  RETURN_IF_ERROR(file->Append(synced));
  RETURN_IF_ERROR(file->Sync());
  RETURN_IF_ERROR(file->Append(unsynced));
  return file->Close();
}

// True if `s` is an I/O error that the fault Env injected and that names `path`.
::testing::AssertionResult IsInjected(const Status& s, const std::string& path) {
  if (!s.IsIOError()) {
    return ::testing::AssertionFailure() << "not an IOError: " << s.ToString();
  }
  const std::string text = s.ToString();
  if (text.find("injected") == std::string::npos) {
    return ::testing::AssertionFailure() << "does not say injected: " << text;
  }
  if (text.find(path) == std::string::npos) {
    return ::testing::AssertionFailure() << "does not name " << path << ": " << text;
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult IsInvalidated(const Status& s) {
  if (!s.IsIOError()) {
    return ::testing::AssertionFailure() << "not an IOError: " << s.ToString();
  }
  if (s.ToString().find("invalidated by crash") == std::string::npos) {
    return ::testing::AssertionFailure() << "does not say invalidated by crash: " << s.ToString();
  }
  return ::testing::AssertionSuccess();
}

class FaultEnvTest : public testing::Test {
 protected:
  FaultEnvTest() : env_(/*seed=*/1) {
    const Status s = env_.CreateDir("/d");
    EXPECT_OK(s);
  }

  FaultInjectionEnv env_;
};

// --- Power loss and process crash ---------------------------------------------------------------

TEST_F(FaultEnvTest, DropUnsyncedKeepsOnlySyncedBytes) {
  ASSERT_OK(Put(&env_, "/d/f", "abc", "def"));
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLossDropUnsynced();
  ASSERT_TRUE(env_.FileExists("/d/f"));
  EXPECT_EQ(env_.FileContents("/d/f"), "abc");
}

TEST_F(FaultEnvTest, PowerLossKeepsAPrefixOfUnsynced) {
  std::set<size_t> lengths;
  for (uint64_t seed = 0; seed < 200; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FaultInjectionEnv env(seed);
    ASSERT_OK(env.CreateDir("/d"));
    ASSERT_OK(Put(&env, "/d/f", "abc", "def"));
    ASSERT_OK(env.SyncDir("/d"));
    env.SimulatePowerLoss();
    const std::string contents = env.FileContents("/d/f");
    ASSERT_GE(contents.size(), 3u) << "synced bytes must survive";
    ASSERT_EQ(std::string_view("abcdef").substr(0, contents.size()), contents)
        << "what survives must be a prefix of what was written";
    lengths.insert(contents.size());
  }
  EXPECT_EQ(lengths.count(3), 1u) << "no unsynced byte surviving must be possible";
  EXPECT_EQ(lengths.count(6), 1u) << "every unsynced byte surviving must be possible";
  EXPECT_TRUE(lengths.count(4) == 1 || lengths.count(5) == 1)
      << "a part of the unsynced bytes surviving must be possible";
}

TEST_F(FaultEnvTest, ProcessCrashKeepsEverything) {
  // Everything the operating system has: every byte that was flushed to it, synced or not, and
  // every directory change. What still waits in a handle is the next test.
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  ASSERT_OK(writer->Flush());
  ASSERT_OK(writer->Append("def"));
  ASSERT_OK(writer->Flush());
  env_.SimulateProcessCrash();
  EXPECT_TRUE(env_.FileExists("/d/f")) << "the directory entry was never synced, and survives";
  EXPECT_EQ(env_.FileContents("/d/f"), "abcdef");
}

TEST_F(FaultEnvTest, ProcessCrashDropsUnflushedBytes) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  ASSERT_OK(writer->Flush());
  ASSERT_OK(writer->Append("def"));  // Never flushed: it dies with the process.
  env_.SimulateProcessCrash();
  EXPECT_EQ(env_.FileContents("/d/f"), "abc");
}

TEST_F(FaultEnvTest, PowerLossDropsUnflushedBytes) {
  // A byte that never left the handle is not among those a power loss may keep, whatever the seed.
  // The flushed ones are, in a prefix, because none of them was synced.
  std::set<size_t> lengths;
  for (uint64_t seed = 0; seed < 100; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FaultInjectionEnv env(seed);
    ASSERT_OK(env.CreateDir("/d"));
    std::unique_ptr<WritableFile> writer;
    ASSERT_OK(env.NewWritableFile("/d/f", &writer));
    ASSERT_OK(writer->Append("abc"));
    ASSERT_OK(writer->Flush());
    ASSERT_OK(writer->Append("def"));
    ASSERT_OK(env.SyncDir("/d"));
    env.SimulatePowerLoss();
    const std::string contents = env.FileContents("/d/f");
    ASSERT_LE(contents.size(), 3u);
    ASSERT_EQ(std::string_view("abc").substr(0, contents.size()), contents);
    lengths.insert(contents.size());
  }
  EXPECT_EQ(lengths, (std::set<size_t>{0, 1, 2, 3}));
}

TEST_F(FaultEnvTest, AppendedBytesStayInTheHandleUntilTheyAreFlushed) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  std::unique_ptr<RandomAccessFile> reader;
  ASSERT_OK(env_.NewRandomAccessFile("/d/f", &reader));
  ASSERT_OK(writer->Append("abc"));
  uint64_t size = 99;
  ASSERT_OK(env_.GetFileSize("/d/f", &size));
  EXPECT_EQ(size, 0u);
  EXPECT_EQ(env_.FileContents("/d/f"), "");
  char scratch[10];
  std::string_view result = "not touched";
  ASSERT_OK(reader->Read(0, 10, &result, scratch));
  EXPECT_TRUE(result.empty()) << "a reader sees only what was flushed";
  ASSERT_OK(writer->Flush());
  ASSERT_OK(env_.GetFileSize("/d/f", &size));
  EXPECT_EQ(size, 3u);
  ASSERT_OK(reader->Read(0, 10, &result, scratch));
  EXPECT_EQ(result, "abc");
  ASSERT_OK(writer->Close());
}

TEST_F(FaultEnvTest, SyncAndCloseHandTheBufferOver) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  ASSERT_OK(writer->Sync());
  EXPECT_EQ(env_.FileContents("/d/f"), "abc");
  ASSERT_OK(writer->Append("def"));
  ASSERT_OK(writer->Close());
  EXPECT_EQ(env_.FileContents("/d/f"), "abcdef");
}

TEST_F(FaultEnvTest, AHandleDroppedWithoutCloseLosesItsUnflushedBytes) {
  {
    std::unique_ptr<WritableFile> writer;
    ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
    ASSERT_OK(writer->Append("abc"));
    ASSERT_OK(writer->Flush());
    ASSERT_OK(writer->Append("def"));
  }
  EXPECT_TRUE(env_.FileExists("/d/f"));
  EXPECT_EQ(env_.FileContents("/d/f"), "abc");
}

TEST_F(FaultEnvTest, AProcessCrashDoesNotMakeAnythingDurable) {
  // The machine survived, so what was lost to the next power loss is still at risk.
  ASSERT_OK(Put(&env_, "/d/f", "abc", "def"));
  env_.SimulateProcessCrash();
  ASSERT_EQ(env_.FileContents("/d/f"), "abcdef");
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_FALSE(env_.FileExists("/d/f")) << "the entry was never synced";
}

TEST_F(FaultEnvTest, WhatSurvivesAPowerLossIsDurable) {
  // After the machine is back, the bytes it kept are on the disk: losing power again drops none of
  // them, however the first loss turned out.
  for (uint64_t seed = 0; seed < 50; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FaultInjectionEnv env(seed);
    ASSERT_OK(env.CreateDir("/d"));
    ASSERT_OK(Put(&env, "/d/f", "abc", "def"));
    ASSERT_OK(env.SyncDir("/d"));
    env.SimulatePowerLoss();
    const std::string after_first = env.FileContents("/d/f");
    env.SimulatePowerLossDropUnsynced();
    EXPECT_EQ(env.FileContents("/d/f"), after_first);
  }
}

TEST_F(FaultEnvTest, UnsyncedCreationVanishes) {
  ASSERT_OK(Put(&env_, "/d/f", "abc", ""));  // The bytes are synced; the directory entry is not.
  ASSERT_TRUE(env_.FileExists("/d/f"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_FALSE(env_.FileExists("/d/f"));
  std::vector<std::string> children;
  ASSERT_OK(env_.GetChildren("/d", &children));
  EXPECT_TRUE(children.empty());
}

TEST_F(FaultEnvTest, SyncedCreationSurvives) {
  ASSERT_OK(Put(&env_, "/d/f", "abc", ""));
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLoss();
  ASSERT_TRUE(env_.FileExists("/d/f"));
  EXPECT_EQ(env_.FileContents("/d/f"), "abc");
}

TEST_F(FaultEnvTest, UnsyncedRemovalIsUndone) {
  env_.SetFileContents("/d/x", "1");
  ASSERT_OK(env_.RemoveFile("/d/x"));
  ASSERT_FALSE(env_.FileExists("/d/x"));
  env_.SimulatePowerLossDropUnsynced();
  ASSERT_TRUE(env_.FileExists("/d/x"));
  EXPECT_EQ(env_.FileContents("/d/x"), "1");
}

TEST_F(FaultEnvTest, SyncedRemovalStays) {
  env_.SetFileContents("/d/x", "1");
  ASSERT_OK(env_.RemoveFile("/d/x"));
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLoss();
  EXPECT_FALSE(env_.FileExists("/d/x"));
}

TEST_F(FaultEnvTest, UnsyncedRenameReverts) {
  env_.SetFileContents("/d/a", "1");
  env_.SetFileContents("/d/b", "2");
  ASSERT_OK(env_.RenameFile("/d/a", "/d/b"));
  ASSERT_EQ(env_.FileContents("/d/b"), "1");
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/a"), "1");
  EXPECT_EQ(env_.FileContents("/d/b"), "2");
}

TEST_F(FaultEnvTest, SyncedRenameSticks) {
  env_.SetFileContents("/d/a", "1");
  env_.SetFileContents("/d/b", "2");
  ASSERT_OK(env_.RenameFile("/d/a", "/d/b"));
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLoss();
  EXPECT_EQ(env_.FileContents("/d/b"), "1");
  EXPECT_FALSE(env_.FileExists("/d/a"));
}

TEST_F(FaultEnvTest, SyncDirCoversOnlyTheEntriesOfThatDirectory) {
  // The parent of a path is everything before its last slash.
  ASSERT_OK(env_.CreateDir("/d/sub"));
  ASSERT_OK(env_.CreateDir("/e"));
  ASSERT_OK(Put(&env_, "/d/f", "1", ""));
  ASSERT_OK(Put(&env_, "/d/sub/g", "2", ""));
  ASSERT_OK(Put(&env_, "/e/h", "3", ""));
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_TRUE(env_.FileExists("/d/f"));
  EXPECT_FALSE(env_.FileExists("/d/sub/g")) << "a subdirectory is a different directory";
  EXPECT_FALSE(env_.FileExists("/e/h"));
}

TEST_F(FaultEnvTest, SyncDirOfARootFileNamesTheRoot) {
  ASSERT_OK(env_.CreateDir("/"));
  ASSERT_OK(Put(&env_, "/f", "1", ""));
  ASSERT_OK(env_.SyncDir("/"));
  env_.SimulatePowerLoss();
  EXPECT_TRUE(env_.FileExists("/f"));
}

TEST_F(FaultEnvTest, SyncDirIgnoresATrailingSlash) {
  ASSERT_OK(Put(&env_, "/d/f", "1", ""));
  ASSERT_OK(env_.SyncDir("/d/"));
  env_.SimulatePowerLoss();
  EXPECT_TRUE(env_.FileExists("/d/f"));
}

TEST_F(FaultEnvTest, ARenameAcrossDirectoriesIsDurableOnlyWhereItIsSynced) {
  ASSERT_OK(env_.CreateDir("/e"));
  env_.SetFileContents("/d/x", "1");
  ASSERT_OK(env_.RenameFile("/d/x", "/e/x"));
  ASSERT_OK(env_.SyncDir("/e"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/e/x"), "1");
  EXPECT_EQ(env_.FileContents("/d/x"), "1")
      << "the removal from /d was not synced, so the file is in both: a rename is not atomic "
         "across directories";
}

TEST_F(FaultEnvTest, ARenameAcrossDirectoriesSyncedOnTheSourceSideOnlyLosesTheFile) {
  // The removal from /d is durable and the creation in /e is not: a caller that syncs the wrong
  // directory, or the right ones in the wrong order, finds no file under either name.
  ASSERT_OK(env_.CreateDir("/e"));
  env_.SetFileContents("/d/x", "1");
  ASSERT_OK(env_.RenameFile("/d/x", "/e/x"));
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_FALSE(env_.FileExists("/d/x"));
  EXPECT_FALSE(env_.FileExists("/e/x"));
}

TEST_F(FaultEnvTest, ARewrittenFileKeepsOnlyTheNewSyncedBytesOnceTheDirectoryIsSynced) {
  // Opening a file for write replaces its contents with an empty file. That is a change of the
  // directory like any other, and it is durable once the directory is synced.
  env_.SetFileContents("/d/f", "the old contents");
  ASSERT_OK(Put(&env_, "/d/f", "new", "er"));
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/f"), "new");
}

TEST_F(FaultEnvTest, ATruncationThatWasNotSyncedBringsTheOldBytesBack) {
  env_.SetFileContents("/d/f", "the old contents");
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("er"));
  ASSERT_OK(writer->Close());
  ASSERT_EQ(env_.FileContents("/d/f"), "er");
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/f"), "the old contents");
}

TEST_F(FaultEnvTest, ATruncationIsKeptOrNotAsAUnitWithTheOtherChangesOfItsDirectory) {
  std::set<std::string> outcomes;
  for (uint64_t seed = 0; seed < 100; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FaultInjectionEnv env(seed);
    ASSERT_OK(env.CreateDir("/d"));
    env.SetFileContents("/d/f", "old");
    ASSERT_OK(Put(&env, "/d/f", "new", ""));  // Synced bytes, so either "old" or "new" remains.
    env.SimulatePowerLoss();
    outcomes.insert(env.FileContents("/d/f"));
  }
  EXPECT_EQ(outcomes, (std::set<std::string>{"old", "new"}));
}

TEST_F(FaultEnvTest, ARecreatedFileComesBackAsTheOldOneIfTheRemovalWasNotSynced) {
  env_.SetFileContents("/d/f", "old");
  ASSERT_OK(env_.RemoveFile("/d/f"));
  ASSERT_OK(Put(&env_, "/d/f", "new", ""));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/f"), "old");
}

// --- Namespace changes persist as a prefix ------------------------------------------------------

TEST_F(FaultEnvTest, UnsyncedNamespaceChangesSurviveAsAnInOrderPrefix) {
  // Two changes to /d with no SyncDir: the creation of a, then the removal of b. A power loss keeps
  // none of them, the first, or both, and never the second without the first.
  std::set<std::pair<bool, bool>> outcomes;  // (a exists, b exists)
  for (uint64_t seed = 0; seed < 200; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FaultInjectionEnv env(seed);
    ASSERT_OK(env.CreateDir("/d"));
    env.SetFileContents("/d/b", "1");
    ASSERT_OK(Put(&env, "/d/a", "x", ""));
    ASSERT_OK(env.RemoveFile("/d/b"));
    env.SimulatePowerLoss();
    const bool a = env.FileExists("/d/a");
    const bool b = env.FileExists("/d/b");
    ASSERT_TRUE(a || b) << "the removal of b survived without the creation of a before it";
    outcomes.emplace(a, b);
  }
  EXPECT_EQ(outcomes,
            (std::set<std::pair<bool, bool>>{{false, true}, {true, true}, {true, false}}));
}

TEST_F(FaultEnvTest, TheDropUnsyncedLossAppliesNoNamespaceChangeAtAll) {
  env_.SetFileContents("/d/b", "1");
  ASSERT_OK(Put(&env_, "/d/a", "x", ""));
  ASSERT_OK(env_.RemoveFile("/d/b"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_FALSE(env_.FileExists("/d/a"));
  EXPECT_TRUE(env_.FileExists("/d/b"));
}

TEST_F(FaultEnvTest, SyncDirMakesEveryEarlierChangeOfThatDirectoryDurable) {
  for (uint64_t seed = 0; seed < 50; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FaultInjectionEnv env(seed);
    ASSERT_OK(env.CreateDir("/d"));
    env.SetFileContents("/d/b", "1");
    ASSERT_OK(Put(&env, "/d/a", "x", ""));
    ASSERT_OK(env.RemoveFile("/d/b"));
    ASSERT_OK(env.SyncDir("/d"));
    env.SimulatePowerLoss();
    EXPECT_TRUE(env.FileExists("/d/a"));
    EXPECT_FALSE(env.FileExists("/d/b"));
  }
}

TEST_F(FaultEnvTest, ALogStartsAgainWhenItsDirectoryIsSynced) {
  // After a SyncDir the changes waiting in the directory are the ones made since, and only them:
  // with one such change left, a power loss keeps it about half the time. A log that still held
  // the synced changes would dilute that.
  int kept = 0;
  for (uint64_t seed = 0; seed < 200; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FaultInjectionEnv env(seed);
    ASSERT_OK(env.CreateDir("/d"));
    for (const char* name : {"/d/a", "/d/b", "/d/c", "/d/e"}) {
      ASSERT_OK(Put(&env, name, "x", ""));
    }
    ASSERT_OK(env.SyncDir("/d"));
    ASSERT_OK(Put(&env, "/d/late", "y", ""));
    env.SimulatePowerLoss();
    kept += env.FileExists("/d/late") ? 1 : 0;
  }
  EXPECT_GT(kept, 70);  // 100 expected, and the spread is about 7.
  EXPECT_LT(kept, 130);
}

TEST_F(FaultEnvTest, EachDirectoryKeepsItsOwnPrefix) {
  std::set<std::pair<bool, bool>> outcomes;  // (/d/a exists, /e/b exists)
  for (uint64_t seed = 0; seed < 100; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FaultInjectionEnv env(seed);
    ASSERT_OK(env.CreateDir("/d"));
    ASSERT_OK(env.CreateDir("/e"));
    ASSERT_OK(Put(&env, "/d/a", "x", ""));
    ASSERT_OK(Put(&env, "/e/b", "y", ""));
    env.SimulatePowerLoss();
    outcomes.emplace(env.FileExists("/d/a"), env.FileExists("/e/b"));
  }
  EXPECT_EQ(outcomes.size(), 4u);
}

TEST_F(FaultEnvTest, ARenameInOneDirectoryIsKeptOrLostAsAWhole) {
  std::set<std::pair<bool, bool>> outcomes;  // (a exists, b exists)
  for (uint64_t seed = 0; seed < 100; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FaultInjectionEnv env(seed);
    ASSERT_OK(env.CreateDir("/d"));
    env.SetFileContents("/d/a", "1");
    ASSERT_OK(env.RenameFile("/d/a", "/d/b"));
    env.SimulatePowerLoss();
    const bool a = env.FileExists("/d/a");
    const bool b = env.FileExists("/d/b");
    ASSERT_NE(a, b) << "the file must be under exactly one of its names";
    EXPECT_EQ(env.FileContents(a ? "/d/a" : "/d/b"), "1");
    outcomes.emplace(a, b);
  }
  EXPECT_EQ(outcomes, (std::set<std::pair<bool, bool>>{{true, false}, {false, true}}));
}

TEST_F(FaultEnvTest, HandlesDieWithTheCrash) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  ASSERT_OK(writer->Flush());
  ASSERT_OK(writer->Append("buffered"));
  env_.SimulateProcessCrash();
  EXPECT_TRUE(IsInvalidated(writer->Append("def")));
  EXPECT_TRUE(IsInvalidated(writer->Flush()));
  EXPECT_TRUE(IsInvalidated(writer->Sync()));
  EXPECT_TRUE(IsInvalidated(writer->Close()));
  EXPECT_EQ(env_.FileContents("/d/f"), "abc")
      << "an invalidated handle writes nothing, and what it buffered is gone";
}

TEST_F(FaultEnvTest, EveryKindOfHandleDiesWithEveryKindOfCrash) {
  const std::vector<std::pair<std::string, std::function<void()>>> crashes = {
      {"power loss", [this] { env_.SimulatePowerLoss(); }},
      {"power loss, drop unsynced", [this] { env_.SimulatePowerLossDropUnsynced(); }},
      {"process crash", [this] { env_.SimulateProcessCrash(); }},
  };
  for (const auto& [name, crash] : crashes) {
    SCOPED_TRACE(name);
    env_.SetFileContents("/d/f", "hello world");
    std::unique_ptr<SequentialFile> sequential;
    std::unique_ptr<RandomAccessFile> random_access;
    std::unique_ptr<WritableFile> writer;
    ASSERT_OK(env_.NewSequentialFile("/d/f", &sequential));
    ASSERT_OK(env_.NewRandomAccessFile("/d/f", &random_access));
    ASSERT_OK(env_.NewWritableFile("/d/g", &writer));
    crash();
    char scratch[100];
    std::string_view result;
    EXPECT_TRUE(IsInvalidated(sequential->Read(5, &result, scratch)));
    EXPECT_TRUE(IsInvalidated(sequential->Skip(1)));
    EXPECT_TRUE(IsInvalidated(random_access->Read(0, 5, &result, scratch)));
    EXPECT_TRUE(IsInvalidated(writer->Append("x")));
    EXPECT_TRUE(IsInvalidated(writer->Flush()));
    EXPECT_TRUE(IsInvalidated(writer->Sync()));
    EXPECT_TRUE(IsInvalidated(writer->Close()));
  }
}

TEST_F(FaultEnvTest, HandlesOpenedAfterTheCrashWork) {
  env_.SetFileContents("/d/f", "hello");
  std::unique_ptr<WritableFile> old_writer;
  ASSERT_OK(env_.NewWritableFile("/d/g", &old_writer));
  env_.SimulateProcessCrash();

  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/g", &writer));
  ASSERT_OK(writer->Append("fresh"));
  ASSERT_OK(writer->Close());
  EXPECT_TRUE(IsInvalidated(old_writer->Close())) << "the old handle stays dead";
  std::string contents;
  ASSERT_OK(ReadFileToString(&env_, "/d/g", &contents));
  EXPECT_EQ(contents, "fresh");
}

TEST_F(FaultEnvTest, AClosedHandleStillAbortsOnUseAfterTheCrash) {
  // Using a handle after Close is a bug in the caller, crash or no crash.
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Close());
  env_.SimulateProcessCrash();
  EXPECT_DEATH(static_cast<void>(writer->Append("x")), "after Close");
}

TEST_F(FaultEnvTest, ACrashReleasesTheLocksOfTheDeadProcess) {
  std::unique_ptr<FileLock> lock;
  ASSERT_OK(env_.LockFile("/d/LOCK", &lock));
  env_.SimulateProcessCrash();

  std::unique_ptr<FileLock> next;
  ASSERT_OK(env_.LockFile("/d/LOCK", &next)) << "the restarted process must be able to lock";
  // The old lock object is still around. Dropping it must not release the new owner's lock.
  lock.reset();
  std::unique_ptr<FileLock> third;
  EXPECT_FALSE(env_.LockFile("/d/LOCK", &third).ok());
  ASSERT_OK(env_.UnlockFile(std::move(next)));
}

TEST_F(FaultEnvTest, UnlockingALockFromBeforeTheCrashReportsIt) {
  std::unique_ptr<FileLock> lock;
  ASSERT_OK(env_.LockFile("/d/LOCK", &lock));
  env_.SimulatePowerLoss();
  EXPECT_TRUE(IsInvalidated(env_.UnlockFile(std::move(lock))));
}

TEST_F(FaultEnvTest, HandlesMayOutliveTheEnv) {
  // As for the plain in-memory Env: a handle keeps what it needs, so destroying the Env first is
  // not a use after free.
  auto env = std::make_unique<FaultInjectionEnv>(/*seed=*/2);
  ASSERT_OK(env->CreateDir("/d"));
  env->SetFileContents("/d/f", "contents");
  std::unique_ptr<RandomAccessFile> reader;
  std::unique_ptr<WritableFile> writer;
  std::unique_ptr<FileLock> lock;
  ASSERT_OK(env->NewRandomAccessFile("/d/f", &reader));
  ASSERT_OK(env->NewWritableFile("/d/g", &writer));
  ASSERT_OK(env->LockFile("/d/LOCK", &lock));
  env.reset();

  char scratch[100];
  std::string_view result;
  ASSERT_OK(reader->Read(0, 100, &result, scratch));
  EXPECT_EQ(result, "contents");
  ASSERT_OK(writer->Append("still works"));
  ASSERT_OK(writer->Sync());
  ASSERT_OK(writer->Close());
  lock.reset();
}

TEST_F(FaultEnvTest, TheCrashCounterCountsEveryKind) {
  EXPECT_EQ(env_.crashes(), 0u);
  env_.SimulatePowerLoss();
  env_.SimulatePowerLossDropUnsynced();
  env_.SimulateProcessCrash();
  EXPECT_EQ(env_.crashes(), 3u);
}

TEST_F(FaultEnvTest, TheClockSurvivesACrash) {
  env_.SleepForMicroseconds(500);
  env_.SimulatePowerLoss();
  EXPECT_EQ(env_.NowMicros(), 500u);
  env_.SleepForMicroseconds(10);
  EXPECT_EQ(env_.NowMicros(), 510u);
}

TEST_F(FaultEnvTest, SetFileContentsIsDurableAtOnce) {
  env_.SetFileContents("/d/f", "abc");
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/f"), "abc") << "synced, and its entry durable, with no SyncDir";
}

TEST_F(FaultEnvTest, SetFileContentsInstallsAFreshFileAndLeavesOpenHandlesOnTheOldOne) {
  env_.SetFileContents("/d/f", "old contents");
  std::unique_ptr<RandomAccessFile> reader;
  ASSERT_OK(env_.NewRandomAccessFile("/d/f", &reader));
  env_.SetFileContents("/d/f", "new");
  char scratch[100];
  std::string_view result;
  ASSERT_OK(reader->Read(0, 100, &result, scratch));
  EXPECT_EQ(result, "old contents") << "the handle was opened on the file that was replaced";
  EXPECT_EQ(env_.FileContents("/d/f"), "new");
}

TEST_F(FaultEnvTest, SetFileContentsDoesNotChangeTheOtherDurableNameOfARenamedFile) {
  // a is renamed to b, and the rename is not synced: the durable view still has the file as a.
  // Setting the contents of b must not reach it.
  env_.SetFileContents("/d/a", "1");
  ASSERT_OK(env_.RenameFile("/d/a", "/d/b"));
  env_.SetFileContents("/d/b", "2");
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/a"), "1");
  EXPECT_EQ(env_.FileContents("/d/b"), "2");
}

TEST_F(FaultEnvTest, SetFileContentsSupersedesTheUnsyncedChangesToItsName) {
  for (uint64_t seed = 0; seed < 50; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    FaultInjectionEnv env(seed);
    ASSERT_OK(env.CreateDir("/d"));
    env.SetFileContents("/d/x", "1");
    ASSERT_OK(env.RemoveFile("/d/x"));
    env.SetFileContents("/d/x",
                        "2");  // Durable at once: the removal must not come back to undo it.
    env.SimulatePowerLoss();
    ASSERT_TRUE(env.FileExists("/d/x"));
    EXPECT_EQ(env.FileContents("/d/x"), "2");
  }
}

TEST_F(FaultEnvTest, FileContentsOfAMissingFileIsEmpty) {
  EXPECT_EQ(env_.FileContents("/d/missing"), "");
}

// --- Injected errors ----------------------------------------------------------------------------

TEST_F(FaultEnvTest, FailOpAfterFailsExactlyOnce) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("x"));
  env_.FailOpAfter(Op::kSync, 2);
  EXPECT_OK(writer->Sync());
  EXPECT_OK(writer->Sync());
  EXPECT_TRUE(IsInjected(writer->Sync(), "/d/f"));
  EXPECT_OK(writer->Sync());
  EXPECT_EQ(env_.injected_errors(), 1u);
  ASSERT_OK(writer->Close());
}

TEST_F(FaultEnvTest, FailOpAfterZeroFailsTheNextCall) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  env_.FailOpAfter(Op::kAppend, 0);
  EXPECT_TRUE(IsInjected(writer->Append("x"), "/d/f"));
  EXPECT_OK(writer->Append("y"));
  ASSERT_OK(writer->Close());
}

TEST_F(FaultEnvTest, AFailedAppendWritesNoBytes) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  env_.FailOpAfter(Op::kAppend, 0);
  EXPECT_FALSE(writer->Append("def").ok());
  ASSERT_OK(writer->Close());
  EXPECT_EQ(env_.FileContents("/d/f"), "abc");
}

TEST_F(FaultEnvTest, AFailedSyncMakesNothingDurable) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  env_.FailOpAfter(Op::kSync, 0);
  EXPECT_FALSE(writer->Sync().ok());
  ASSERT_OK(writer->Close());
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/f"), "") << "the bytes were never synced";
}

TEST_F(FaultEnvTest, ASyncThatSucceedsAfterAFailedOneMakesNothingDurable) {
  // Some operating systems report success from a second fsync of data that the first failed to
  // write and then dropped. The Env's documentation says such a Sync proves nothing, and here it
  // really does nothing: the bytes that were there when the first one failed are never synced.
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  env_.FailOpAfter(Op::kSync, 0);
  EXPECT_FALSE(writer->Sync().ok());
  EXPECT_OK(writer->Sync());
  ASSERT_OK(writer->Close());
  ASSERT_OK(env_.SyncDir("/d"));
  ASSERT_EQ(env_.FileContents("/d/f"), "abc") << "the bytes are there for readers";
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/f"), "");
}

TEST_F(FaultEnvTest, AFailedSyncKeepsWhatWasSyncedBeforeAndNothingAfter) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  ASSERT_OK(writer->Sync());
  ASSERT_OK(writer->Append("def"));
  env_.FailOpAfter(Op::kSync, 0);
  EXPECT_FALSE(writer->Sync().ok());
  ASSERT_OK(writer->Append("ghi"));
  EXPECT_OK(writer->Sync());
  ASSERT_OK(writer->Close());
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/f"), "abc");
}

TEST_F(FaultEnvTest, AFailedSyncOnlyFreezesItsOwnFile) {
  std::unique_ptr<WritableFile> failing;
  std::unique_ptr<WritableFile> other;
  ASSERT_OK(env_.NewWritableFile("/d/f", &failing));
  ASSERT_OK(env_.NewWritableFile("/d/g", &other));
  ASSERT_OK(failing->Append("abc"));
  ASSERT_OK(other->Append("xyz"));
  env_.FailOpAfter(Op::kSync, 0);
  EXPECT_FALSE(failing->Sync().ok());
  ASSERT_OK(other->Sync());
  ASSERT_OK(failing->Close());
  ASSERT_OK(other->Close());
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/f"), "");
  EXPECT_EQ(env_.FileContents("/d/g"), "xyz");
}

TEST_F(FaultEnvTest, ReopeningAFileAfterAFailedSyncStartsAFreshFile) {
  // Writing the bytes again to a new file is what the documentation tells a caller to do. A file
  // that is reopened for write is empty, so it is a new file in this respect too.
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  env_.FailOpAfter(Op::kSync, 0);
  EXPECT_FALSE(writer->Sync().ok());
  ASSERT_OK(writer->Close());

  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("new"));
  ASSERT_OK(writer->Sync());
  ASSERT_OK(writer->Close());
  ASSERT_OK(env_.SyncDir("/d"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_EQ(env_.FileContents("/d/f"), "new");
}

TEST_F(FaultEnvTest, AFailedFlushPushesNothing) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  env_.FailOpAfter(Op::kFlush, 0);
  EXPECT_TRUE(IsInjected(writer->Flush(), "/d/f"));
  EXPECT_EQ(env_.FileContents("/d/f"), "");
  uint64_t size = 99;
  ASSERT_OK(env_.GetFileSize("/d/f", &size));
  EXPECT_EQ(size, 0u);
  env_.SimulateProcessCrash();
  EXPECT_EQ(env_.FileContents("/d/f"), "") << "what the failed Flush did not push is lost";
}

TEST_F(FaultEnvTest, AFlushAfterAFailedOneStillHandsOverTheBytesThatWereWaiting) {
  // The Env's documentation says a caller must not go on after a failed Flush, and this Env does
  // not stop it, so that a test can see what the caller does. The failed call changed nothing, so
  // the bytes it did not push are still in the handle.
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  env_.FailOpAfter(Op::kFlush, 0);
  EXPECT_FALSE(writer->Flush().ok());
  ASSERT_OK(writer->Append("def"));
  EXPECT_OK(writer->Flush());
  EXPECT_EQ(env_.FileContents("/d/f"), "abcdef");
  ASSERT_OK(writer->Close());
}

TEST_F(FaultEnvTest, FlushIsNotSyncAndSyncIsNotFlush) {
  // kFlush counts Flush calls only: the flush inside Sync and Close is theirs.
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  env_.FailOpAfter(Op::kFlush, 0);
  ASSERT_OK(writer->Append("abc"));
  EXPECT_OK(writer->Sync());
  EXPECT_OK(writer->Close());
  EXPECT_EQ(env_.injected_errors(), 0u);
  EXPECT_EQ(env_.FileContents("/d/f"), "abc");
}

TEST_F(FaultEnvTest, AFailedCloseLosesWhatTheHandleBuffered) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("abc"));
  env_.FailOpAfter(Op::kClose, 0);
  EXPECT_FALSE(writer->Close().ok());
  EXPECT_EQ(env_.FileContents("/d/f"), "");
}

TEST_F(FaultEnvTest, AFailedCloseStillClosesTheHandle) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  env_.FailOpAfter(Op::kClose, 0);
  EXPECT_TRUE(IsInjected(writer->Close(), "/d/f"));
  // The file is closed even when Close reports an error, so a second Close is a use after Close.
  EXPECT_DEATH(static_cast<void>(writer->Close()), "after Close");
}

TEST_F(FaultEnvTest, EveryOpCanBeFailedAndAFailedOpHasNoEffect) {
  env_.SetFileContents("/d/a", "1");
  env_.SetFileContents("/d/b", "2");

  std::unique_ptr<SequentialFile> sequential;
  std::unique_ptr<RandomAccessFile> random_access;
  std::unique_ptr<WritableFile> writable;
  char scratch[10];
  std::string_view result;

  env_.FailOpAfter(Op::kNewSequential, 0);
  EXPECT_TRUE(IsInjected(env_.NewSequentialFile("/d/a", &sequential), "/d/a"));
  EXPECT_EQ(sequential, nullptr);

  env_.FailOpAfter(Op::kNewRandomAccess, 0);
  EXPECT_TRUE(IsInjected(env_.NewRandomAccessFile("/d/a", &random_access), "/d/a"));
  EXPECT_EQ(random_access, nullptr);

  env_.FailOpAfter(Op::kNewWritable, 0);
  EXPECT_TRUE(IsInjected(env_.NewWritableFile("/d/a", &writable), "/d/a"));
  EXPECT_EQ(writable, nullptr);
  EXPECT_EQ(env_.FileContents("/d/a"), "1") << "a failed open must not truncate";

  env_.FailOpAfter(Op::kRename, 0);
  EXPECT_TRUE(IsInjected(env_.RenameFile("/d/a", "/d/b"), "/d/a"));
  EXPECT_EQ(env_.FileContents("/d/a"), "1");
  EXPECT_EQ(env_.FileContents("/d/b"), "2");

  env_.FailOpAfter(Op::kRemove, 0);
  EXPECT_TRUE(IsInjected(env_.RemoveFile("/d/a"), "/d/a"));
  EXPECT_TRUE(env_.FileExists("/d/a"));

  env_.FailOpAfter(Op::kSyncDir, 0);
  ASSERT_OK(env_.NewWritableFile("/d/c", &writable));
  ASSERT_OK(writable->Close());
  EXPECT_TRUE(IsInjected(env_.SyncDir("/d"), "/d"));
  env_.SimulatePowerLossDropUnsynced();
  EXPECT_FALSE(env_.FileExists("/d/c")) << "a failed SyncDir must not make anything durable";

  ASSERT_OK(env_.NewWritableFile("/d/h", &writable));
  ASSERT_OK(writable->Append("x"));
  env_.FailOpAfter(Op::kFlush, 0);
  EXPECT_TRUE(IsInjected(writable->Flush(), "/d/h"));
  EXPECT_EQ(env_.FileContents("/d/h"), "");

  ASSERT_OK(env_.NewSequentialFile("/d/a", &sequential));
  ASSERT_OK(env_.NewRandomAccessFile("/d/a", &random_access));
  env_.FailOpAfter(Op::kRead, 0);
  EXPECT_TRUE(IsInjected(sequential->Read(1, &result, scratch), "/d/a"));
  env_.FailOpAfter(Op::kRead, 0);
  EXPECT_TRUE(IsInjected(random_access->Read(0, 1, &result, scratch), "/d/a"));
  EXPECT_EQ(env_.injected_errors(), 9u);
}

TEST_F(FaultEnvTest, OpsAreCountedSeparately) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  env_.FailOpAfter(Op::kSync, 1);
  EXPECT_OK(writer->Append("a"));
  EXPECT_OK(writer->Append("b"));
  EXPECT_OK(writer->Sync()) << "appends are not syncs";
  EXPECT_FALSE(writer->Sync().ok());
  ASSERT_OK(writer->Close());
}

TEST_F(FaultEnvTest, AFailureOfAnotherKindDoesNotUseUpTheCountdown) {
  // FailOpAfter(op, n) lets n successful calls of op through. A call that fails for any other
  // reason is not one of them.
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  ASSERT_OK(writer->Append("x"));
  env_.FailOpAfter(Op::kSync, 1);
  env_.SetFailureProbability(Op::kSync, 1.0);
  EXPECT_FALSE(writer->Sync().ok());  // Fails by probability: not a success.
  EXPECT_FALSE(writer->Sync().ok());
  env_.SetFailureProbability(Op::kSync, 0.0);
  EXPECT_OK(writer->Sync()) << "the one success the countdown waits for";
  EXPECT_TRUE(IsInjected(writer->Sync(), "/d/f"));
  EXPECT_OK(writer->Sync());
  EXPECT_EQ(env_.injected_errors(), 3u);
  ASSERT_OK(writer->Close());
}

TEST_F(FaultEnvTest, ACallThatFailsForARealReasonDoesNotUseUpTheCountdown) {
  env_.SetFileContents("/d/a", "1");
  env_.FailOpAfter(Op::kRemove, 1);
  EXPECT_FALSE(env_.RemoveFile("/d/missing").ok());
  EXPECT_EQ(env_.injected_errors(), 0u);
  EXPECT_OK(env_.RemoveFile("/d/a")) << "the first success";
  env_.SetFileContents("/d/b", "2");
  EXPECT_TRUE(IsInjected(env_.RemoveFile("/d/b"), "/d/b"));
}

TEST_F(FaultEnvTest, ArmingAnOpAgainReplacesTheEarlierCountdown) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  env_.FailOpAfter(Op::kSync, 5);
  env_.FailOpAfter(Op::kSync, 1);
  EXPECT_OK(writer->Sync());
  EXPECT_FALSE(writer->Sync().ok());
  EXPECT_OK(writer->Sync());
  ASSERT_OK(writer->Close());
}

TEST_F(FaultEnvTest, AnInvalidatedHandleDoesNotUseUpTheCountdown) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  env_.FailOpAfter(Op::kSync, 0);
  env_.SimulateProcessCrash();
  EXPECT_TRUE(IsInvalidated(writer->Sync()));
  EXPECT_EQ(env_.injected_errors(), 0u);
  std::unique_ptr<WritableFile> fresh;
  ASSERT_OK(env_.NewWritableFile("/d/g", &fresh));
  EXPECT_TRUE(IsInjected(fresh->Sync(), "/d/g")) << "the injection is still waiting";
  ASSERT_OK(fresh->Close());
}

TEST_F(FaultEnvTest, FailureProbabilityExtremes) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  env_.SetFailureProbability(Op::kAppend, 1.0);
  int failed = 0;
  for (int i = 0; i < 100; i++) {
    failed += writer->Append("x").ok() ? 0 : 1;
  }
  EXPECT_EQ(failed, 100);
  EXPECT_EQ(env_.injected_errors(), 100u);

  env_.SetFailureProbability(Op::kAppend, 0.0);
  failed = 0;
  for (int i = 0; i < 100; i++) {
    failed += writer->Append("x").ok() ? 0 : 1;
  }
  EXPECT_EQ(failed, 0);
  ASSERT_OK(writer->Close());
  EXPECT_EQ(env_.FileContents("/d/f"), std::string(100, 'x')) << "failed appends wrote nothing";
}

TEST_F(FaultEnvTest, FailureProbabilityBetweenTheExtremesFailsSometimes) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  env_.SetFailureProbability(Op::kAppend, 0.25);
  int failed = 0;
  for (int i = 0; i < 2000; i++) {
    failed += writer->Append("x").ok() ? 0 : 1;
  }
  EXPECT_GT(failed, 350);  // 25% of 2000 is 500, and the spread is about 20.
  EXPECT_LT(failed, 650);
  EXPECT_EQ(env_.injected_errors(), static_cast<uint64_t>(failed));
  ASSERT_OK(writer->Close());
}

TEST_F(FaultEnvTest, FailureProbabilityIsPerOp) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  env_.SetFailureProbability(Op::kSync, 1.0);
  EXPECT_OK(writer->Append("x"));
  EXPECT_FALSE(writer->Sync().ok());
  ASSERT_OK(writer->Close());
}

// Which of 200 appends fail, as a string of 0 and 1.
std::string FailurePattern(uint64_t seed) {
  FaultInjectionEnv env(seed);
  std::unique_ptr<WritableFile> writer;
  EXPECT_OK(env.NewWritableFile("/f", &writer));
  env.SetFailureProbability(Op::kAppend, 0.5);
  std::string pattern;
  for (int i = 0; i < 200; i++) {
    pattern += writer->Append("x").ok() ? '0' : '1';
  }
  EXPECT_OK(writer->Close());
  return pattern;
}

TEST_F(FaultEnvTest, FailureProbabilityFollowsTheSeed) {
  EXPECT_EQ(FailurePattern(11), FailurePattern(11));
  EXPECT_NE(FailurePattern(11), FailurePattern(12));
}

TEST_F(FaultEnvTest, ClearFaultsRemovesEveryConfiguredFault) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_.NewWritableFile("/d/f", &writer));
  env_.FailOpAfter(Op::kAppend, 0);
  env_.SetFailureProbability(Op::kSync, 1.0);
  env_.SetReadCorruptionProbability(1.0);
  env_.ClearFaults();
  EXPECT_OK(writer->Append("abc"));
  EXPECT_OK(writer->Sync());
  ASSERT_OK(writer->Close());
  std::string contents;
  ASSERT_OK(ReadFileToString(&env_, "/d/f", &contents));
  EXPECT_EQ(contents, "abc");
  EXPECT_EQ(env_.injected_errors(), 0u);
  EXPECT_EQ(env_.injected_corruptions(), 0u);
}

TEST_F(FaultEnvTest, ClearFaultsKeepsTheCounters) {
  env_.FailOpAfter(Op::kRemove, 0);
  EXPECT_FALSE(env_.RemoveFile("/d/missing").ok());
  ASSERT_EQ(env_.injected_errors(), 1u);
  env_.ClearFaults();
  EXPECT_EQ(env_.injected_errors(), 1u);
}

TEST_F(FaultEnvTest, ProbabilitiesOutsideZeroToOneAbort) {
  EXPECT_DEATH(env_.SetFailureProbability(Op::kAppend, 1.5), "probability");
  EXPECT_DEATH(env_.SetFailureProbability(Op::kAppend, -0.1), "probability");
  EXPECT_DEATH(env_.SetReadCorruptionProbability(2.0), "probability");
  EXPECT_DEATH(env_.FailOpAfter(Op::kSync, -1), "successes");
}

// --- Read corruption ----------------------------------------------------------------------------

// The number of bits that differ between two strings of the same length.
int BitDifference(std::string_view a, std::string_view b) {
  int bits = 0;
  for (size_t i = 0; i < a.size(); i++) {
    bits += std::popcount(static_cast<unsigned char>(static_cast<unsigned char>(a[i]) ^
                                                     static_cast<unsigned char>(b[i])));
  }
  return bits;
}

TEST_F(FaultEnvTest, ReadCorruptionFlipsOneBit) {
  Random rnd(5);
  const std::string written = RandomBinaryString(&rnd, 200);
  env_.SetFileContents("/d/f", written);
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_OK(env_.NewRandomAccessFile("/d/f", &file));
  env_.SetReadCorruptionProbability(1.0);
  for (int i = 0; i < 100; i++) {
    char scratch[200];
    std::string_view result;
    ASSERT_OK(file->Read(0, 200, &result, scratch));
    ASSERT_EQ(result.size(), written.size());
    ASSERT_EQ(BitDifference(result, written), 1) << "read " << i;
  }
  EXPECT_EQ(env_.injected_corruptions(), 100u);
}

TEST_F(FaultEnvTest, SequentialReadsAreCorruptedToo) {
  Random rnd(6);
  const std::string written = RandomBinaryString(&rnd, 64);
  env_.SetFileContents("/d/f", written);
  std::unique_ptr<SequentialFile> file;
  ASSERT_OK(env_.NewSequentialFile("/d/f", &file));
  env_.SetReadCorruptionProbability(1.0);
  char scratch[64];
  std::string_view result;
  ASSERT_OK(file->Read(64, &result, scratch));
  EXPECT_EQ(BitDifference(result, written), 1);
  EXPECT_EQ(env_.injected_corruptions(), 1u);
}

TEST_F(FaultEnvTest, ReadCorruptionNeverTouchesTheStoredBytes) {
  const std::string written(100, 'a');
  env_.SetFileContents("/d/f", written);
  env_.SetReadCorruptionProbability(1.0);
  std::string corrupted;
  ASSERT_OK(ReadFileToString(&env_, "/d/f", &corrupted));
  EXPECT_NE(corrupted, written);
  EXPECT_EQ(env_.FileContents("/d/f"), written);
  env_.SetReadCorruptionProbability(0.0);
  std::string clean;
  ASSERT_OK(ReadFileToString(&env_, "/d/f", &clean));
  EXPECT_EQ(clean, written);
}

TEST_F(FaultEnvTest, ReadCorruptionLeavesEmptyReadsAlone) {
  env_.SetFileContents("/d/f", "abc");
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_OK(env_.NewRandomAccessFile("/d/f", &file));
  env_.SetReadCorruptionProbability(1.0);
  char scratch[10];
  std::string_view result = "not touched";
  ASSERT_OK(file->Read(3, 10, &result, scratch));
  EXPECT_TRUE(result.empty());
  ASSERT_OK(file->Read(0, 0, &result, scratch));
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(env_.injected_corruptions(), 0u) << "there is no bit to flip in nothing";
}

TEST_F(FaultEnvTest, AFailedReadIsNotAlsoCorrupted) {
  env_.SetFileContents("/d/f", "abc");
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_OK(env_.NewRandomAccessFile("/d/f", &file));
  env_.SetReadCorruptionProbability(1.0);
  env_.FailOpAfter(Op::kRead, 0);
  char scratch[10];
  std::string_view result;
  EXPECT_FALSE(file->Read(0, 3, &result, scratch).ok());
  EXPECT_EQ(env_.injected_corruptions(), 0u);
}

TEST_F(FaultEnvTest, ReadCorruptionBetweenTheExtremesHitsSomeReads) {
  env_.SetFileContents("/d/f", std::string(16, 'a'));
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_OK(env_.NewRandomAccessFile("/d/f", &file));
  env_.SetReadCorruptionProbability(0.5);
  int corrupted = 0;
  for (int i = 0; i < 1000; i++) {
    char scratch[16];
    std::string_view result;
    ASSERT_OK(file->Read(0, 16, &result, scratch));
    corrupted += result == std::string(16, 'a') ? 0 : 1;
  }
  EXPECT_GT(corrupted, 400);
  EXPECT_LT(corrupted, 600);
  EXPECT_EQ(env_.injected_corruptions(), static_cast<uint64_t>(corrupted));
}

TEST_F(FaultEnvTest, EveryBitPositionIsReachable) {
  // The bit to flip is drawn uniformly over the whole read, so over many reads each of the
  // 2 bytes x 8 bits comes up.
  env_.SetFileContents("/d/f", std::string(2, '\0'));
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_OK(env_.NewRandomAccessFile("/d/f", &file));
  env_.SetReadCorruptionProbability(1.0);
  std::set<std::string> seen;
  for (int i = 0; i < 400; i++) {
    char scratch[2];
    std::string_view result;
    ASSERT_OK(file->Read(0, 2, &result, scratch));
    seen.emplace(result);
  }
  EXPECT_EQ(seen.size(), 16u);
}

// --- Determinism --------------------------------------------------------------------------------

// Runs a fixed script of file operations, with three power losses in it, on `env` and returns the
// contents of every file the script touches.
std::map<std::string, std::string> RunScript(FaultInjectionEnv* env) {
  Random script(99);
  EXPECT_OK(env->CreateDir("/d"));
  const int kFiles = 6;
  for (int round = 0; round < 4; round++) {
    for (int i = 0; i < kFiles; i++) {
      if (script.OneIn(4)) {
        continue;
      }
      const std::string path = "/d/f" + std::to_string(i);
      std::unique_ptr<WritableFile> file;
      EXPECT_OK(env->NewWritableFile(path, &file));
      const uint32_t chunks = 1 + script.Uniform(3);
      for (uint32_t c = 0; c < chunks; c++) {
        EXPECT_OK(file->Append(RandomString(&script, 1 + script.Uniform(50))));
        if (script.OneIn(2)) {
          EXPECT_OK(file->Sync());
        }
      }
      EXPECT_OK(file->Close());
    }
    if (script.OneIn(2)) {
      EXPECT_OK(env->SyncDir("/d"));
    }
    if (round < 3) {
      env->SimulatePowerLoss();
    }
  }
  std::map<std::string, std::string> contents;
  for (int i = 0; i < kFiles; i++) {
    const std::string path = "/d/f" + std::to_string(i);
    contents[path] = env->FileContents(path);
  }
  return contents;
}

TEST_F(FaultEnvTest, SameSeedSameOutcome) {
  FaultInjectionEnv a(7);
  FaultInjectionEnv b(7);
  const std::map<std::string, std::string> from_a = RunScript(&a);
  const std::map<std::string, std::string> from_b = RunScript(&b);
  EXPECT_EQ(from_a, from_b);
  EXPECT_EQ(a.crashes(), 3u);
}

TEST_F(FaultEnvTest, DifferentSeedsCanGiveDifferentOutcomes) {
  // The script is the same, so any difference comes from the seed alone.
  std::set<std::map<std::string, std::string>> outcomes;
  for (uint64_t seed = 0; seed < 20; seed++) {
    FaultInjectionEnv env(seed);
    outcomes.insert(RunScript(&env));
  }
  EXPECT_GT(outcomes.size(), 1u);
}

TEST_F(FaultEnvTest, ConfiguringOtherFaultsDoesNotChangeWhichBytesSurviveAPowerLoss) {
  // The power-loss draws come from their own stream, so a test that adds read corruption or
  // probabilistic failures to a run does not change what a given seed loses.
  auto survivors = [](bool noisy) {
    FaultInjectionEnv env(21);
    EXPECT_OK(env.CreateDir("/d"));
    EXPECT_OK(Put(&env, "/d/f", "", std::string(1000, 'x')));
    EXPECT_OK(env.SyncDir("/d"));
    if (noisy) {
      env.SetReadCorruptionProbability(0.5);
      env.SetFailureProbability(Op::kRead, 0.5);
      std::unique_ptr<RandomAccessFile> file;
      EXPECT_OK(env.NewRandomAccessFile("/d/f", &file));
      for (int i = 0; i < 100; i++) {
        char scratch[10];
        std::string_view result;
        static_cast<void>(file->Read(0, 10, &result, scratch));
      }
    }
    env.SimulatePowerLoss();
    return env.FileContents("/d/f").size();
  };
  EXPECT_EQ(survivors(false), survivors(true));
}

// --- Equivalence with the plain in-memory Env ---------------------------------------------------

TEST_F(FaultEnvTest, NoFaultsConfiguredMeansNoFaults) {
  constexpr uint64_t kSeed = 3;
  SCOPED_TRACE("seed " + std::to_string(kSeed));
  Random rnd(kSeed);
  FaultInjectionEnv fault(/*seed=*/9);
  const std::unique_ptr<Env> mem = NewMemEnv();
  ASSERT_OK(fault.CreateDir("/d"));
  ASSERT_OK(mem->CreateDir("/d"));

  const std::vector<std::string> names = {"/d/a", "/d/b", "/d/c", "/d/e"};
  // What each file holds, as the plain Env shows it (everything appended) and as the fault Env
  // shows it (what was flushed: it buffers in the handle). A close hands the buffer over, so the
  // two meet at every Flush, Sync and Close.
  std::map<std::string, std::string> shadow;
  std::map<std::string, std::string> flushed;
  struct Handles {
    std::unique_ptr<WritableFile> fault;
    std::unique_ptr<WritableFile> mem;
  };
  std::map<std::string, Handles> open;

  auto close_handles = [&](const std::string& name) {
    auto it = open.find(name);
    if (it == open.end()) {
      return;
    }
    EXPECT_OK(it->second.fault->Close());
    EXPECT_OK(it->second.mem->Close());
    open.erase(it);
    flushed[name] = shadow[name];
  };
  // Both Envs must agree on whether a call succeeds, and on nothing else needing a check here.
  auto same = [](const Status& from_fault, const Status& from_mem, const std::string& what) {
    EXPECT_EQ(from_fault.ok(), from_mem.ok())
        << what << ": fault " << from_fault.ToString() << ", mem " << from_mem.ToString();
    return from_mem.ok();
  };

  for (int step = 0; step < 1000; step++) {
    SCOPED_TRACE("step " + std::to_string(step));
    const std::string& name = names[rnd.Uniform(static_cast<uint32_t>(names.size()))];
    switch (rnd.Uniform(8)) {
      case 0: {  // Create or replace.
        close_handles(name);
        Handles handles;
        ASSERT_TRUE(same(fault.NewWritableFile(name, &handles.fault),
                         mem->NewWritableFile(name, &handles.mem), "create " + name));
        open[name] = std::move(handles);
        shadow[name] = "";
        flushed[name] = "";
        break;
      }
      case 1: {  // Append to an open file.
        auto it = open.find(name);
        if (it == open.end()) {
          break;
        }
        const std::string data = RandomBinaryString(&rnd, rnd.Uniform(200));
        ASSERT_TRUE(
            same(it->second.fault->Append(data), it->second.mem->Append(data), "append " + name));
        shadow[name] += data;
        break;
      }
      case 2: {  // Flush or sync an open file.
        auto it = open.find(name);
        if (it == open.end()) {
          break;
        }
        if (rnd.OneIn(2)) {
          ASSERT_TRUE(same(it->second.fault->Flush(), it->second.mem->Flush(), "flush " + name));
        } else {
          ASSERT_TRUE(same(it->second.fault->Sync(), it->second.mem->Sync(), "sync " + name));
        }
        flushed[name] = shadow[name];
        break;
      }
      case 3:  // Close.
        close_handles(name);
        break;
      case 4: {  // Rename.
        const std::string& target = names[rnd.Uniform(static_cast<uint32_t>(names.size()))];
        close_handles(name);
        close_handles(target);
        const bool ok = same(fault.RenameFile(name, target), mem->RenameFile(name, target),
                             "rename " + name + " to " + target);
        EXPECT_EQ(ok, shadow.count(name) != 0);
        if (ok && name != target) {
          shadow[target] = std::move(shadow[name]);
          shadow.erase(name);
          flushed[target] = std::move(flushed[name]);
          flushed.erase(name);
        }
        break;
      }
      case 5: {  // Remove.
        close_handles(name);
        const bool ok = same(fault.RemoveFile(name), mem->RemoveFile(name), "remove " + name);
        EXPECT_EQ(ok, shadow.count(name) != 0);
        shadow.erase(name);
        flushed.erase(name);
        break;
      }
      case 6: {  // Read the whole file.
        std::string from_fault;
        std::string from_mem;
        const bool ok = same(ReadFileToString(&fault, name, &from_fault),
                             ReadFileToString(mem.get(), name, &from_mem), "read " + name);
        EXPECT_EQ(ok, shadow.count(name) != 0);
        if (ok) {
          EXPECT_TRUE(from_fault == flushed[name]) << "the fault Env differs from the shadow model";
          EXPECT_TRUE(from_mem == shadow[name]) << "the model itself is wrong";
        }
        break;
      }
      default: {  // List the directory, sync it, and ask for a size.
        std::vector<std::string> from_fault;
        std::vector<std::string> from_mem;
        ASSERT_TRUE(
            same(fault.GetChildren("/d", &from_fault), mem->GetChildren("/d", &from_mem), "list"));
        std::sort(from_fault.begin(), from_fault.end());
        std::sort(from_mem.begin(), from_mem.end());
        EXPECT_EQ(from_fault, from_mem);
        ASSERT_TRUE(same(fault.SyncDir("/d"), mem->SyncDir("/d"), "sync dir"));
        uint64_t size_in_fault = 1;
        uint64_t size_in_mem = 2;
        if (same(fault.GetFileSize(name, &size_in_fault), mem->GetFileSize(name, &size_in_mem),
                 "size " + name)) {
          EXPECT_EQ(size_in_fault, flushed[name].size());
          EXPECT_EQ(size_in_mem, shadow[name].size());
        }
        break;
      }
    }
  }
  EXPECT_EQ(fault.injected_errors(), 0u);
  EXPECT_EQ(fault.injected_corruptions(), 0u);
  EXPECT_EQ(fault.crashes(), 0u);
  while (!open.empty()) {
    close_handles(open.begin()->first);
  }
}

// --- Threads ------------------------------------------------------------------------------------

TEST_F(FaultEnvTest, CrashesWhileOtherThreadsWriteOnlyEverInvalidateTheirHandles) {
  // Four writers append to their own files while the main thread keeps crashing the machine. Every
  // call either succeeds or reports the crash, nothing else, and the process neither aborts nor
  // trips a sanitizer.
  constexpr int kWriters = 4;
  std::atomic<int> unexpected{0};
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; w++) {
    writers.emplace_back([this, w, &unexpected] {
      const std::string path = "/d/w" + std::to_string(w);
      std::unique_ptr<WritableFile> file;
      if (!env_.NewWritableFile(path, &file).ok()) {
        unexpected++;
        return;
      }
      for (int i = 0; i < 2000; i++) {
        const Status s = i % 10 == 9 ? file->Sync() : file->Append("0123456789");
        if (!s.ok() && s.ToString().find("invalidated by crash") == std::string::npos) {
          unexpected++;
        }
      }
      static_cast<void>(file->Close());
    });
  }
  for (int i = 0; i < 100; i++) {
    if (i % 3 == 0) {
      env_.SimulatePowerLoss();
    } else if (i % 3 == 1) {
      env_.SimulatePowerLossDropUnsynced();
    } else {
      env_.SimulateProcessCrash();
    }
    std::this_thread::yield();
  }
  for (std::thread& writer : writers) {
    writer.join();
  }
  EXPECT_EQ(unexpected.load(), 0);
  EXPECT_EQ(env_.crashes(), 100u);
}

}  // namespace
}  // namespace shale
