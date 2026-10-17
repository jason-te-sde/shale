#ifndef SHALE_TESTS_DB_HOOK_ENV_H_
#define SHALE_TESTS_DB_HOOK_ENV_H_

// An Env for the tests that need to see, or to hold up, or to fail, one particular call the engine
// makes: whether the log is synced at a switch, a listing of the directory that fails, a table that
// is being created while the test does something else. FaultInjectionEnv fails calls by count and
// probability; this one lets the test decide from the call itself.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shale/env.h"
#include "shale/status.h"

namespace shale {

/// Passes every call to another Env, after asking a hook about the calls it knows of. The hook gets
/// the name of the call, "new_writable", "sync" (of a writable file), "get_children" or "remove",
/// and the path it is about, and returns OK to let the call through or an error to make the call
/// return that, without making it. It runs on the thread of the call, without any lock of this
/// class held, so it may block, and may call into the database.
class HookEnv final : public Env {
 public:
  /// What a hook is.
  using Hook = std::function<Status(std::string_view call, const std::string& path)>;

  /// Wraps `target`, which is not owned and must outlive this Env and every file it handed out.
  explicit HookEnv(Env* target) : target_(target) {}

  /// Sets the hook, replacing the one there was; an empty one lets every call through.
  void SetHook(Hook hook) {
    const std::lock_guard<std::mutex> lock(mu_);
    hook_ = std::move(hook);
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
    const Status hooked = Ask("new_writable", fname);
    if (!hooked.ok()) {
      result->reset();
      return hooked;
    }
    std::unique_ptr<WritableFile> file;
    const Status s = target_->NewWritableFile(fname, &file);
    if (!s.ok()) {
      result->reset();
      return s;
    }
    *result = std::make_unique<HookedFile>(this, fname, std::move(file));
    return s;
  }

  bool FileExists(const std::string& fname) override { return target_->FileExists(fname); }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    const Status hooked = Ask("get_children", dir);
    if (!hooked.ok()) {
      result->clear();
      return hooked;
    }
    return target_->GetChildren(dir, result);
  }

  Status RemoveFile(const std::string& fname) override {
    const Status hooked = Ask("remove", fname);
    if (!hooked.ok()) {
      return hooked;
    }
    return target_->RemoveFile(fname);
  }

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
  // Asks the hook, if there is one, outside the lock: it may block.
  Status Ask(std::string_view call, const std::string& path) {
    Hook hook;
    {
      const std::lock_guard<std::mutex> lock(mu_);
      hook = hook_;
    }
    return hook ? hook(call, path) : Status::OK();
  }

  class HookedFile final : public WritableFile {
   public:
    HookedFile(HookEnv* env, std::string path, std::unique_ptr<WritableFile> file)
        : env_(env), path_(std::move(path)), file_(std::move(file)) {}

    Status Append(std::string_view data) override { return file_->Append(data); }
    Status Flush() override { return file_->Flush(); }

    Status Sync() override {
      const Status hooked = env_->Ask("sync", path_);
      if (!hooked.ok()) {
        return hooked;
      }
      return file_->Sync();
    }

    Status Close() override { return file_->Close(); }

   private:
    HookEnv* const env_;
    const std::string path_;
    const std::unique_ptr<WritableFile> file_;
  };

  Env* const target_;
  std::mutex mu_;
  Hook hook_;
};

}  // namespace shale

#endif  // SHALE_TESTS_DB_HOOK_ENV_H_
