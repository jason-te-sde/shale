#ifndef SHALE_ENV_H_
#define SHALE_ENV_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "shale/status.h"

namespace shale {

/// A file read from front to back.
///
/// A handle is used by one thread at a time. It may pass from one thread to another between
/// calls, but two calls that overlap in time abort the process: the position they would both
/// move has no meaningful value afterwards.
class SequentialFile {
 public:
  /// Closes the file.
  virtual ~SequentialFile();

  /// Reads up to `n` bytes from the current position and moves the position past them.
  ///
  /// `*result` is set to the bytes read, which live in `scratch`; `scratch` must hold at least `n`
  /// bytes and must stay untouched for as long as `*result` is used. The result is shorter than
  /// `n` only when the end of the file was reached, and is empty at the end of the file. Both are
  /// success: a short read is not an error.
  /// @param n the most bytes to read.
  /// @param result receives the bytes read.
  /// @param scratch storage for the bytes, `n` bytes or more.
  virtual Status Read(size_t n, std::string_view* result, char* scratch) = 0;

  /// Moves the position forward by `n` bytes. Skipping past the end of the file is allowed; the
  /// next Read() then returns nothing.
  /// @param n how many bytes to skip.
  virtual Status Skip(uint64_t n) = 0;
};

/// A file read at arbitrary offsets. Any number of threads may use one handle at the same time.
class RandomAccessFile {
 public:
  /// Closes the file.
  virtual ~RandomAccessFile();

  /// Reads up to `n` bytes starting at byte `offset`.
  ///
  /// `*result` is set to the bytes read, which live in `scratch`; `scratch` must hold at least `n`
  /// bytes and must stay untouched for as long as `*result` is used. The result is shorter than
  /// `n` when the read runs into the end of the file, and empty when `offset` is at or past it;
  /// both are success.
  /// @param offset where in the file to start reading.
  /// @param n the most bytes to read.
  /// @param result receives the bytes read.
  /// @param scratch storage for the bytes, `n` bytes or more.
  virtual Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const = 0;
};

/// A file written from front to back.
///
/// Appended bytes may wait in memory, in the handle, until they are flushed. Flush() hands them to
/// the operating system, which makes them visible to readers but not durable; Sync() makes them
/// durable. A handle that is destroyed without Close() may discard the bytes that were never
/// flushed, as a crash would, so every writer ends with Close().
///
/// A handle is used by one thread at a time, as for SequentialFile. Any call after Close(),
/// including a second Close(), is a bug in the caller and aborts the process.
///
/// After Append(), Flush() or Sync() returns an error, the only call that is valid is Close(), and
/// what the file holds is unspecified: some of the appended bytes, in order, may have reached it
/// and the rest not. Retrying is not safe either. Some operating systems report success from a
/// second fsync() of data that the first one failed to write and then dropped, so a Sync() that
/// succeeds after a failed one proves nothing. A caller that needs the bytes writes them again to a
/// new file.
class WritableFile {
 public:
  /// Releases the handle. It does not flush: see the class description.
  virtual ~WritableFile();

  /// Adds `data` to the end of the file.
  /// @param data the bytes to append; may be empty.
  virtual Status Append(std::string_view data) = 0;

  /// Hands every byte appended so far to the operating system, so that other handles on the file
  /// see it. Does not make it durable.
  virtual Status Flush() = 0;

  /// Flushes, then makes every byte appended so far durable: it survives a crash of the process
  /// and of the machine. It does not make the file's directory entry durable; that is
  /// Env::SyncDir().
  virtual Status Sync() = 0;

  /// Flushes and closes the file. The handle must not be used again. The file is closed even when
  /// this returns an error.
  virtual Status Close() = 0;
};

/// A lock on a file, held by whoever holds this object. Destroying it releases the lock, as the
/// death of the process holding it would; Env::UnlockFile() is the explicit way and reports a
/// failure to release.
class FileLock {
 public:
  /// Releases the lock if it is still held.
  virtual ~FileLock();
};

/// Everything the engine asks of the outside world: files, directories, locks and time.
///
/// The engine reaches the filesystem and the clock only through an Env, so that a test can replace
/// both. Default() is the real filesystem; NewMemEnv() is an in-memory one with a virtual clock.
///
/// Every method that fails returns an IOError whose message names the path it failed on, a missing
/// file included. Methods may be called from any thread at the same time.
class Env {
 public:
  /// Does nothing; an Env is destroyed through this base class.
  virtual ~Env();

  /// Returns the Env for the real filesystem and the real clock. The same object is returned
  /// every time and lives until the program ends; the caller must not delete it.
  static Env* Default();

  /// Opens the existing file `fname` for sequential reading.
  /// @param fname the path of the file.
  /// @param result receives the handle on success, and is null on failure.
  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) = 0;

