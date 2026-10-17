#ifndef SHALE_TESTS_DB_RECORDING_ENV_H_
#define SHALE_TESTS_DB_RECORDING_ENV_H_

// An Env for the tests of the order in which the engine changes a directory and makes the changes
// durable. FaultInjectionEnv keeps the changes of a directory in order, so a power loss that it
// simulates can never show a later change without an earlier one, and a filesystem that persists
// directory changes out of order is a case it cannot produce. What the engine does about that is a
// matter of the sequence of its calls, which this Env records.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "shale/env.h"
#include "shale/status.h"

namespace shale {

/// Passes every call to another Env and records the calls that change a directory, in the order
/// they were made: "rename <from> <to>", "remove <path>" and "syncdir <dir>".
class RecordingEnv final : public Env {
 public:
  /// Wraps `target`, which is not owned and must outlive this Env and every file it handed out.
  explicit RecordingEnv(Env* target) : target_(target) {}

  /// Returns the calls recorded so far, oldest first.
  std::vector<std::string> events() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return events_;
  }

  /// Forgets the calls recorded so far.
  void ClearEvents() {
    const std::lock_guard<std::mutex> lock(mu_);
    events_.clear();
  }

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    return target_->NewSequentialFile(fname, result);
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    return target_->NewRandomAccessFile(fname, result);
  }

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    return target_->NewWritableFile(fname, result);
  }

  bool FileExists(const std::string& fname) override { return target_->FileExists(fname); }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    return target_->GetChildren(dir, result);
  }

  Status RemoveFile(const std::string& fname) override {
    Record("remove " + fname);
    return target_->RemoveFile(fname);
  }

  Status CreateDir(const std::string& dirname) override { return target_->CreateDir(dirname); }
  Status RemoveDir(const std::string& dirname) override { return target_->RemoveDir(dirname); }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    return target_->GetFileSize(fname, size);
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    Record("rename " + src + " " + target);
    return target_->RenameFile(src, target);
  }

  Status SyncDir(const std::string& dirname) override {
    Record("syncdir " + dirname);
    return target_->SyncDir(dirname);
  }

  Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) override {
    return target_->LockFile(fname, lock);
  }

  Status UnlockFile(std::unique_ptr<FileLock> lock) override {
    return target_->UnlockFile(std::move(lock));
  }

  uint64_t NowMicros() override { return target_->NowMicros(); }
  void SleepForMicroseconds(uint64_t micros) override { target_->SleepForMicroseconds(micros); }

 private:
  void Record(std::string event) {
    const std::lock_guard<std::mutex> lock(mu_);
    events_.push_back(std::move(event));
  }

  Env* const target_;
  mutable std::mutex mu_;
  std::vector<std::string> events_;
};

}  // namespace shale

#endif  // SHALE_TESTS_DB_RECORDING_ENV_H_
