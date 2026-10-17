// The behaviour every Env must have, run against each implementation. A new Env passes this suite
// or it is not an Env: the engine cannot tell a real filesystem from a simulated one, and neither
// may the tests that stand in for it.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/env_util.h"
#include "shale/env.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/check.h"
#include "util/random.h"

namespace shale {
namespace {

// An Env together with a fresh, empty directory to work in. The directory disappears with the
// object, so a test never sees what an earlier one left behind.
class EnvUnderTest {
 public:
  virtual ~EnvUnderTest() = default;
  virtual Env* env() = 0;
  virtual const std::string& dir() const = 0;
};

using EnvFactory = std::function<std::unique_ptr<EnvUnderTest>()>;

// Removes `dir` and everything below it, ignoring failures: it only tidies up after a test.
void RemoveTree(Env* env, const std::string& dir) {
  std::vector<std::string> children;
  if (!env->GetChildren(dir, &children).ok()) {
    return;
  }
  for (const std::string& child : children) {
    const std::string path = dir + "/" + child;
    if (!env->RemoveFile(path).ok()) {
      RemoveTree(env, path);
    }
  }
  static_cast<void>(env->RemoveDir(dir));
}

class PosixEnvUnderTest : public EnvUnderTest {
 public:
  PosixEnvUnderTest() : dir_(NewTempDir("shale_env_contract_")) {}
  ~PosixEnvUnderTest() override { RemoveTree(env(), dir_); }

  Env* env() override { return Env::Default(); }
  const std::string& dir() const override { return dir_; }

 private:
  const std::string dir_;
};

class MemEnvUnderTest : public EnvUnderTest {
 public:
  MemEnvUnderTest() : env_(NewMemEnv()) {
    // A directory of its own, as the POSIX fixture has, so that the tests treat both alike.
    const Status s = env_->CreateDir(dir_);
    SHALE_CHECK(s.ok(), s.ToString());
  }

  Env* env() override { return env_.get(); }
  const std::string& dir() const override { return dir_; }

 private:
  const std::unique_ptr<Env> env_;
  const std::string dir_ = "/shale_env_contract";
};

struct EnvCase {
  std::string name;
  EnvFactory make;
};

// gtest prints the parameter of a failing test; without this it would print the bytes of the
// std::function.
void PrintTo(const EnvCase& env_case, std::ostream* os) { *os << env_case.name; }

class EnvContractTest : public testing::TestWithParam<EnvCase> {
 protected:
  void SetUp() override {
    fixture_ = GetParam().make();
    env_ = fixture_->env();
    dir_ = fixture_->dir();
  }

  std::string Path(const std::string& name) const { return dir_ + "/" + name; }

  // Creates `name` holding exactly `contents`.
  void WriteFile(const std::string& name, std::string_view contents) {
    ASSERT_OK(WriteStringToFile(env_, contents, Path(name), /*sync=*/false));
  }

  // Returns the contents of `name`, or fails the test.
  std::string ReadFile(const std::string& name) {
    std::string contents;
    EXPECT_OK(ReadFileToString(env_, Path(name), &contents));
    return contents;
  }

  std::unique_ptr<EnvUnderTest> fixture_;
  Env* env_ = nullptr;
  std::string dir_;
};

TEST_P(EnvContractTest, WriteThenReadSequential) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &writer));
  ASSERT_OK(writer->Append("hello "));
  ASSERT_OK(writer->Append("world"));
  ASSERT_OK(writer->Close());

  std::unique_ptr<SequentialFile> reader;
  ASSERT_OK(env_->NewSequentialFile(Path("f"), &reader));
  char scratch[100];
  std::string_view result;
  ASSERT_OK(reader->Read(5, &result, scratch));
  EXPECT_EQ(result, "hello");
  ASSERT_OK(reader->Read(100, &result, scratch));
  EXPECT_EQ(result, " world") << "a read that runs into the end returns what is left, not an error";
  ASSERT_OK(reader->Read(100, &result, scratch));
  EXPECT_TRUE(result.empty()) << "a read at the end of the file returns nothing, not an error";
}

