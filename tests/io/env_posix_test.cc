// What only the POSIX Env promises, or can be checked against the real operating system. The
// behaviour every Env shares is in env_contract_test.

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "shale/env.h"
#include "shale/status.h"
#include "testkit/test_util.h"

namespace shale {
namespace {

testing::AssertionResult IsOk(const Status& s) {
  if (s.ok()) {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure() << s.ToString();
}

#define ASSERT_OK(expr) ASSERT_TRUE(IsOk(expr))

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
  ASSERT_OK(writer->Close());

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
// /dev/full accepts the open and fails every write with ENOSPC, which is the only way to reach the
// write error path without filling a disk. It exists on Linux only.
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
