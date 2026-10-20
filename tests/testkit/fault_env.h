#ifndef SHALE_TESTKIT_FAULT_ENV_H_
#define SHALE_TESTKIT_FAULT_ENV_H_

// An in-memory Env for the tests that crash the engine. It lives in the shale_testkit library,
// which only test targets link.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "shale/env.h"
#include "shale/status.h"

namespace shale {

namespace internal {
class FaultState;
}  // namespace internal

/// An in-memory Env that can lose power, kill the process, fail calls and flip bits in what it
/// reads, all of it driven by one seed so that a failing run can be replayed exactly.
///
/// Without a fault configured it behaves like NewMemEnv() as far as the Env's contract goes: the
/// same files, the same errors, a virtual clock that moves only when someone sleeps, and any number
/// of threads. What it adds is the gap between what a program has written and what a disk has
/// kept, in three layers.
///
/// Bytes:
/// - A writable handle buffers what is appended to it, without a limit. Flush(), Sync() and Close()
///   hand the buffer to the file, where readers see it; nothing else does. A handle that is dropped
///   without Close(), or that a crash invalidates, takes its buffer with it.
/// - What was handed over is what the operating system holds, and survives the death of the
///   process. A file also has a count of leading bytes that Sync() made durable. A power loss keeps
///   those and a prefix of the rest, and what it keeps is durable from then on.
/// - Or, for a file that was appended to since its last sync, a power loss keeps the synced bytes
///   followed by as many zero bytes as were handed over after them, one time in four: the length
///   of the file reached the disk and its data did not. XFS and ext4 leave this after a power loss,
///   and a process killed on macOS 14 has been seen to leave it. The synced bytes are never zeroed.
/// - A Sync() that fails marks its file. From then on every Sync() of it reports success and makes
///   nothing durable, whatever it is asked: the Env's documentation says a Sync() that succeeds
///   after a failed one proves nothing, and a caller that trusts it loses its bytes here, where a
///   test can see it. Opening the file for write again gives it a fresh start.
///
/// Names:
/// - The namespace has two views: the live one, which every call sees, and the durable one, which
///   is what a power loss starts from. Each change to a directory (a creation, a removal, a
///   rename, an open for write that empties an existing file) is kept in an ordered log for that
///   directory until SyncDir() makes the whole log durable and clears it. The parent of a path is
///   everything before its last '/', so `/d/f` is under `/d`, `/d/sub/g` is under `/d/sub` and
///   `/f` is under `/`.
/// - A power loss keeps, for each directory, a prefix of its log, from none of it to all of it:
///   never a change without the ones made before it in that directory. A rename inside one
///   directory is one entry, kept whole or not at all. A rename across directories is two entries,
///   one in each log, and each half may be kept without the other. Opening a file for write is a
///   new empty file under the name, so a power loss that keeps none of it brings the old file back.
///
/// Crashes:
/// - Every crash invalidates every handle and every lock taken before it. The restarted process
///   opens its own. A process crash releases the locks, as the death of a process does.
/// - A process crash keeps every flushed byte and every directory change, because the machine
///   survives; it makes nothing durable, so a later power loss can still take them.
///
/// What an injected fault does: a failed call has no effect. A failed Append() adds nothing to the
/// buffer, a failed Flush() hands nothing over, a failed Sync() makes nothing durable (and marks
/// the file, as above), a failed NewWritableFile() does not empty the file, a failed Close() hands
/// nothing over and still closes the handle. The Env's documentation says that after a failed
/// write only Close() is valid and what the file holds is unspecified; this Env does not stop a
/// caller from going on, so that a test can check what the caller does next, and after a failed
/// Flush() a later Flush() still hands over the bytes that were waiting.
///
/// Two independent streams of random numbers come from the seed: one decides what a power loss
/// keeps, the other decides which calls fail and which reads are corrupted. Configuring read
/// corruption in a run therefore does not change what a given seed loses to a power loss.
///
/// What it does not model, so that a test does not trust it for these:
/// - Directories themselves: CreateDir() and RemoveDir() are durable at once and no crash rolls
///   them back. Only the entries for files are subject to SyncDir().
/// - Garbage tails, and zeros that do not start where the synced bytes end: what a power loss
///   leaves of a file is a clean prefix of what was written, or the synced bytes followed by zeros
///   to the end, never a prefix followed by junk, zeros after a part of the unsynced bytes, or a
///   hole.
/// - Partial effects of a failure: a failed call does nothing, not half of what it was asked.
/// - A limit on a handle's buffer, which a real writer flushes when it is full: here a crash loses
///   every byte that was not flushed, which is never fewer than a real writer would lose.
/// - Faults that a crash clears: the countdowns, the probabilities and the corruption stay armed
///   across a crash, until ClearFaults(). A failure injected into the restart is a feature.
/// - Corruption of the bytes at rest: a read may come back with a flipped bit, but the stored
///   bytes never change by themselves.
class FaultInjectionEnv final : public Env {
 public:
  /// Creates an Env with no files, a clock at zero and no fault configured.
  /// @param seed fixes every random choice this Env makes.
  explicit FaultInjectionEnv(uint64_t seed);

