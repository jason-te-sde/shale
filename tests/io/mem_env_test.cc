// What only the in-memory Env promises. The behaviour every Env shares is in env_contract_test,
// which runs against this one too.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "io/env_util.h"
#include "shale/env.h"
#include "shale/status.h"
#include "testkit/test_util.h"

namespace shale {
namespace {

std::string ReadAll(Env* env, const std::string& path) {
  std::string contents;
  EXPECT_OK(ReadFileToString(env, path, &contents));
  return contents;
}

TEST(MemEnvTest, ClockIsVirtual) {
  const std::unique_ptr<Env> env = NewMemEnv();
  EXPECT_EQ(env->NowMicros(), 0u);
  env->SleepForMicroseconds(1500);
  EXPECT_EQ(env->NowMicros(), 1500u);

  // A thousand sleeps of a second each take a thousand seconds of this Env's time and next to none
  // of ours. If a sleep really slept, this test would not finish; the bound just names the bug.
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; i++) {
    env->SleepForMicroseconds(1000000);
  }
  const auto wall = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(env->NowMicros(), 1500u + 1000u * 1000000u);
  EXPECT_LT(wall, std::chrono::seconds(1)) << "the virtual clock waited in real time";
}

TEST(MemEnvTest, TheClockMovesOnlyWhenSomeoneSleeps) {
  const std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  ASSERT_OK(env->NewWritableFile("/f", &file));
  ASSERT_OK(file->Append("data"));
  ASSERT_OK(file->Sync());
  ASSERT_OK(file->Close());
  EXPECT_EQ(env->NowMicros(), 0u) << "file operations must not advance the clock";
  env->SleepForMicroseconds(0);
  EXPECT_EQ(env->NowMicros(), 0u);
}

TEST(MemEnvTest, RemovedFileStaysReadableThroughOpenHandle) {
  const std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(WriteStringToFile(env.get(), "hello world", "/f", /*sync=*/false));
  std::unique_ptr<SequentialFile> sequential;
  std::unique_ptr<RandomAccessFile> random_access;
  ASSERT_OK(env->NewSequentialFile("/f", &sequential));
  ASSERT_OK(env->NewRandomAccessFile("/f", &random_access));

  ASSERT_OK(env->RemoveFile("/f"));
  EXPECT_FALSE(env->FileExists("/f"));

  char scratch[100];
  std::string_view result;
  ASSERT_OK(sequential->Read(5, &result, scratch));
  EXPECT_EQ(result, "hello");
  ASSERT_OK(random_access->Read(6, 5, &result, scratch));
  EXPECT_EQ(result, "world");
}

TEST(MemEnvTest, HandlesOutliveTheEnv) {
  std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(env->CreateDir("/d"));
  ASSERT_OK(WriteStringToFile(env.get(), "contents", "/d/f", /*sync=*/false));
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
  ASSERT_OK(writer->Close());
  lock.reset();
}

TEST(MemEnvTest, SeparateEnvsDoNotShareFiles) {
  const std::unique_ptr<Env> a = NewMemEnv();
  const std::unique_ptr<Env> b = NewMemEnv();
  ASSERT_OK(a->CreateDir("/d"));
  ASSERT_OK(WriteStringToFile(a.get(), "in a", "/d/f", /*sync=*/false));
  EXPECT_TRUE(a->FileExists("/d/f"));
  EXPECT_FALSE(b->FileExists("/d/f"));
  EXPECT_FALSE(b->FileExists("/d"));
  std::string contents;
  EXPECT_FALSE(ReadFileToString(b.get(), "/d/f", &contents).ok());
}

TEST(MemEnvTest, SeparateEnvsDoNotShareLocksOrClocks) {
  const std::unique_ptr<Env> a = NewMemEnv();
  const std::unique_ptr<Env> b = NewMemEnv();
  std::unique_ptr<FileLock> lock_a;
  std::unique_ptr<FileLock> lock_b;
  ASSERT_OK(a->LockFile("/LOCK", &lock_a));
  ASSERT_OK(b->LockFile("/LOCK", &lock_b)) << "a lock in one Env must not be seen by another";
  a->SleepForMicroseconds(10);
  EXPECT_EQ(a->NowMicros(), 10u);
  EXPECT_EQ(b->NowMicros(), 0u);
  ASSERT_OK(a->UnlockFile(std::move(lock_a)));
  ASSERT_OK(b->UnlockFile(std::move(lock_b)));
}

TEST(MemEnvTest, AFileNeedsNoDirectory) {
  // The namespace is flat: a path is a key, and the directory in it is not looked up. Directories
  // matter only to the calls that list, remove or sync them.
  const std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(WriteStringToFile(env.get(), "x", "/never/created/f", /*sync=*/false));
  EXPECT_EQ(ReadAll(env.get(), "/never/created/f"), "x");
  std::vector<std::string> children;
  EXPECT_FALSE(env->GetChildren("/never/created", &children).ok());
}

TEST(MemEnvTest, TrailingSlashOnADirectoryNameIsIgnored) {
  const std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(env->CreateDir("/d/"));
  ASSERT_OK(WriteStringToFile(env.get(), "x", "/d/f", /*sync=*/false));
  std::vector<std::string> children;
  ASSERT_OK(env->GetChildren("/d", &children));
  EXPECT_EQ(children, std::vector<std::string>{"f"});
  ASSERT_OK(env->GetChildren("/d/", &children));
  EXPECT_EQ(children, std::vector<std::string>{"f"});
  EXPECT_TRUE(env->FileExists("/d"));
  EXPECT_TRUE(env->FileExists("/d/"));
}

TEST(MemEnvTest, ADirectoryNameIsNotListedForAFileWithAPrefixOfIt) {
  // "/d" must not list "/dx/f": the prefix to match is "/d/", not "/d".
  const std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(env->CreateDir("/d"));
  ASSERT_OK(env->CreateDir("/dx"));
  ASSERT_OK(WriteStringToFile(env.get(), "1", "/d/a", /*sync=*/false));
  ASSERT_OK(WriteStringToFile(env.get(), "2", "/dx/b", /*sync=*/false));
  std::vector<std::string> children;
  ASSERT_OK(env->GetChildren("/d", &children));
  EXPECT_EQ(children, std::vector<std::string>{"a"});
}

TEST(MemEnvTest, CreateDirSucceedsWithAMissingParent) {
  const std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(env->CreateDir("/no/such/parent/child"));
  EXPECT_TRUE(env->FileExists("/no/such/parent/child"));
  EXPECT_FALSE(env->FileExists("/no/such/parent")) << "creating a directory creates only it";
}

TEST(MemEnvTest, TheRootDoesNotExistUntilItIsCreated) {
  const std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(WriteStringToFile(env.get(), "x", "/f", /*sync=*/false));
  std::vector<std::string> children;
  EXPECT_FALSE(env->GetChildren("/", &children).ok());
  ASSERT_OK(env->CreateDir("/"));
  ASSERT_OK(env->GetChildren("/", &children));
  EXPECT_EQ(children, std::vector<std::string>{"f"});
}

TEST(MemEnvTest, ALockStaysOnItsNameWhenTheFileIsRemovedOrRenamed) {
  const std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<FileLock> lock;
  ASSERT_OK(env->LockFile("/LOCK", &lock));
  std::unique_ptr<FileLock> other;

  ASSERT_OK(env->RenameFile("/LOCK", "/moved"));
  EXPECT_FALSE(env->LockFile("/LOCK", &other).ok()) << "the lock belongs to the name, not the file";
  ASSERT_OK(env->LockFile("/moved", &other)) << "the renamed file is not locked under its new name";
  ASSERT_OK(env->UnlockFile(std::move(other)));

  ASSERT_OK(env->RemoveFile("/moved"));
  EXPECT_FALSE(env->LockFile("/LOCK", &other).ok())
      << "removing the file does not release the lock";
  ASSERT_OK(env->UnlockFile(std::move(lock)));
  ASSERT_OK(env->LockFile("/LOCK", &other));
  ASSERT_OK(env->UnlockFile(std::move(other)));
}

TEST(MemEnvTest, RemoveFileOfADirectoryAndRenameOfADirectoryReportNoSuchFile) {
  const std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(env->CreateDir("/d"));
  const Status removed = env->RemoveFile("/d");
  EXPECT_TRUE(removed.IsIOError()) << removed.ToString();
  EXPECT_NE(removed.ToString().find("No such file"), std::string::npos) << removed.ToString();
  const Status renamed = env->RenameFile("/d", "/e");
  EXPECT_TRUE(renamed.IsIOError()) << renamed.ToString();
  EXPECT_NE(renamed.ToString().find("No such file"), std::string::npos) << renamed.ToString();
  EXPECT_TRUE(env->FileExists("/d")) << "the refused calls must leave the directory alone";
}

TEST(MemEnvTest, ConcurrentAppendsToDifferentFiles) {
  const std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(env->CreateDir("/d"));
  constexpr int kThreads = 4;
  constexpr int kAppends = 1000;
  const std::string record(100, 'x');

  std::vector<Status> results(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t] {
      std::unique_ptr<WritableFile> file;
      Status s = env->NewWritableFile("/d/file" + std::to_string(t), &file);
      for (int i = 0; s.ok() && i < kAppends; i++) {
        s = file->Append(record);
      }
      if (s.ok()) {
        s = file->Close();
      }
      results[static_cast<size_t>(t)] = s;
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  for (int t = 0; t < kThreads; t++) {
    const std::string path = "/d/file" + std::to_string(t);
    EXPECT_OK(results[static_cast<size_t>(t)]);
    uint64_t size = 0;
    ASSERT_OK(env->GetFileSize(path, &size));
    EXPECT_EQ(size, uint64_t{kAppends} * record.size()) << path;
  }
}

TEST(MemEnvTest, ConcurrentReadsOfOneFileAgree) {
  // RandomAccessFile promises that threads may share a handle. 8 threads read the same bytes
  // while a ninth appends elsewhere in the same file; the bytes they read must never change.
  const std::unique_ptr<Env> env = NewMemEnv();
  const std::string stable(4096, 'a');
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env->NewWritableFile("/f", &writer));
  ASSERT_OK(writer->Append(stable));
  std::unique_ptr<RandomAccessFile> reader;
  ASSERT_OK(env->NewRandomAccessFile("/f", &reader));

  std::atomic<int> mismatches{0};
  std::atomic<int> reads{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&] {
      char scratch[4096];
      for (int i = 0; i < 500; i++) {
        std::string_view result;
        if (!reader->Read(0, sizeof(scratch), &result, scratch).ok() || result != stable) {
          mismatches++;
        }
        reads++;
      }
    });
  }
  // No assertion while the readers are joinable: a failed ASSERT returns from the test, and
  // destroying a joinable std::thread terminates the process. The first failure is kept and
  // checked after the join.
  Status append_status;
  for (int i = 0; i < 500 && append_status.ok(); i++) {
    append_status = writer->Append("more");
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  ASSERT_OK(append_status);
  ASSERT_OK(writer->Close());
  EXPECT_EQ(reads.load(), 8 * 500) << "the readers did not all run";
  EXPECT_EQ(mismatches.load(), 0);
}