TEST_P(EnvContractTest, SequentialSkip) {
  WriteFile("f", "hello world");
  std::unique_ptr<SequentialFile> reader;
  ASSERT_OK(env_->NewSequentialFile(Path("f"), &reader));
  ASSERT_OK(reader->Skip(6));
  char scratch[100];
  std::string_view result;
  ASSERT_OK(reader->Read(100, &result, scratch));
  EXPECT_EQ(result, "world");
}

TEST_P(EnvContractTest, SequentialSkipPastTheEndThenReadReturnsNothing) {
  WriteFile("f", "abc");
  std::unique_ptr<SequentialFile> reader;
  ASSERT_OK(env_->NewSequentialFile(Path("f"), &reader));
  ASSERT_OK(reader->Skip(1000));
  char scratch[10];
  std::string_view result = "not touched";
  ASSERT_OK(reader->Read(10, &result, scratch));
  EXPECT_TRUE(result.empty());
}

TEST_P(EnvContractTest, RandomAccessReadsAtOffsets) {
  WriteFile("f", "hello world");
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_OK(env_->NewRandomAccessFile(Path("f"), &file));
  char scratch[100];
  std::string_view result;
  ASSERT_OK(file->Read(6, 5, &result, scratch));
  EXPECT_EQ(result, "world");
  ASSERT_OK(file->Read(9, 100, &result, scratch));
  EXPECT_EQ(result, "ld") << "a read that runs into the end is short, not an error";
  ASSERT_OK(file->Read(100, 5, &result, scratch));
  EXPECT_TRUE(result.empty()) << "a read past the end returns nothing, not an error";
  // Reads are independent of each other: the file has no position.
  ASSERT_OK(file->Read(0, 5, &result, scratch));
  EXPECT_EQ(result, "hello");
}

TEST_P(EnvContractTest, LargeAppendAcrossBufferBoundary) {
  constexpr uint64_t kSeed = 301;
  SCOPED_TRACE("seed " + std::to_string(kSeed));
  Random rnd(kSeed);
  const size_t total = (size_t{1} << 20) + 17;
  const std::string expected = RandomBinaryString(&rnd, total);

  // The first chunk sizes sit on either side of a 64 KiB write buffer, whatever an implementation
  // does with one: fill it to the byte, overflow it by one, and exceed it in a single call. The
  // rest are random, up to 70000 bytes.
  const std::vector<size_t> boundary_chunks = {1, 65535, 1, 65536, 65537, 70000, 1};
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("big"), &writer));
  size_t written = 0;
  size_t chunk_index = 0;
  while (written < total) {
    size_t chunk = chunk_index < boundary_chunks.size() ? boundary_chunks[chunk_index]
                                                        : 1 + rnd.Uniform(70000);
    chunk_index++;
    chunk = std::min(chunk, total - written);
    ASSERT_OK(writer->Append(std::string_view(expected).substr(written, chunk)));
    written += chunk;
  }
  ASSERT_OK(writer->Close());

  uint64_t size = 0;
  ASSERT_OK(env_->GetFileSize(Path("big"), &size));
  EXPECT_EQ(size, total);
  const std::string actual = ReadFile("big");
  EXPECT_EQ(actual.size(), expected.size());
  EXPECT_TRUE(actual == expected) << "the bytes read back differ from the bytes appended";
}

TEST_P(EnvContractTest, FlushMakesAppendedBytesVisibleToFileSize) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &writer));
  ASSERT_OK(writer->Append("abcde"));
  ASSERT_OK(writer->Flush());
  uint64_t size = 0;
  ASSERT_OK(env_->GetFileSize(Path("f"), &size));
  EXPECT_EQ(size, 5u);
  ASSERT_OK(writer->Close());
}

TEST_P(EnvContractTest, SyncImpliesFlush) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &writer));
  ASSERT_OK(writer->Append("abcde"));
  ASSERT_OK(writer->Sync());
  uint64_t size = 0;
  ASSERT_OK(env_->GetFileSize(Path("f"), &size));
  EXPECT_EQ(size, 5u) << "Sync must hand the buffered bytes over before it makes them durable";
  ASSERT_OK(writer->Close());
}

