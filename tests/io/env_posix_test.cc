// What only the POSIX Env promises, or can be checked against the real operating system. The
// behaviour every Env shares is in env_contract_test.

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>

#include "io/env_util.h"
#include "shale/env.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/random.h"

namespace shale {
namespace {

void IgnoreSignal(int) {}

// Caps the size of any file this process writes, so that a write which crosses the cap is cut
// short: write() stores the bytes up to the cap and returns that count, and the next write fails
// with EFBIG. That is a deterministic partial write followed by a deterministic error, on macOS and
// on Linux, with no fault-injection hook in the code under test. Crossing the cap raises SIGXFSZ,
// whose default action kills the process, so the signal is caught and dropped for as long as the
// cap is in force.
//
// The cap applies to the whole process, gtest's own output included, so a test keeps it in force
// only around the calls it means to cut short and checks the results after it is lifted.
class ScopedFileSizeLimit {
 public:
  explicit ScopedFileSizeLimit(rlim_t bytes) {
    struct sigaction ignore{};
    ignore.sa_handler = &IgnoreSignal;
    sigemptyset(&ignore.sa_mask);
    EXPECT_EQ(sigaction(SIGXFSZ, &ignore, &saved_action_), 0);
    EXPECT_EQ(getrlimit(RLIMIT_FSIZE, &saved_limit_), 0);
    struct rlimit capped = saved_limit_;
    capped.rlim_cur = bytes;
    EXPECT_EQ(setrlimit(RLIMIT_FSIZE, &capped), 0);
  }

  ScopedFileSizeLimit(const ScopedFileSizeLimit&) = delete;
  ScopedFileSizeLimit& operator=(const ScopedFileSizeLimit&) = delete;

  ~ScopedFileSizeLimit() {
    // Only the soft limit was lowered, so it can be raised back.
    EXPECT_EQ(setrlimit(RLIMIT_FSIZE, &saved_limit_), 0);
    EXPECT_EQ(sigaction(SIGXFSZ, &saved_action_, nullptr), 0);
  }

 private:
  struct rlimit saved_limit_{};
  struct sigaction saved_action_{};
};

TEST(PosixEnvTest, DefaultIsSingleton) {
  ASSERT_NE(Env::Default(), nullptr);
  EXPECT_EQ(Env::Default(), Env::Default());
}

TEST(PosixEnvTest, LockIsExclusiveAcrossProcesses) {
  Env* env = Env::Default();
  const std::string dir = NewTempDir("shale_env_posix_");
  const std::string lock_path = dir + "/LOCK";

  std::unique_ptr<FileLock> lock;
  ASSERT_OK(env->LockFile(lock_path, &lock));

  const pid_t child = fork();
  ASSERT_GE(child, 0) << "fork failed";
  if (child == 0) {
    // Only async-signal-safe work is allowed between fork and _exit in a process that may have
    // had other threads, but this one is single-threaded, so allocating is fine. The exit status
    // is the whole report: 0 means the lock was refused, as it must be.
    std::unique_ptr<FileLock> child_lock;
    const Status s = Env::Default()->LockFile(lock_path, &child_lock);
    _exit(s.ok() ? 1 : 0);
  }

  int wait_status = 0;
  ASSERT_EQ(waitpid(child, &wait_status, 0), child);
  ASSERT_TRUE(WIFEXITED(wait_status)) << "the child did not exit normally: " << wait_status;
  EXPECT_EQ(WEXITSTATUS(wait_status), 0) << "the child took a lock that this process holds: the "
                                            "lock is not exclusive across processes";

  // The lock is still ours, and releasing it lets another process in.
  ASSERT_OK(env->UnlockFile(std::move(lock)));
  const pid_t second_child = fork();
  ASSERT_GE(second_child, 0) << "fork failed";
  if (second_child == 0) {
    std::unique_ptr<FileLock> child_lock;
    const Status s = Env::Default()->LockFile(lock_path, &child_lock);
    _exit(s.ok() ? 0 : 1);
  }
  ASSERT_EQ(waitpid(second_child, &wait_status, 0), second_child);
  ASSERT_TRUE(WIFEXITED(wait_status)) << "the child did not exit normally: " << wait_status;
  EXPECT_EQ(WEXITSTATUS(wait_status), 0)
      << "the child could not take the lock after it was released";

  static_cast<void>(env->RemoveFile(lock_path));
  static_cast<void>(env->RemoveDir(dir));
}

TEST(PosixEnvTest, AppendsStayInUserSpaceUntilFlushed) {
  // The write buffer is the point of the class: a small append must not cost a system call. The
  // way to see that from outside is the file size, which only moves when the bytes reach the OS.
  Env* env = Env::Default();
  const std::string dir = NewTempDir("shale_env_posix_");
  const std::string path = dir + "/f";
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env->NewWritableFile(path, &writer));
  ASSERT_OK(writer->Append("abc"));
  uint64_t size = 99;
  ASSERT_OK(env->GetFileSize(path, &size));
  EXPECT_EQ(size, 0u) << "a 3-byte append reached the file before any Flush";
  ASSERT_OK(writer->Flush());
  ASSERT_OK(env->GetFileSize(path, &size));
  EXPECT_EQ(size, 3u);