  FaultInjectionEnv(const FaultInjectionEnv&) = delete;
  FaultInjectionEnv& operator=(const FaultInjectionEnv&) = delete;

  /// The calls that can be made to fail.
  ///
  /// kRead covers the Read() of a sequential and of a random-access file, and Skip() is not
  /// faultable. kFlush is the Flush() call alone: the flush inside Sync() and Close() belongs to
  /// them. The others name the Env or file method they are called after.
  enum class Op {
    kNewWritable,
    kAppend,
    kSync,
    kClose,
    kRename,
    kRemove,
    kSyncDir,
    kRead,
    kNewSequential,
    kNewRandomAccess,
    kFlush,
  };

  // The Env interface. NewSequentialFile() and the rest are documented on Env.
  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override;
  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override;
  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override;
  bool FileExists(const std::string& fname) override;
  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override;
  Status RemoveFile(const std::string& fname) override;
  Status CreateDir(const std::string& dirname) override;
  Status RemoveDir(const std::string& dirname) override;
  Status GetFileSize(const std::string& fname, uint64_t* size) override;
  Status RenameFile(const std::string& src, const std::string& target) override;
  Status SyncDir(const std::string& dirname) override;
  Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) override;
  Status UnlockFile(std::unique_ptr<FileLock> lock) override;
  uint64_t NowMicros() override;
  void SleepForMicroseconds(uint64_t micros) override;

  /// Simulates a power loss. Each directory keeps a prefix of the changes made to it since it was
  /// last synced, of a length drawn uniformly from none to all of them, and undoes the rest. Each
  /// file keeps the bytes that were synced plus a prefix of the ones that were handed over and not
  /// synced, of a length drawn the same way, or, one time in four, as many zero bytes as there
  /// were of those. Bytes that were still in a handle are gone. What survives is durable from then
  /// on. Invalidates every handle and lock opened before the call.
  void SimulatePowerLoss();

  /// Like SimulatePowerLoss(), but nothing that was not made durable survives: every directory
  /// keeps none of its unsynced changes and every file keeps exactly its synced bytes. The worst
  /// case.
  void SimulatePowerLossDropUnsynced();

  /// Simulates the death of the process while the machine stays up: every byte that was flushed
  /// and every directory change is kept, since the operating system still has them, and the bytes
  /// still in a handle are lost. Nothing becomes durable, so a later power loss can still take what
  /// is unsynced. Invalidates every handle and lock opened before the call, and releases the
  /// locks, as the death of a process does.
  void SimulateProcessCrash();

  /// Makes one call of `op` fail: the first `successes` calls of `op` from now on that succeed are
  /// let through, and the call after them fails with an IOError that says "injected" and names the
  /// path, once. A call that fails for another reason, such as an injected failure by probability
  /// or a missing file, is not one of the `successes`. Calling it again for the same `op`
  /// replaces the earlier countdown.
  /// @param op the kind of call to fail.
  /// @param successes how many successful calls to let through first; at least 0.
  void FailOpAfter(Op op, int successes);

  /// Makes every call of `op` fail with probability `p`, each call drawn independently from the
  /// seeded stream. 0 turns it off; 1 fails every call without drawing.
  /// @param op the kind of call to fail.
  /// @param p the probability, from 0 to 1.
  void SetFailureProbability(Op op, double p);

  /// Makes a successful read that returns at least one byte come back with exactly one bit flipped,
  /// at a random position, with probability `p`. The stored bytes are not changed: a second read
  /// is corrupted, or not, on its own. 0 turns it off; 1 corrupts every such read.
  /// @param p the probability, from 0 to 1.
  void SetReadCorruptionProbability(double p);

  /// Removes every configured fault: the countdowns, the probabilities and the corruption. The
  /// counters keep what they have counted.
  void ClearFaults();

  /// Returns how many times a crash was simulated, of any kind.
  uint64_t crashes() const;

  /// Returns how many calls were failed on purpose.
  uint64_t injected_errors() const;

  /// Returns how many reads were returned with a flipped bit.
  uint64_t injected_corruptions() const;

  /// Test helper: makes `fname` hold exactly `data`, creating it if need be, and marks the bytes
  /// synced and the directory entry durable, as if the file had been written, synced and its
  /// directory synced before the test began. It is a new file: a handle already open on the name
  /// stays on the file it had. Changes to the name that were waiting for a SyncDir() are dropped,
  /// because they would only undo it. It is not a call of any Op and cannot fail.
  /// @param fname the path of the file.
  /// @param data the new contents.
  void SetFileContents(const std::string& fname, std::string data);

  /// Test helper: returns what readers see in `fname` now, or an empty string if there is no such
  /// file.
  /// @param fname the path of the file.
  std::string FileContents(const std::string& fname) const;

 private:
  const std::shared_ptr<internal::FaultState> state_;
};

}  // namespace shale

#endif  // SHALE_TESTKIT_FAULT_ENV_H_