TEST_P(EnvContractTest, CloseHandsOverBufferedBytes) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &writer));
  ASSERT_OK(writer->Append("abcde"));
  ASSERT_OK(writer->Close());
  EXPECT_EQ(ReadFile("f"), "abcde");
}

TEST_P(EnvContractTest, NewWritableFileTruncates) {
  WriteFile("f", "a much longer first version");
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &writer));
  uint64_t size = 99;
  ASSERT_OK(env_->GetFileSize(Path("f"), &size));
  EXPECT_EQ(size, 0u) << "opening for write must empty the file at once, before any append";
  ASSERT_OK(writer->Append("short"));
  ASSERT_OK(writer->Close());
  EXPECT_EQ(ReadFile("f"), "short");
}

TEST_P(EnvContractTest, TruncatingAnOpenFileIsSeenByItsReaders) {
  // Opening for write empties the file itself, not a copy of it, so a reader that was already open
  // finds it empty. Replacing the file by rename is the other case, and leaves the reader alone.
  WriteFile("f", "old contents");
  std::unique_ptr<RandomAccessFile> reader;
  ASSERT_OK(env_->NewRandomAccessFile(Path("f"), &reader));
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &writer));
  char scratch[100];
  std::string_view result = "not touched";
  ASSERT_OK(reader->Read(0, 100, &result, scratch));
  EXPECT_TRUE(result.empty());
  ASSERT_OK(writer->Append("new"));
  ASSERT_OK(writer->Close());
  ASSERT_OK(reader->Read(0, 100, &result, scratch));
  EXPECT_EQ(result, "new");
}

TEST_P(EnvContractTest, RenameReplacesTarget) {
  WriteFile("a", "1");
  WriteFile("b", "2");
  ASSERT_OK(env_->RenameFile(Path("a"), Path("b")));
  EXPECT_EQ(ReadFile("b"), "1");
  EXPECT_FALSE(env_->FileExists(Path("a")));
}

TEST_P(EnvContractTest, ReplacingAFileByRenameLeavesOpenReadersOnTheOldContents) {
  WriteFile("a", "new");
  WriteFile("b", "old contents");
  std::unique_ptr<RandomAccessFile> reader;
  ASSERT_OK(env_->NewRandomAccessFile(Path("b"), &reader));
  ASSERT_OK(env_->RenameFile(Path("a"), Path("b")));
  char scratch[100];
  std::string_view result;
  ASSERT_OK(reader->Read(0, 100, &result, scratch));
  EXPECT_EQ(result, "old contents");
  EXPECT_EQ(ReadFile("b"), "new");
}

TEST_P(EnvContractTest, RenameMovesToAnAbsentName) {
  WriteFile("a", "1");
  ASSERT_OK(env_->RenameFile(Path("a"), Path("c")));
  EXPECT_EQ(ReadFile("c"), "1");
  EXPECT_FALSE(env_->FileExists(Path("a")));
}

TEST_P(EnvContractTest, RemoveFileAndExists) {
  EXPECT_FALSE(env_->FileExists(Path("f")));
  WriteFile("f", "x");
  EXPECT_TRUE(env_->FileExists(Path("f")));
  ASSERT_OK(env_->RemoveFile(Path("f")));
  EXPECT_FALSE(env_->FileExists(Path("f")));
}

TEST_P(EnvContractTest, GetChildrenListsBareNames) {
  WriteFile("x", "1");
  WriteFile("y", "2");
  std::vector<std::string> children;
  ASSERT_OK(env_->GetChildren(dir_, &children));
  const std::set<std::string> names(children.begin(), children.end());
  EXPECT_EQ(names, (std::set<std::string>{"x", "y"}));
  EXPECT_EQ(children.size(), names.size()) << "a name must be listed once";
  EXPECT_EQ(names.count("."), 0u);
  EXPECT_EQ(names.count(".."), 0u);
}

