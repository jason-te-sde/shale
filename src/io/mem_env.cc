#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/exclusive_use.h"
#include "shale/env.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/mutex.h"
#include "util/thread_annotations.h"

namespace shale {
namespace {

// The contents of one file, standing for a POSIX inode: the directory entry and every open handle
// share it, so a handle keeps working after its file is removed or replaced.
//
// It carries its own lock rather than borrowing the Env's. A handle then needs nothing from the
// Env after it is opened, which is what lets one outlive it, and appending to one file does not
// wait for a lookup in another. Lock order, where both are needed: the namespace first, then a
// file. A FileState never reaches back into the namespace, so the order cannot be inverted.
class FileState {
 public:
  uint64_t Size() const {
    MutexLock lock(&mu_);
    return data_.size();
  }

  void Append(std::string_view data) {
    MutexLock lock(&mu_);
    data_.append(data);
  }

  void Truncate() {
    MutexLock lock(&mu_);
    data_.clear();
  }

  // Copies up to `n` bytes starting at `offset` into `scratch` and returns how many it copied: none
  // at or past the end. It copies rather than pointing into the file, because an append may
  // reallocate the string the moment this returns.
  size_t Read(uint64_t offset, size_t n, char* scratch) const {
    MutexLock lock(&mu_);
    if (offset >= data_.size()) {
      return 0;
    }
    const size_t available = data_.size() - static_cast<size_t>(offset);
    const size_t count = std::min(n, available);
    std::memcpy(scratch, data_.data() + static_cast<size_t>(offset), count);
    return count;
  }

 private:
  mutable Mutex mu_;
  std::string data_ SHALE_GUARDED_BY(mu_);
};

Status NoSuchEntry(std::string_view operation, const std::string& path) {
  return Status::IOError(std::string(operation) + " " + path, "No such file or directory");
}

class MemSequentialFile final : public SequentialFile {
 public:
  MemSequentialFile(std::string fname, std::shared_ptr<FileState> file)
      : fname_(std::move(fname)), file_(std::move(file)) {}

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    const size_t count = file_->Read(position_, n, scratch);
    position_ += count;
    *result = std::string_view(scratch, count);
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    // Skipping past the end is allowed, so the position may exceed the size; saturate rather than
    // wrap if someone skips by the largest possible amount.
    position_ = n > std::numeric_limits<uint64_t>::max() - position_
                    ? std::numeric_limits<uint64_t>::max()
                    : position_ + n;
    return Status::OK();
  }

 private:
  const std::string fname_;
  const std::shared_ptr<FileState> file_;
  uint64_t position_ = 0;
  ExclusiveUse use_;
};

class MemRandomAccessFile final : public RandomAccessFile {
 public:
  explicit MemRandomAccessFile(std::shared_ptr<FileState> file) : file_(std::move(file)) {}

  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    *result = std::string_view(scratch, file_->Read(offset, n, scratch));
    return Status::OK();
  }

 private:
  const std::shared_ptr<FileState> file_;
};

class MemWritableFile final : public WritableFile {
 public:
  MemWritableFile(std::string fname, std::shared_ptr<FileState> file)
      : fname_(std::move(fname)), file_(std::move(file)) {}

  Status Append(std::string_view data) override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Append");
    file_->Append(data);
    return Status::OK();
  }

  // There is no buffer between this handle and the file, and nothing to make durable.
  Status Flush() override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Flush");
    return Status::OK();
  }

  Status Sync() override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Sync");
    return Status::OK();
  }

  Status Close() override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Close");
    closed_ = true;
    return Status::OK();
  }

 private:
  // The same rule as the POSIX Env's, so that code which breaks it fails here, in a test, and not
  // only on a real disk.
  void CheckOpen(std::string_view operation) const {
    SHALE_CHECK(!closed_, std::string(operation) + " after Close on " + fname_);
  }

  const std::string fname_;
  const std::shared_ptr<FileState> file_;
  bool closed_ = false;
  ExclusiveUse use_;
};

// Everything one MemEnv knows apart from the file contents: the names, the directories, the locks
// and the clock. It is shared with every FileLock, because a lock that is dropped without being
// released must release itself, and that may happen after the Env is gone.
struct Namespace {
  Mutex mu;
  std::map<std::string, std::shared_ptr<FileState>> files SHALE_GUARDED_BY(mu);
  std::set<std::string> dirs SHALE_GUARDED_BY(mu);
  std::set<std::string> locks SHALE_GUARDED_BY(mu);
  uint64_t now_micros SHALE_GUARDED_BY(mu) = 0;