  /// Opens the existing file `fname` for random-access reading.
  /// @param fname the path of the file.
  /// @param result receives the handle on success, and is null on failure.
  virtual Status NewRandomAccessFile(const std::string& fname,
                                     std::unique_ptr<RandomAccessFile>* result) = 0;

  /// Opens `fname` for writing, creating it if it does not exist and emptying it if it does. The
  /// file is empty from this call on, before anything is appended.
  /// @param fname the path of the file.
  /// @param result receives the handle on success, and is null on failure.
  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) = 0;

  /// Returns true if `fname` exists, as a file or as a directory.
  /// @param fname the path to look for.
  virtual bool FileExists(const std::string& fname) = 0;

  /// Lists what is in the directory `dir`: its files and its subdirectories, by bare name, in no
  /// particular order. The entries for the directory itself and for its parent are not listed.
  /// @param dir the directory to list.
  /// @param result is cleared, then receives the names.
  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) = 0;

  /// Deletes the file `fname`. A handle that is already open on it keeps working.
  /// @param fname the path of the file.
  virtual Status RemoveFile(const std::string& fname) = 0;

  /// Creates the one directory `dirname`; missing parents are not created. It is not an error if
  /// `dirname` already exists as a directory.
  /// @param dirname the path of the directory.
  virtual Status CreateDir(const std::string& dirname) = 0;

  /// Deletes the directory `dirname`, which must be empty.
  /// @param dirname the path of the directory.
  virtual Status RemoveDir(const std::string& dirname) = 0;

  /// Stores the size of the file `fname` in bytes in `*size`. Bytes still waiting in a writer's
  /// buffer are not counted.
  /// @param fname the path of the file.
  /// @param size receives the size.
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;

  /// Renames `src` to `target`. If `target` exists it is replaced, atomically: a reader sees the
  /// old file or the new one, never neither.
  /// @param src the path of the file to rename.
  /// @param target the new path.
  virtual Status RenameFile(const std::string& src, const std::string& target) = 0;

  /// Makes the creations, removals and renames done in the directory `dirname` durable. A file
  /// that was synced but whose directory was not may be gone after a crash.
  /// @param dirname the directory to make durable.
  virtual Status SyncDir(const std::string& dirname) = 0;

  /// Takes an exclusive lock on `fname`, creating the file if it does not exist. It fails, without
  /// waiting, if anything else holds the lock: another process, or another handle in this one.
  /// @param fname the path of the file to lock.
  /// @param lock receives the lock on success, and is null on failure.
  virtual Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) = 0;

  /// Releases a lock taken by LockFile() on this Env. Passing a lock from another Env aborts.
  /// @param lock the lock to release; it is consumed even when releasing fails.
  virtual Status UnlockFile(std::unique_ptr<FileLock> lock) = 0;

  /// Returns the time in microseconds since an origin that depends on the Env. It never goes
  /// backwards. Only differences between two readings mean anything.
  virtual uint64_t NowMicros() = 0;

  /// Blocks the calling thread for at least `micros` microseconds of this Env's time.
  /// @param micros how long to sleep.
  virtual void SleepForMicroseconds(uint64_t micros) = 0;
};

/// Returns a new Env that keeps every file in memory and runs on a virtual clock. Nothing it does
/// reaches the real filesystem or the real clock, so it is fast, leaves nothing behind, and makes
/// a run reproducible. It is what the deterministic simulator and most tests run on.
///
/// - The clock starts at 0 and moves only when SleepForMicroseconds() is called, by exactly that
///   much. A sleep returns at once, without waiting.
/// - Files live in one flat namespace, keyed by the full path as a string. A path is not
///   normalised: `a//b` and `a/b` name different files. A file can be created at any path, whether
///   or not CreateDir() was ever called for its directory, but GetChildren(), RemoveDir() and
///   SyncDir() work only on directories that CreateDir() made. A trailing slash on a directory
///   name is ignored.
/// - Appended bytes are visible to every reader at once. Flush() and Sync() succeed and do
///   nothing, and nothing is ever lost: this Env does not model a crash. A wrapper that does is
///   built on top of it.
/// - Locks are per Env: two Envs from this function share no files, no locks and no clock.
/// - It is safe to use from any number of threads. An open handle keeps its file alive on its own,
///   so a file that was removed or replaced stays readable through the handles that were open on
///   it, as on POSIX, and a handle may outlive the Env that made it.
///
/// It differs from POSIX in these ways, besides what is said above:
/// - CreateDir() succeeds when the parent directory does not exist.
/// - The root `/` does not exist as a directory until CreateDir("/") is called, so
///   GetChildren("/") fails before that.
/// - A lock belongs to a path name, not to a file. It survives RemoveFile() and RenameFile() of the
///   locked file and stays on the name, where a POSIX lock would follow the file.
/// - RemoveFile() of a directory reports "No such file or directory", and RenameFile() moves files
///   only, not directories.
std::unique_ptr<Env> NewMemEnv();

}  // namespace shale

#endif  // SHALE_ENV_H_