  // The buffer holds 64 KiB. Two appends of 40000 bytes do not both fit, so the second one first
  // hands the 40000 buffered bytes to the OS to keep the order, and then waits in the buffer
  // itself.
  const std::string chunk(40000, 'x');
  ASSERT_OK(writer->Append(chunk));
  ASSERT_OK(env->GetFileSize(path, &size));
  EXPECT_EQ(size, 3u) << "an append that fits in the buffer must not be written yet";
  ASSERT_OK(writer->Append(chunk));
  ASSERT_OK(env->GetFileSize(path, &size));
  EXPECT_EQ(size, 3u + 40000u) << "an append that does not fit must flush the buffer before it";
  ASSERT_OK(writer->Close());
  ASSERT_OK(env->GetFileSize(path, &size));
  EXPECT_EQ(size, 3u + 80000u);

  static_cast<void>(env->RemoveFile(path));
  static_cast<void>(env->RemoveDir(dir));
}

TEST(PosixEnvTest, FlushAfterAPartialWriteDoesNotRepeatBytesThatReachedTheFile) {
  // The flush is cut short after 1000 of 2000 bytes. The 1000 that reached the file must leave the
  // buffer, or the Close that follows writes them a second time.
  constexpr uint64_t kLimit = 1000;
  Env* env = Env::Default();
  const std::string dir = NewTempDir("shale_env_posix_");
  const std::string path = dir + "/f";
  Random rnd(401);
  const std::string data = RandomBinaryString(&rnd, 2000);
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env->NewWritableFile(path, &writer));
  ASSERT_OK(writer->Append(data)) << "2000 bytes fit in the buffer, so nothing is written yet";

  Status flush;
  Status size_status;
  uint64_t size_after_failure = 0;
  {
    const ScopedFileSizeLimit limit(kLimit);
    flush = writer->Flush();
    size_status = env->GetFileSize(path, &size_after_failure);
  }
  EXPECT_TRUE(flush.IsIOError()) << flush.ToString();
  EXPECT_NE(flush.ToString().find(path), std::string::npos) << flush.ToString();
  ASSERT_OK(size_status);
  ASSERT_EQ(size_after_failure, kLimit)
      << "the size cap did not cut the write short, so the partial-write path was not reached";

  ASSERT_OK(writer->Close());
  std::string contents;
  ASSERT_OK(ReadFileToString(env, path, &contents));
  EXPECT_EQ(contents.size(), data.size()) << "bytes were repeated or lost after the partial write";
  EXPECT_TRUE(contents == data) << "the file is not what was appended";

  static_cast<void>(env->RemoveFile(path));
  static_cast<void>(env->RemoveDir(dir));
}

