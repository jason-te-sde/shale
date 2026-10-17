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
/// Without a fault configured it behaves like NewMemEnv(): the same files, the same errors, a
/// virtual clock that moves only when someone sleeps, and any number of threads. What it adds is
/// the gap between what a program has written and what a disk has kept.
///
/// How it models a disk:
/// - Every file has a live content, which readers see at once, and a count of leading bytes that
///   were made durable by Sync().
/// - The namespace has two views: the live one, which every call sees, and the durable one, which
///   is what a power loss brings back. SyncDir(d) copies what the live view holds under `d`, and
///   what it no longer holds, into the durable view. The parent of a path is everything before its
///   last '/', so `/d/f` is under `/d`, `/d/sub/g` is under `/d/sub` and `/f` is under `/`.
/// - Directories themselves are not rolled back: CreateDir() and RemoveDir() are durable at once,
///   and SyncDir() applies to the files in them. Opening a file for write empties it, and that is
///   durable at once too: a power loss does not bring the old bytes back, although a real disk
///   might. The engine never reopens a file it has written.
/// - A handle has no buffer of its own, so Append() reaches the file at once and a process crash
///   keeps every appended byte. Flush() has nothing to do and cannot be made to fail.
/// - A crash invalidates every handle and every lock taken before it. The restarted process opens
///   its own.
///
/// What an injected fault does: a failed call has no effect. A failed Append() appends nothing, a
/// failed Sync() makes nothing durable, a failed NewWritableFile() does not empty the file, a
/// failed Close() still closes the handle. The Env's documentation says that after a failed write
/// only Close() is valid and what the file holds is unspecified; this Env holds exactly what was
/// there before, which is one of the outcomes the documentation allows, and does not stop a caller
/// from going on, so that a test can check what the caller does next.
///
/// Two independent streams of random numbers come from the seed: one decides what a power loss
/// keeps, the other decides which calls fail and which reads are corrupted. Configuring read
/// corruption in a run therefore does not change what a given seed loses to a power loss.
class FaultInjectionEnv final : public Env {
 public:
  /// Creates an Env with no files, a clock at zero and no fault configured.
  /// @param seed fixes every random choice this Env makes.
  explicit FaultInjectionEnv(uint64_t seed);

  FaultInjectionEnv(const FaultInjectionEnv&) = delete;
  FaultInjectionEnv& operator=(const FaultInjectionEnv&) = delete;

  /// The calls that can be made to fail.
  ///
  /// kRead covers the Read() of a sequential and of a random-access file; Skip() and Flush() are
  /// not faultable. The others name the Env or file method they are called after.
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

  /// Simulates a power loss. Each file keeps the bytes that were synced plus a prefix of the ones
  /// that were not, of a length drawn uniformly from none to all of them. Every creation, removal
  /// and rename that no SyncDir() of its directory made durable is undone. What survives is
  /// durable from then on. Invalidates every handle and lock opened before the call.
  void SimulatePowerLoss();

  /// Like SimulatePowerLoss(), but every file keeps exactly its synced bytes: the worst case.
  void SimulatePowerLossDropUnsynced();

  /// Simulates the death of the process while the machine stays up: every appended byte and every
  /// directory change is kept, since the operating system still has them. Nothing becomes durable,
  /// so a later power loss can still take what is unsynced. Invalidates every handle and lock
  /// opened before the call, and releases the locks, as the death of a process does.
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
  /// directory synced before the test began. A handle already open on the file sees the new bytes.
  /// It is not a call of any Op and cannot fail.
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