TEST_P(EnvContractTest, GetChildrenListsSubdirectoriesAndNotTheirContents) {
  ASSERT_OK(env_->CreateDir(Path("sub")));
  WriteFile("sub/inner", "1");
  WriteFile("outer", "2");
  std::vector<std::string> children;
  ASSERT_OK(env_->GetChildren(dir_, &children));
  EXPECT_EQ(std::set<std::string>(children.begin(), children.end()),
            (std::set<std::string>{"outer", "sub"}));
  ASSERT_OK(env_->GetChildren(Path("sub"), &children));
  EXPECT_EQ(children, std::vector<std::string>{"inner"});
}

TEST_P(EnvContractTest, GetChildrenOfAnEmptyDirectoryIsEmpty) {
  std::vector<std::string> children = {"stale"};
  ASSERT_OK(env_->GetChildren(dir_, &children));
  EXPECT_TRUE(children.empty()) << "the result must replace what the vector held";
}

TEST_P(EnvContractTest, CreateDirIsIdempotent) {
  ASSERT_OK(env_->CreateDir(Path("sub")));
  EXPECT_TRUE(env_->FileExists(Path("sub")));
  ASSERT_OK(env_->CreateDir(Path("sub")));
  EXPECT_TRUE(env_->FileExists(Path("sub")));
}

TEST_P(EnvContractTest, CreateDirOverAFileFails) {
  WriteFile("plain_file", "x");
  const Status s = env_->CreateDir(Path("plain_file"));
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_NE(s.ToString().find("plain_file"), std::string::npos) << s.ToString();
}

TEST_P(EnvContractTest, RemoveDirRemovesAnEmptyDirectory) {
  ASSERT_OK(env_->CreateDir(Path("sub")));
  ASSERT_OK(env_->RemoveDir(Path("sub")));
  EXPECT_FALSE(env_->FileExists(Path("sub")));
}

TEST_P(EnvContractTest, RemoveDirRefusesANonEmptyDirectory) {
  ASSERT_OK(env_->CreateDir(Path("sub")));
  WriteFile("sub/f", "x");
  const Status s = env_->RemoveDir(Path("sub"));
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_TRUE(env_->FileExists(Path("sub/f"))) << "a refused removal must leave the contents";
}

TEST_P(EnvContractTest, MissingFileErrorsNameThePath) {
  std::unique_ptr<SequentialFile> file;
  const Status s = env_->NewSequentialFile(Path("nope"), &file);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_NE(s.ToString().find("nope"), std::string::npos) << s.ToString();
}

// Every operation that can fail must say which path it failed on: the message is often all a
// person woken at night has.
TEST_P(EnvContractTest, EveryFailureOnAMissingPathNamesIt) {
  const std::string missing = Path("missing_entry");
  std::unique_ptr<SequentialFile> sequential;
  std::unique_ptr<RandomAccessFile> random_access;
  std::vector<std::string> children;
  uint64_t size = 0;
  const std::vector<std::pair<std::string, Status>> failures = [&] {
    std::vector<std::pair<std::string, Status>> v;
    v.emplace_back("NewSequentialFile", env_->NewSequentialFile(missing, &sequential));
    v.emplace_back("NewRandomAccessFile", env_->NewRandomAccessFile(missing, &random_access));
    v.emplace_back("GetChildren", env_->GetChildren(missing, &children));
    v.emplace_back("RemoveFile", env_->RemoveFile(missing));
    v.emplace_back("RemoveDir", env_->RemoveDir(missing));
    v.emplace_back("GetFileSize", env_->GetFileSize(missing, &size));
    v.emplace_back("RenameFile", env_->RenameFile(missing, Path("target")));
    v.emplace_back("SyncDir", env_->SyncDir(missing));
    return v;
  }();
  for (const auto& [operation, status] : failures) {
    EXPECT_TRUE(status.IsIOError()) << operation << ": " << status.ToString();
    EXPECT_NE(status.ToString().find("missing_entry"), std::string::npos)
        << operation << " does not name the path: " << status.ToString();
  }
}

