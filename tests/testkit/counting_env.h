#ifndef SHALE_TESTKIT_COUNTING_ENV_H_
#define SHALE_TESTKIT_COUNTING_ENV_H_

// An Env that counts the reads made from its random access files, for tests of how many reads, and
// how many bytes, an operation costs. It lives in the shale_testkit library, which only test
// targets link.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shale/env.h"
#include "shale/status.h"

namespace shale {

/// Forwards every call to the Env it was given, and counts the Read() calls of the files that
/// NewRandomAccessFile() returns. The counts are atomic, so reads from any number of threads are
/// counted. A file keeps counting after the Env that made it is gone only if the counters are:
/// the Env must outlive its files.
class CountingEnv final : public Env {
 public:
  /// @param target the Env to forward to. It is not owned and must outlive this one.
  explicit CountingEnv(Env* target) : target_(target) {}

  /// Returns the number of Read() calls made so far, or since ResetCounts().
  uint64_t random_reads() const { return reads_.load(std::memory_order_relaxed); }
  /// Returns the bytes those calls returned.
  uint64_t random_read_bytes() const { return bytes_.load(std::memory_order_relaxed); }
  /// Returns the largest offset plus returned size over those calls, which is how far into any file
  /// a read has reached.
  uint64_t max_read_end() const { return max_end_.load(std::memory_order_relaxed); }

  /// Sets the three counts to zero.
  void ResetCounts() {
    reads_.store(0, std::memory_order_relaxed);
    bytes_.store(0, std::memory_order_relaxed);
    max_end_.store(0, std::memory_order_relaxed);
  }

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    return target_->NewSequentialFile(fname, result);
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    std::unique_ptr<RandomAccessFile> inner;
    Status s = target_->NewRandomAccessFile(fname, &inner);
    if (s.ok()) {
      *result = std::make_unique<File>(this, std::move(inner));
    }
    return s;
  }

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    return target_->NewWritableFile(fname, result);
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
  class File final : public RandomAccessFile {
   public:
    File(CountingEnv* env, std::unique_ptr<RandomAccessFile> inner)
        : env_(env), inner_(std::move(inner)) {}

    Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
      Status s = inner_->Read(offset, n, result, scratch);
      env_->reads_.fetch_add(1, std::memory_order_relaxed);
      if (s.ok()) {
        env_->bytes_.fetch_add(result->size(), std::memory_order_relaxed);
        const uint64_t end = offset + result->size();
        uint64_t seen = env_->max_end_.load(std::memory_order_relaxed);
        while (end > seen &&
               !env_->max_end_.compare_exchange_weak(seen, end, std::memory_order_relaxed)) {
        }
      }
      return s;
    }

   private:
    CountingEnv* const env_;
    const std::unique_ptr<RandomAccessFile> inner_;
  };

  Env* const target_;
  std::atomic<uint64_t> reads_{0};
  std::atomic<uint64_t> bytes_{0};
  std::atomic<uint64_t> max_end_{0};
};

}  // namespace shale

#endif  // SHALE_TESTKIT_COUNTING_ENV_H_