  // Returns true if any file or directory lies below `prefix`, which ends in a slash.
  bool HasEntriesBelow(const std::string& prefix) const SHALE_REQUIRES(mu) {
    const auto file = files.lower_bound(prefix);
    if (file != files.end() && file->first.starts_with(prefix)) {
      return true;
    }
    const auto dir = dirs.lower_bound(prefix);
    return dir != dirs.end() && dir->starts_with(prefix);
  }

  void ReleaseLock(const std::string& name) {
    MutexLock lock(&mu);
    locks.erase(name);
  }
};

class MemFileLock final : public FileLock {
 public:
  MemFileLock(std::shared_ptr<Namespace> ns, std::string name)
      : ns_(std::move(ns)), name_(std::move(name)) {}

  MemFileLock(const MemFileLock&) = delete;
  MemFileLock& operator=(const MemFileLock&) = delete;

  ~MemFileLock() override { ns_->ReleaseLock(name_); }

  bool BelongsTo(const Namespace* ns) const { return ns_.get() == ns; }

 private:
  const std::shared_ptr<Namespace> ns_;
  const std::string name_;
};

// A directory name without its trailing slashes, so that "/d" and "/d/" are the same directory.
// A name that is only slashes is left as the root.
std::string NormalizeDir(std::string name) {
  while (name.size() > 1 && name.back() == '/') {
    name.pop_back();
  }
  return name;
}

// The prefix every entry directly or indirectly below `dir` starts with.
std::string PrefixOf(const std::string& dir) { return dir == "/" ? dir : dir + "/"; }