TEST(PosixEnvTest, AppendOfAWholeBufferCutShortKeepsWhatReachedTheFileInOrder) {
  // The same cut, on the branch that writes a large append straight to the file. The small append
  // before it is flushed first and fits under the cap; the large one is cut after 700 bytes. What
  // is in the file is then a prefix of everything appended, in order, with nothing repeated, and
  // closing the file adds nothing.
  constexpr uint64_t kLimit = 1000;
  Env* env = Env::Default();
  const std::string dir = NewTempDir("shale_env_posix_");
  const std::string path = dir + "/f";
  Random rnd(402);
  const std::string small = RandomBinaryString(&rnd, 300);
  const std::string large = RandomBinaryString(&rnd, 70000);
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env->NewWritableFile(path, &writer));
  ASSERT_OK(writer->Append(small));

  Status append;
  Status size_status;
  uint64_t size_after_failure = 0;
  {
    const ScopedFileSizeLimit limit(kLimit);
    append = writer->Append(large);
    size_status = env->GetFileSize(path, &size_after_failure);
  }
  EXPECT_TRUE(append.IsIOError()) << append.ToString();
  EXPECT_NE(append.ToString().find(path), std::string::npos) << append.ToString();
  ASSERT_OK(size_status);
  ASSERT_EQ(size_after_failure, kLimit)
      << "the size cap did not cut the write short, so the partial-write path was not reached";

  ASSERT_OK(writer->Close());
  std::string contents;
  ASSERT_OK(ReadFileToString(env, path, &contents));
  const std::string appended = small + large;
  EXPECT_EQ(contents.size(), kLimit);
  EXPECT_TRUE(contents == appended.substr(0, kLimit))
      << "the file is not a prefix of the bytes that were appended";

  static_cast<void>(env->RemoveFile(path));
  static_cast<void>(env->RemoveDir(dir));
}

TEST(PosixEnvTest, SkipBeyondAnyFileOffsetIsAnIOErrorNamingThePath) {
  Env* env = Env::Default();
  const std::string dir = NewTempDir("shale_env_posix_");
  const std::string path = dir + "/skip_target";
  ASSERT_OK(WriteStringToFile(env, "abc", path, /*sync=*/false));
  std::unique_ptr<SequentialFile> reader;
  ASSERT_OK(env->NewSequentialFile(path, &reader));
  // The contract is that every failure is an IOError that names the path.
  const Status s = reader->Skip(UINT64_MAX);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_NE(s.ToString().find("skip_target"), std::string::npos) << s.ToString();
  reader.reset();

  static_cast<void>(env->RemoveFile(path));
  static_cast<void>(env->RemoveDir(dir));
}

TEST(PosixEnvTest, NewWritableFileInAMissingDirectoryFails) {
  Env* env = Env::Default();
  const std::string dir = NewTempDir("shale_env_posix_");
  std::unique_ptr<WritableFile> writer;
  const Status s = env->NewWritableFile(dir + "/no_such_dir/f", &writer);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_NE(s.ToString().find("no_such_dir"), std::string::npos) << s.ToString();
  EXPECT_EQ(writer, nullptr);
  static_cast<void>(env->RemoveDir(dir));
}

#ifdef __linux__
// /dev/full accepts the open and fails every write with ENOSPC, which checks the error a full disk
// gives. It writes nothing before it fails, so it cannot reach the partial-write path; the size cap
// above does. It exists on Linux only.
TEST(PosixEnvTest, WriteFailureNamesThePathAndTheCause) {
  Env* env = Env::Default();
  ASSERT_TRUE(env->FileExists("/dev/full")) << "this test needs /dev/full";
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env->NewWritableFile("/dev/full", &writer));
  ASSERT_OK(writer->Append("x")) << "the append only fills the buffer, nothing is written yet";
  const Status s = writer->Flush();
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_NE(s.ToString().find("/dev/full"), std::string::npos) << s.ToString();
  EXPECT_NE(s.ToString().find("No space left"), std::string::npos) << s.ToString();
  EXPECT_FALSE(writer->Sync().ok()) << "Sync of bytes that cannot be written must not succeed";
}
#endif  // __linux__

}  // namespace
}  // namespace shale
