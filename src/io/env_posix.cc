#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "io/exclusive_use.h"
#include "shale/env.h"
#include "shale/status.h"
#include "util/check.h"

namespace shale {
namespace {

// Appends of up to this many bytes cost a memcpy, not a system call. It is the size LevelDB and
// RocksDB use for the same reason: large enough that the write-ahead log's small records are
// batched, small enough that a few hundred open files do not pin megabytes.
constexpr size_t kWritableFileBufferSize = size_t{64} * 1024;

constexpr uint64_t kMaxFileOffset = static_cast<uint64_t>(std::numeric_limits<off_t>::max());

// Every error names the operation and the path, then the system's own description of the cause.
// std::system_category().message() is safe to call from any thread, which strerror() is not.
Status PosixError(std::string_view operation, const std::string& path, int error_number) {
  return Status::IOError(std::string(operation) + " " + path,
                         std::system_category().message(error_number));
}

// Makes the bytes written to `fd` durable.
//
// On Linux fdatasync flushes the data and the metadata needed to read it back, such as the file
// size, and skips the timestamps. On macOS neither fsync nor fdatasync asks the drive to empty its
// write cache, so a power failure can lose data that was reported as synced; F_FULLFSYNC does ask.
// It is not supported on every file system, such as some network mounts, and then plain fsync is
// the best there is. A directory has no data to separate from its metadata, so it takes fsync.
int SyncDescriptor(int fd, [[maybe_unused]] bool is_directory) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
  if (::fcntl(fd, F_FULLFSYNC) == 0) {
    return 0;
  }
  return ::fsync(fd);
#elif defined(__linux__)
  return is_directory ? ::fsync(fd) : ::fdatasync(fd);
#else
  return ::fsync(fd);
#endif
}

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string fname, int fd) : fname_(std::move(fname)), fd_(fd) {}

  PosixSequentialFile(const PosixSequentialFile&) = delete;
  PosixSequentialFile& operator=(const PosixSequentialFile&) = delete;

  ~PosixSequentialFile() override { ::close(fd_); }

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    size_t total = 0;
    // One read() may return less than asked for without being at the end (a signal, a pipe), so a
    // short result is only believed once read() has returned 0.
    while (total < n) {
      const ssize_t got = ::read(fd_, scratch + total, n - total);
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        *result = std::string_view();
        return PosixError("read", fname_, errno);
      }
      if (got == 0) {
        break;
      }
      total += static_cast<size_t>(got);
    }
    *result = std::string_view(scratch, total);
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    if (n > kMaxFileOffset) {
      return Status::InvalidArgument("skip is larger than any file offset: " + fname_);
    }
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) == static_cast<off_t>(-1)) {
      return PosixError("skip in", fname_, errno);
    }
    return Status::OK();
  }

 private:
  const std::string fname_;
  const int fd_;
  ExclusiveUse use_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string fname, int fd) : fname_(std::move(fname)), fd_(fd) {}

  PosixRandomAccessFile(const PosixRandomAccessFile&) = delete;
  PosixRandomAccessFile& operator=(const PosixRandomAccessFile&) = delete;

  ~PosixRandomAccessFile() override { ::close(fd_); }

  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    if (offset > kMaxFileOffset) {
      // No file is that long, so the offset is past the end of this one.
      *result = std::string_view();
      return Status::OK();
    }
    size_t total = 0;
    // pread() takes the offset as an argument and leaves the descriptor's position alone, which
    // is what lets any number of threads share one handle without a lock.
    while (total < n) {
      const off_t position = static_cast<off_t>(offset + total);
      const ssize_t got = ::pread(fd_, scratch + total, n - total, position);
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        *result = std::string_view();
        return PosixError("read", fname_, errno);
      }
      if (got == 0) {
        break;
      }
      total += static_cast<size_t>(got);
    }
    *result = std::string_view(scratch, total);
    return Status::OK();
  }

 private:
  const std::string fname_;
  const int fd_;
};

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string fname, int fd) : fname_(std::move(fname)), fd_(fd) {
    buffer_.reserve(kWritableFileBufferSize);
  }

  PosixWritableFile(const PosixWritableFile&) = delete;
  PosixWritableFile& operator=(const PosixWritableFile&) = delete;

  // Whatever is still in the buffer is dropped, as the class description in env.h says.
  ~PosixWritableFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  Status Append(std::string_view data) override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Append");
    if (data.size() <= kWritableFileBufferSize - buffer_.size()) {
      buffer_.append(data);
      return Status::OK();
    }
    // It does not fit. What is buffered goes out first, to keep the bytes in order, and then the
    // new data either starts a fresh buffer or, being as large as a buffer itself, is not worth
    // copying and goes straight to the file.
    Status s = FlushBuffer();
    if (!s.ok()) {
      return s;
    }
    if (data.size() < kWritableFileBufferSize) {
      buffer_.append(data);
      return Status::OK();
    }
    size_t written = 0;
    return WriteAll(data.data(), data.size(), &written);
  }

  Status Flush() override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Flush");
    return FlushBuffer();
  }

  Status Sync() override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Sync");
    Status s = FlushBuffer();
    if (!s.ok()) {
      return s;
    }
    if (SyncDescriptor(fd_, /*is_directory=*/false) != 0) {
      return PosixError("sync", fname_, errno);
    }
    return Status::OK();
  }

  Status Close() override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Close");
    Status s = FlushBuffer();
    // The descriptor is released whether or not the flush worked: there is no second try, and an
    // open descriptor is a leak.
    const int close_result = ::close(fd_);
    const int close_errno = errno;
    fd_ = -1;
    if (close_result != 0 && s.ok()) {
      s = PosixError("close", fname_, close_errno);
    }
    return s;
  }

 private:
  // Using a handle after Close() is a bug in the caller, not something that happens in a correct
  // program, and carrying on would write to a descriptor that may by now belong to another file.
  void CheckOpen(std::string_view operation) const {
    SHALE_CHECK(fd_ >= 0, std::string(operation) + " after Close on " + fname_);
  }

  // Writes `size` bytes at `data` to the file, however many write() calls it takes. `*written` is
  // how many reached the file, which is less than `size` only when an error is returned.
  Status WriteAll(const char* data, size_t size, size_t* written) {
    *written = 0;
    while (*written < size) {
      const ssize_t put = ::write(fd_, data + *written, size - *written);
      if (put < 0) {
        if (errno == EINTR) {
          continue;
        }
        return PosixError("write", fname_, errno);
      }
      *written += static_cast<size_t>(put);
    }
    return Status::OK();
  }

  Status FlushBuffer() {
    size_t written = 0;
    const Status s = WriteAll(buffer_.data(), buffer_.size(), &written);
    // After a partial write the bytes that did reach the file leave the buffer, so that a retry
    // does not write them twice.
    buffer_.erase(0, written);
    return s;
  }

  const std::string fname_;
  int fd_;
  std::string buffer_;
  ExclusiveUse use_;
};