class MemEnv final : public Env {
 public:
  MemEnv() : ns_(std::make_shared<Namespace>()) {}

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    result->reset();
    std::shared_ptr<FileState> file;
    Status s = Find(fname, "open", &file);
    if (!s.ok()) {
      return s;
    }
    *result = std::make_unique<MemSequentialFile>(fname, std::move(file));
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    result->reset();
    std::shared_ptr<FileState> file;
    Status s = Find(fname, "open", &file);
    if (!s.ok()) {
      return s;
    }
    *result = std::make_unique<MemRandomAccessFile>(std::move(file));
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    result->reset();
    std::shared_ptr<FileState> file;
    {
      Namespace& ns = *ns_;
      MutexLock lock(&ns.mu);
      if (ns.dirs.count(NormalizeDir(fname)) != 0) {
        return Status::IOError("open " + fname, "Is a directory");
      }
      auto [entry, created] = ns.files.try_emplace(fname);
      if (created) {
        entry->second = std::make_shared<FileState>();
      } else {
        // The same file, emptied, as O_TRUNC does: handles already open on it see it empty.
        entry->second->Truncate();
      }
      file = entry->second;
    }
    *result = std::make_unique<MemWritableFile>(fname, std::move(file));
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    Namespace& ns = *ns_;
    MutexLock lock(&ns.mu);
    return ns.files.count(fname) != 0 || ns.dirs.count(NormalizeDir(fname)) != 0;
  }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();
    const std::string normalized = NormalizeDir(dir);
    const std::string prefix = PrefixOf(normalized);
    Namespace& ns = *ns_;
    MutexLock lock(&ns.mu);
    if (ns.dirs.count(normalized) == 0) {
      return NoSuchEntry("list directory", dir);
    }
    // The map is ordered by path, so everything below the directory is one contiguous run that
    // starts at the prefix. A name with a slash left in it is deeper than a child, and is not one.
    for (auto it = ns.files.lower_bound(prefix); it != ns.files.end(); ++it) {
      if (!it->first.starts_with(prefix)) {
        break;
      }
      AddIfChild(it->first, prefix, result);
    }
    for (auto it = ns.dirs.lower_bound(prefix); it != ns.dirs.end(); ++it) {
      if (!it->starts_with(prefix)) {
        break;
      }
      AddIfChild(*it, prefix, result);
    }
    return Status::OK();
  }

  Status RemoveFile(const std::string& fname) override {
    Namespace& ns = *ns_;
    MutexLock lock(&ns.mu);
    // Erasing the entry drops the namespace's reference only; open handles keep the contents.
    if (ns.files.erase(fname) == 0) {
      return NoSuchEntry("remove", fname);
    }
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override {
    Namespace& ns = *ns_;
    MutexLock lock(&ns.mu);
    if (ns.files.count(dirname) != 0) {
      return Status::IOError("create directory " + dirname, "File exists");
    }
    ns.dirs.insert(NormalizeDir(dirname));
    return Status::OK();
  }

  Status RemoveDir(const std::string& dirname) override {
    const std::string normalized = NormalizeDir(dirname);
    Namespace& ns = *ns_;
    MutexLock lock(&ns.mu);
    if (ns.dirs.count(normalized) == 0) {
      return NoSuchEntry("remove directory", dirname);
    }
    if (ns.HasEntriesBelow(PrefixOf(normalized))) {
      return Status::IOError("remove directory " + dirname, "Directory not empty");
    }
    ns.dirs.erase(normalized);
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    *size = 0;
    std::shared_ptr<FileState> file;
    Status s = Find(fname, "get size of", &file);
    if (!s.ok()) {
      return s;
    }
    *size = file->Size();
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    const std::string what = src + " to " + target;
    Namespace& ns = *ns_;
    MutexLock lock(&ns.mu);
    const auto source = ns.files.find(src);
    if (source == ns.files.end()) {
      return NoSuchEntry("rename", what);
    }
    if (ns.dirs.count(NormalizeDir(target)) != 0) {
      return Status::IOError("rename " + what, "Is a directory");
    }
    if (src != target) {
      // Whatever `target` held is dropped from the namespace, and lives on only in the handles
      // that were open on it, as an unlinked file does.
      ns.files[target] = std::move(source->second);
      ns.files.erase(source);
    }
    return Status::OK();
  }

  Status SyncDir(const std::string& dirname) override {
    Namespace& ns = *ns_;
    MutexLock lock(&ns.mu);
    if (ns.dirs.count(NormalizeDir(dirname)) == 0) {
      return NoSuchEntry("open directory", dirname);
    }
    return Status::OK();
  }

  Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) override {
    lock->reset();
    {
      Namespace& ns = *ns_;
      MutexLock guard(&ns.mu);
      if (ns.dirs.count(NormalizeDir(fname)) != 0) {
        return Status::IOError("open lock file " + fname, "Is a directory");
      }
      // Like open(O_CREAT) on POSIX, taking the lock creates the file if it is missing.
      auto [entry, created] = ns.files.try_emplace(fname);
      if (created) {
        entry->second = std::make_shared<FileState>();
      }
      if (!ns.locks.insert(fname).second) {
        return Status::IOError("lock " + fname, "already held by another process or handle");
      }
    }
    *lock = std::make_unique<MemFileLock>(ns_, fname);
    return Status::OK();
  }

  Status UnlockFile(std::unique_ptr<FileLock> lock) override {
    auto* const mem_lock = dynamic_cast<MemFileLock*>(lock.get());
    SHALE_CHECK(mem_lock != nullptr && mem_lock->BelongsTo(ns_.get()),
                "UnlockFile needs a lock that this Env's LockFile returned");
    // Destroying the lock releases it. The namespace mutex is not held here: the destructor takes
    // it, and the mutex is not recursive.
    lock.reset();
    return Status::OK();
  }

  uint64_t NowMicros() override {
    Namespace& ns = *ns_;
    MutexLock lock(&ns.mu);
    return ns.now_micros;
  }

  void SleepForMicroseconds(uint64_t micros) override {
    Namespace& ns = *ns_;
    MutexLock lock(&ns.mu);
    // Saturate instead of wrapping: a clock that goes backwards breaks what NowMicros() promises.
    ns.now_micros = micros > std::numeric_limits<uint64_t>::max() - ns.now_micros
                        ? std::numeric_limits<uint64_t>::max()
                        : ns.now_micros + micros;
  }

 private:
  // Looks up the file `fname` and returns the shared contents in `*file`, or an error that says
  // what `operation` could not do.
  Status Find(const std::string& fname, std::string_view operation,
              std::shared_ptr<FileState>* file) {
    Namespace& ns = *ns_;
    MutexLock lock(&ns.mu);
    const auto it = ns.files.find(fname);
    if (it == ns.files.end()) {
      return ns.dirs.count(NormalizeDir(fname)) != 0
                 ? Status::IOError(std::string(operation) + " " + fname, "Is a directory")
                 : NoSuchEntry(operation, fname);
    }
    *file = it->second;
    return Status::OK();
  }

  // Adds the part of `path` after `prefix` to `*names` if it is a single name.
  static void AddIfChild(const std::string& path, const std::string& prefix,
                         std::vector<std::string>* names) {
    const std::string_view rest = std::string_view(path).substr(prefix.size());
    if (!rest.empty() && rest.find('/') == std::string_view::npos) {
      names->emplace_back(rest);
    }
  }

  const std::shared_ptr<Namespace> ns_;
};

}  // namespace

std::unique_ptr<Env> NewMemEnv() { return std::make_unique<MemEnv>(); }

}  // namespace shale
