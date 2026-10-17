#ifndef SHALE_TESTS_DB_HANDLE_CHECK_ENV_H_
#define SHALE_TESTS_DB_HANDLE_CHECK_ENV_H_

// An Env for the tests of code that opens files and must close them on every path, error paths
// included. The Env's contract is that a writable file is released by Close(), and a handle that is
// dropped instead may lose what it holds and, on a real filesystem, leaks a descriptor. Nothing in
// the other Envs notices that, so a path that forgets would pass every test.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shale/env.h"
#include "shale/status.h"

namespace shale {

/// Passes every call to another Env and counts the writable files it hands out that are destroyed
/// without Close() having been called on them.
class HandleCheckEnv final : public Env {
 public:
  /// Wraps `target`, which is not owned and must outlive this Env and every file it handed out.
  explicit HandleCheckEnv(Env* target) : target_(target) {}

  /// Returns how many writable files were destroyed without Close(), so far.
  int leaked() const { return leaked_.load(); }

  /// Returns how many writable files were opened, so far.
  int opened() const { return opened_.load(); }

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    return target_->NewSequentialFile(fname, result);
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    return target_->NewRandomAccessFile(fname, result);
  }

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    std::unique_ptr<WritableFile> file;
    const Status s = target_->NewWritableFile(fname, &file);
    if (!s.ok()) {
      result->reset();
      return s;
    }
    opened_.fetch_add(1);
    *result = std::make_unique<CheckedFile>(std::move(file), &leaked_);
    return s;
  }

  bool FileExists(const std::string& fname) override { return target_->FileExists(fname); }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    return target_->GetChildren(dir, result);
  }

  Status RemoveFile(const std::string& fname) override { return target_->RemoveFile(fname); }
  Status CreateDir(const std::string& dirname) override { return target_->CreateDir(dirname); }
  Status RemoveDir(const std::string& dirname) override { return target_->RemoveDir(dirname); }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    return target_->GetFileSize(fname, size);
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    return target_->RenameFile(src, target);
  }

  Status SyncDir(const std::string& dirname) override { return target_->SyncDir(dirname); }

  Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) override {
    return target_->LockFile(fname, lock);
  }

  Status UnlockFile(std::unique_ptr<FileLock> lock) override {
    return target_->UnlockFile(std::move(lock));
  }

  uint64_t NowMicros() override { return target_->NowMicros(); }
  void SleepForMicroseconds(uint64_t micros) override { target_->SleepForMicroseconds(micros); }

 private:
  class CheckedFile final : public WritableFile {
   public:
    CheckedFile(std::unique_ptr<WritableFile> file, std::atomic<int>* leaked)
        : file_(std::move(file)), leaked_(leaked) {}

    ~CheckedFile() override {
      if (!closed_) {
        leaked_->fetch_add(1);
      }
    }

    Status Append(std::string_view data) override { return file_->Append(data); }
    Status Flush() override { return file_->Flush(); }
    Status Sync() override { return file_->Sync(); }

    Status Close() override {
      closed_ = true;
      return file_->Close();
    }

   private:
    const std::unique_ptr<WritableFile> file_;
    std::atomic<int>* const leaked_;
    bool closed_ = false;
  };

  Env* const target_;
  std::atomic<int> opened_{0};
  std::atomic<int> leaked_{0};
};

}  // namespace shale

#endif  // SHALE_TESTS_DB_HANDLE_CHECK_ENV_H_