TEST_P(EnvContractTest, SyncAndSyncDirSucceed) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &writer));
  ASSERT_OK(writer->Append("durable"));
  ASSERT_OK(writer->Sync());
  ASSERT_OK(writer->Close());
  ASSERT_OK(env_->SyncDir(dir_));
}

TEST_P(EnvContractTest, LockIsExclusiveUntilUnlocked) {
  const std::string lock_path = Path("LOCK");
  std::unique_ptr<FileLock> first;
  ASSERT_OK(env_->LockFile(lock_path, &first));
  ASSERT_NE(first, nullptr);

  std::unique_ptr<FileLock> second;
  const Status s = env_->LockFile(lock_path, &second);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_NE(s.ToString().find("LOCK"), std::string::npos) << s.ToString();
  EXPECT_EQ(second, nullptr) << "a failed lock must not hand out a handle";

  ASSERT_OK(env_->UnlockFile(std::move(first)));
  ASSERT_OK(env_->LockFile(lock_path, &second));
  ASSERT_OK(env_->UnlockFile(std::move(second)));
}

TEST_P(EnvContractTest, LocksOnDifferentPathsDoNotConflict) {
  std::unique_ptr<FileLock> a;
  std::unique_ptr<FileLock> b;
  ASSERT_OK(env_->LockFile(Path("LOCK_A"), &a));
  ASSERT_OK(env_->LockFile(Path("LOCK_B"), &b));
  ASSERT_OK(env_->UnlockFile(std::move(a)));
  ASSERT_OK(env_->UnlockFile(std::move(b)));
}

TEST_P(EnvContractTest, DroppingALockReleasesIt) {
  // A process that dies, or a database that is abandoned, never calls UnlockFile. Its lock has to
  // go with it, or the next owner would be shut out by a lock nobody holds.
  const std::string lock_path = Path("LOCK");
  std::unique_ptr<FileLock> first;
  ASSERT_OK(env_->LockFile(lock_path, &first));
  first.reset();
  std::unique_ptr<FileLock> second;
  ASSERT_OK(env_->LockFile(lock_path, &second));
  ASSERT_OK(env_->UnlockFile(std::move(second)));
}

TEST_P(EnvContractTest, LockFileCreatesTheFile) {
  std::unique_ptr<FileLock> lock;
  ASSERT_OK(env_->LockFile(Path("LOCK"), &lock));
  EXPECT_TRUE(env_->FileExists(Path("LOCK")));
  ASSERT_OK(env_->UnlockFile(std::move(lock)));
  EXPECT_TRUE(env_->FileExists(Path("LOCK"))) << "unlocking leaves the lock file in place";
}

// A file that is removed or replaced lives on, nameless, for as long as a handle is open on it.
TEST_P(EnvContractTest, RemovedFileStaysReadableThroughOpenHandle) {
  WriteFile("f", "hello world");
  std::unique_ptr<SequentialFile> sequential;
  std::unique_ptr<RandomAccessFile> random_access;
  ASSERT_OK(env_->NewSequentialFile(Path("f"), &sequential));
  ASSERT_OK(env_->NewRandomAccessFile(Path("f"), &random_access));

  ASSERT_OK(env_->RemoveFile(Path("f")));
  EXPECT_FALSE(env_->FileExists(Path("f")));

  char scratch[100];
  std::string_view result;
  ASSERT_OK(sequential->Read(5, &result, scratch));
  EXPECT_EQ(result, "hello");
  ASSERT_OK(random_access->Read(6, 5, &result, scratch));
  EXPECT_EQ(result, "world");
}

TEST_P(EnvContractTest, RemovedFileDoesNotComeBackWhenItsWriterContinues) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &writer));
  ASSERT_OK(writer->Append("one"));
  ASSERT_OK(env_->RemoveFile(Path("f")));
  ASSERT_OK(writer->Append("two"));
  ASSERT_OK(writer->Close());
  EXPECT_FALSE(env_->FileExists(Path("f")));
}