class PosixFileLock final : public FileLock {
 public:
  PosixFileLock(std::string fname, int fd) : fname_(std::move(fname)), fd_(fd) {}

  PosixFileLock(const PosixFileLock&) = delete;
  PosixFileLock& operator=(const PosixFileLock&) = delete;

  // Closing the descriptor drops its flock, so a lock that is never explicitly released goes away
  // with its handle, as it would with the process.
  ~PosixFileLock() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  // Releases the lock and closes the file, and reports the first thing that went wrong.
  Status Release() {
    Status s;
    if (::flock(fd_, LOCK_UN) != 0) {
      s = PosixError("unlock", fname_, errno);
    }
    if (::close(fd_) != 0 && s.ok()) {
      s = PosixError("close lock file", fname_, errno);
    }
    fd_ = -1;
    return s;
  }

 private:
  const std::string fname_;
  int fd_;
};

class PosixEnv final : public Env {
 public:
  PosixEnv() = default;

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    result->reset();
    const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return PosixError("open", fname, errno);
    }
    *result = std::make_unique<PosixSequentialFile>(fname, fd);
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    result->reset();
    const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return PosixError("open", fname, errno);
    }
    *result = std::make_unique<PosixRandomAccessFile>(fname, fd);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    result->reset();
    const int fd = ::open(fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
      return PosixError("open", fname, errno);
    }
    *result = std::make_unique<PosixWritableFile>(fname, fd);
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override { return ::access(fname.c_str(), F_OK) == 0; }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();
    DIR* const handle = ::opendir(dir.c_str());
    if (handle == nullptr) {
      return PosixError("list directory", dir, errno);
    }
    Status s;
    for (;;) {
      // readdir() returns null both at the end and on an error, and says which only through errno.
      errno = 0;
      const struct dirent* const entry = ::readdir(handle);
      if (entry == nullptr) {
        if (errno != 0) {
          s = PosixError("list directory", dir, errno);
        }
        break;
      }
      const std::string_view name = entry->d_name;
      if (name != "." && name != "..") {
        result->emplace_back(name);
      }
    }
    ::closedir(handle);
    if (!s.ok()) {
      result->clear();
    }
    return s;
  }

  Status RemoveFile(const std::string& fname) override {
    if (::unlink(fname.c_str()) != 0) {
      return PosixError("remove", fname, errno);
    }
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override {
    if (::mkdir(dirname.c_str(), 0755) == 0) {
      return Status::OK();
    }
    const int mkdir_errno = errno;
    // "Already there" is fine if what is there is a directory. A file in the way is an error.
    struct stat info;
    if (mkdir_errno == EEXIST && ::stat(dirname.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
      return Status::OK();
    }
    return PosixError("create directory", dirname, mkdir_errno);
  }

  Status RemoveDir(const std::string& dirname) override {
    if (::rmdir(dirname.c_str()) != 0) {
      return PosixError("remove directory", dirname, errno);
    }
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    struct stat info;
    if (::stat(fname.c_str(), &info) != 0) {
      *size = 0;
      return PosixError("get size of", fname, errno);
    }
    *size = static_cast<uint64_t>(info.st_size);
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    if (::rename(src.c_str(), target.c_str()) != 0) {
      return PosixError("rename", src + " to " + target, errno);
    }
    return Status::OK();
  }

  Status SyncDir(const std::string& dirname) override {
    const int fd = ::open(dirname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return PosixError("open directory", dirname, errno);
    }
    Status s;
    if (SyncDescriptor(fd, /*is_directory=*/true) != 0) {
      s = PosixError("sync directory", dirname, errno);
    }
    ::close(fd);
    return s;
  }

  Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) override {
    lock->reset();
    const int fd = ::open(fname.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      return PosixError("open lock file", fname, errno);
    }
    // flock() ties the lock to the open file description, not to the process. A second open() of
    // the same file in this very process gets its own description, and so conflicts, which is what
    // keeps two databases in one process from sharing a directory. fcntl() locks would not: they
    // belong to the process, and a second request from it succeeds.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int lock_errno = errno;
      ::close(fd);
      if (lock_errno == EWOULDBLOCK) {
        return Status::IOError("lock " + fname, "already held by another process or handle");
      }
      return PosixError("lock", fname, lock_errno);
    }
    *lock = std::make_unique<PosixFileLock>(fname, fd);
    return Status::OK();
  }

  Status UnlockFile(std::unique_ptr<FileLock> lock) override {
    auto* const posix_lock = dynamic_cast<PosixFileLock*>(lock.get());
    SHALE_CHECK(posix_lock != nullptr, "UnlockFile needs a lock that this Env's LockFile returned");
    return posix_lock->Release();
  }

  uint64_t NowMicros() override {
    // steady_clock never goes backwards, which system_clock does when the date is changed, and
    // the engine measures intervals with this.
    const auto since_origin = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(since_origin).count());
  }

  void SleepForMicroseconds(uint64_t micros) override {
    std::this_thread::sleep_for(
        std::chrono::microseconds(static_cast<std::chrono::microseconds::rep>(micros)));
  }
};

}  // namespace

Env* Env::Default() {
  // The one object of static storage duration the engine has. It holds no state, so there is
  // nothing to race on and nothing to order against other destructors at exit.
  static PosixEnv env;
  return &env;
}

}  // namespace shale