TEST(MemEnvTest, ConcurrentNamespaceOperationsAreSafe) {
  // Threads create, rename, list and remove files in one directory at the same time. What is
  // asserted is that nothing is lost or invented: each thread's names are its own, so after the
  // run the directory is empty, and every step on a thread's own files succeeded.
  const std::unique_ptr<Env> env = NewMemEnv();
  ASSERT_OK(env->CreateDir("/d"));
  constexpr int kThreads = 4;
  constexpr int kRounds = 200;
  std::atomic<int> failures{0};
  std::atomic<int> listings{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t] {
      const std::string a = "/d/a" + std::to_string(t);
      const std::string b = "/d/b" + std::to_string(t);
      for (int i = 0; i < kRounds; i++) {
        bool ok = WriteStringToFile(env.get(), "x", a, /*sync=*/false).ok();
        ok = ok && env->RenameFile(a, b).ok();
        std::vector<std::string> children;
        ok = ok && env->GetChildren("/d", &children).ok();
        listings++;
        ok = ok && env->FileExists(b);
        ok = ok && env->RemoveFile(b).ok();
        if (!ok) {
          failures++;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(listings.load(), kThreads * kRounds);
  EXPECT_EQ(failures.load(), 0);
  std::vector<std::string> children;
  ASSERT_OK(env->GetChildren("/d", &children));
  EXPECT_TRUE(children.empty());
}

TEST(MemEnvTest, OnlyOneThreadWinsTheLock) {
  const std::unique_ptr<Env> env = NewMemEnv();
  constexpr int kThreads = 8;
  std::atomic<int> winners{0};
  std::vector<std::unique_ptr<FileLock>> held(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t] {
      if (env->LockFile("/LOCK", &held[static_cast<size_t>(t)]).ok()) {
        winners++;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(winners.load(), 1);
}

TEST(MemEnvDeathTest, UnlockingALockFromAnotherEnvAborts) {
  const std::unique_ptr<Env> a = NewMemEnv();
  const std::unique_ptr<Env> b = NewMemEnv();
  std::unique_ptr<FileLock> lock;
  ASSERT_OK(a->LockFile("/LOCK", &lock));
  EXPECT_DEATH(static_cast<void>(b->UnlockFile(std::move(lock))), "this Env's LockFile");
}

}  // namespace
}  // namespace shale