TEST_P(EnvContractTest, ARecreatedFileIsNotTheOldOne) {
  WriteFile("f", "first");
  std::unique_ptr<RandomAccessFile> old_reader;
  ASSERT_OK(env_->NewRandomAccessFile(Path("f"), &old_reader));
  ASSERT_OK(env_->RemoveFile(Path("f")));
  WriteFile("f", "second");

  char scratch[100];
  std::string_view result;
  ASSERT_OK(old_reader->Read(0, 100, &result, scratch));
  EXPECT_EQ(result, "first");
  EXPECT_EQ(ReadFile("f"), "second");
}

TEST_P(EnvContractTest, SleepAdvancesClock) {
  const uint64_t t0 = env_->NowMicros();
  env_->SleepForMicroseconds(1000);
  EXPECT_GE(env_->NowMicros() - t0, 1000u);
}

TEST_P(EnvContractTest, ClockNeverGoesBackwards) {
  uint64_t previous = env_->NowMicros();
  for (int i = 0; i < 100; i++) {
    env_->SleepForMicroseconds(1);
    const uint64_t now = env_->NowMicros();
    ASSERT_GE(now, previous) << "at iteration " << i;
    previous = now;
  }
}

TEST_P(EnvContractTest, EnvUtilRoundTrip) {
  ASSERT_OK(WriteStringToFile(env_, "payload", Path("f"), /*sync=*/true));
  std::string data = "stale";
  ASSERT_OK(ReadFileToString(env_, Path("f"), &data));
  EXPECT_EQ(data, "payload") << "the result must replace what the string held";
}

TEST_P(EnvContractTest, EnvUtilRoundTripsEmptyAndBinaryData) {
  Random rnd(302);
  const std::string binary = RandomBinaryString(&rnd, 100000);
  ASSERT_OK(WriteStringToFile(env_, binary, Path("binary"), /*sync=*/false));
  ASSERT_OK(WriteStringToFile(env_, "", Path("empty"), /*sync=*/true));
  EXPECT_TRUE(ReadFile("binary") == binary);
  EXPECT_EQ(ReadFile("empty"), "");
  EXPECT_TRUE(env_->FileExists(Path("empty")));
}

TEST_P(EnvContractTest, EnvUtilWriteReplacesExistingFile) {
  ASSERT_OK(WriteStringToFile(env_, "a long first version", Path("f"), /*sync=*/false));
  ASSERT_OK(WriteStringToFile(env_, "short", Path("f"), /*sync=*/false));
  EXPECT_EQ(ReadFile("f"), "short");
}

TEST_P(EnvContractTest, EnvUtilReadMissingFileNamesThePath) {
  std::string data;
  const Status s = ReadFileToString(env_, Path("nope"), &data);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_NE(s.ToString().find("nope"), std::string::npos) << s.ToString();
}

// The handles refuse misuse rather than corrupt a file: using one after Close is a bug in the
// caller, and so is using one from two threads at once. The second is covered by
// exclusive_use_test, which can make it happen deterministically.
TEST_P(EnvContractTest, AppendAfterCloseAborts) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &writer));
  ASSERT_OK(writer->Close());
  EXPECT_DEATH(static_cast<void>(writer->Append("x")), "after Close");
}

TEST_P(EnvContractTest, CloseTwiceAborts) {
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &writer));
  ASSERT_OK(writer->Close());
  EXPECT_DEATH(static_cast<void>(writer->Close()), "after Close");
}

INSTANTIATE_TEST_SUITE_P(
    Envs, EnvContractTest,
    testing::Values(EnvCase{"posix", [] { return std::make_unique<PosixEnvUnderTest>(); }},
                    EnvCase{"mem", [] { return std::make_unique<MemEnvUnderTest>(); }}),
    // The parameter is not called `info`: the macro already has one of that name, and GCC's
    // -Wshadow rejects the lambda that hides it.
    [](const testing::TestParamInfo<EnvCase>& param_info) { return param_info.param.name; });

}  // namespace
}  // namespace shale
