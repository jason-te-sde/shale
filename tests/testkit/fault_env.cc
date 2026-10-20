#include "testkit/fault_env.h"

#include <algorithm>
#include <array>
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
#include "util/random.h"
#include "util/thread_annotations.h"

namespace shale {
namespace internal {

namespace {

using Op = FaultInjectionEnv::Op;

constexpr size_t kOpCount = static_cast<size_t>(Op::kFlush) + 1;

size_t IndexOf(Op op) {
  const auto index = static_cast<size_t>(op);
  SHALE_CHECK(index < kOpCount, "unknown FaultInjectionEnv::Op");
  return index;
}

Status NoSuchEntry(std::string_view operation, const std::string& path) {
  return Status::IOError(std::string(operation) + " " + path, "No such file or directory");
}

Status Invalidated(std::string_view operation, const std::string& path) {
  return Status::IOError(std::string(operation) + " " + path, "handle invalidated by crash");
}

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

// Everything before the last slash of `path`: the directory whose SyncDir() makes the entry for
// `path` durable. A path directly under the root has the root as its parent, and a name with no
// slash at all has the empty string.
std::string ParentOf(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return "";
  }
  return slash == 0 ? "/" : path.substr(0, slash);
}

// Streams of random numbers are told apart by mixing a constant into the seed, so that the
// decisions of one kind do not shift with how many of another kind were made.
constexpr uint64_t kCrashStream = 0x6372617368ULL;
constexpr uint64_t kFaultStream = 0x6661756c74ULL;

// One power loss in this many leaves the unsynced bytes of a file as zeros instead of a prefix of
// them, drawn for each file that has unsynced bytes.
constexpr uint32_t kZeroFillOneIn = 4;

}  // namespace

// What the disk holds for one file: the bytes that reached it, and how many leading ones are
// durable. Everything in it is guarded by the mutex of the FaultState that made it.
struct File {
  std::string data;
  // How many leading bytes of `data` are durable.
  uint64_t synced_size = 0;
  // True once a Sync() of this file has failed. From then on a Sync() reports success and makes
  // nothing durable: `synced_size` never moves again. See FaultState::Sync().
  bool sync_failed = false;
};

// What a handle is open on, standing for a POSIX inode: the file a name had when the handle was
// opened. Opening the name for write puts a new, empty File behind the same Inode, so that handles
// already open on it find it emptied, as O_TRUNC makes them; the File it had stays in the durable
// view until the directory is synced.
struct Inode {
  std::shared_ptr<File> file;
};

using DurableView = std::map<std::string, std::shared_ptr<File>>;

// One change to a directory that no SyncDir() has made durable yet.
struct NamespaceOp {
  enum class Kind {
    kSet,     // `name` now names `file`: a creation, or an open for write that emptied the file.
    kRemove,  // `name` is gone.
    kRename,  // `name` is gone and `target` names `file`, in one step: both are in one directory.
  };

  static NamespaceOp Set(std::string name, std::shared_ptr<File> file) {
    return NamespaceOp{Kind::kSet, std::move(name), std::string(), std::move(file)};
  }
  static NamespaceOp Remove(std::string name) {
    return NamespaceOp{Kind::kRemove, std::move(name), std::string(), nullptr};
  }
  static NamespaceOp Rename(std::string name, std::string target, std::shared_ptr<File> file) {
    return NamespaceOp{Kind::kRename, std::move(name), std::move(target), std::move(file)};
  }

  // Makes the change in `*view`.
  void ApplyTo(DurableView* view) const {
    switch (kind) {
      case Kind::kSet:
        (*view)[name] = file;
        break;
      case Kind::kRemove:
        view->erase(name);
        break;
      case Kind::kRename:
        view->erase(name);
        (*view)[target] = file;
        break;
    }
  }

  Kind kind;
  std::string name;
  std::string target;
  std::shared_ptr<File> file;
};

// Everything one FaultInjectionEnv knows. Handles hold a share of it, so that one may outlive the
// Env that made it, as a MemEnv handle may.
//
// One mutex guards all of it, the file contents included. These Envs are used by tests, where a
// simple lock that makes every call atomic is worth more than letting appends to different files
// overlap, and the decisions about faults then follow the order of the calls exactly.
class FaultState {
 public:
  explicit FaultState(uint64_t seed)
      : crash_random_(seed ^ kCrashStream), fault_random_(seed ^ kFaultStream) {}

  // --- Env operations ----------------------------------------------------------------------------

  // Opens `fname` for reading: the work of NewSequentialFile and NewRandomAccessFile. `op` says
  // which of the two it is.
  Status OpenForRead(Op op, const std::string& fname, std::shared_ptr<Inode>* inode,
                     uint64_t* epoch) {
    MutexLock lock(&mu_);
    Status s = Inject(op, "open", fname);
    if (!s.ok()) {
      return s;
    }
    const auto it = files_.find(fname);
    if (it == files_.end()) {
      return dirs_.count(NormalizeDir(fname)) != 0
                 ? Status::IOError("open " + fname, "Is a directory")
                 : NoSuchEntry("open", fname);
    }
    *inode = it->second;
    *epoch = epoch_;
    NoteSuccess(op);
    return Status::OK();
  }

  Status OpenForWrite(const std::string& fname, std::shared_ptr<Inode>* inode, uint64_t* epoch) {
    MutexLock lock(&mu_);
    Status s = Inject(Op::kNewWritable, "open", fname);
    if (!s.ok()) {
      return s;
    }
    if (dirs_.count(NormalizeDir(fname)) != 0) {
      return Status::IOError("open " + fname, "Is a directory");
    }
    const auto it = files_.find(fname);
    if (it == files_.end()) {
      *inode = CreateFile(fname);
    } else {
      // A new, empty file behind the same Inode, which is what O_TRUNC looks like to the handles
      // open on it. The old file is still what the durable view has until the directory is
      // synced, so a power loss may bring it back, and its bytes with it.
      it->second->file = std::make_shared<File>();
      LogChange(NamespaceOp::Set(fname, it->second->file));
      *inode = it->second;
    }
    *epoch = epoch_;
    NoteSuccess(Op::kNewWritable);
    return Status::OK();
  }

  bool FileExists(const std::string& fname) {
    MutexLock lock(&mu_);
    return files_.count(fname) != 0 || dirs_.count(NormalizeDir(fname)) != 0;
  }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) {
    result->clear();
    const std::string normalized = NormalizeDir(dir);
    const std::string prefix = PrefixOf(normalized);
    MutexLock lock(&mu_);
    if (dirs_.count(normalized) == 0) {
      return NoSuchEntry("list directory", dir);
    }
    // The map is ordered by path, so everything below the directory is one contiguous run that
    // starts at the prefix. A name with a slash left in it is deeper than a child, and is not one.
    for (auto it = files_.lower_bound(prefix); it != files_.end(); ++it) {
      if (!it->first.starts_with(prefix)) {
        break;
      }
      AddIfChild(it->first, prefix, result);
    }
    for (auto it = dirs_.lower_bound(prefix); it != dirs_.end(); ++it) {
      if (!it->starts_with(prefix)) {
        break;
      }
      AddIfChild(*it, prefix, result);
    }
    return Status::OK();
  }

  Status RemoveFile(const std::string& fname) {
    MutexLock lock(&mu_);
    Status s = Inject(Op::kRemove, "remove", fname);
    if (!s.ok()) {
      return s;
    }
    // Erasing the entry drops the live view's reference only. Open handles keep the contents, and
    // so does the durable view, until the directory is synced.
    if (files_.erase(fname) == 0) {
      return NoSuchEntry("remove", fname);
    }
    LogChange(NamespaceOp::Remove(fname));
    NoteSuccess(Op::kRemove);
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) {
    MutexLock lock(&mu_);
    if (files_.count(dirname) != 0) {
      return Status::IOError("create directory " + dirname, "File exists");
    }
    dirs_.insert(NormalizeDir(dirname));
    return Status::OK();
  }

  Status RemoveDir(const std::string& dirname) {
    const std::string normalized = NormalizeDir(dirname);
    MutexLock lock(&mu_);
    if (dirs_.count(normalized) == 0) {
      return NoSuchEntry("remove directory", dirname);
    }
    if (HasEntriesBelow(PrefixOf(normalized))) {
      return Status::IOError("remove directory " + dirname, "Directory not empty");
    }
    dirs_.erase(normalized);
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) {
    *size = 0;
    MutexLock lock(&mu_);
    const auto it = files_.find(fname);
    if (it == files_.end()) {
      return dirs_.count(NormalizeDir(fname)) != 0
                 ? Status::IOError("get size of " + fname, "Is a directory")
                 : NoSuchEntry("get size of", fname);
    }
    *size = it->second->file->data.size();
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) {
    const std::string what = src + " to " + target;
    MutexLock lock(&mu_);
    Status s = Inject(Op::kRename, "rename", what);
    if (!s.ok()) {
      return s;
    }
    const auto source = files_.find(src);
    if (source == files_.end()) {
      return NoSuchEntry("rename", what);
    }
    if (dirs_.count(NormalizeDir(target)) != 0) {
      return Status::IOError("rename " + what, "Is a directory");
    }
    if (src != target) {
      std::shared_ptr<File> file = source->second->file;
      // Whatever `target` held is dropped from the live view, and lives on in the handles that
      // were open on it and in the durable view, as an unlinked file does until the directory is
      // synced.
      files_[target] = std::move(source->second);
      files_.erase(source);
      if (ParentOf(src) == ParentOf(target)) {
        // One step in the one directory's log, so that a power loss keeps the whole rename or none
        // of it.
        LogChange(NamespaceOp::Rename(src, target, file));
      } else {
        // Two directories, two logs, and a power loss may keep either half or both.
        LogChange(NamespaceOp::Remove(src));
        LogChange(NamespaceOp::Set(target, file));
      }
    }
    NoteSuccess(Op::kRename);
    return Status::OK();
  }

  Status SyncDir(const std::string& dirname) {
    const std::string normalized = NormalizeDir(dirname);
    MutexLock lock(&mu_);
    Status s = Inject(Op::kSyncDir, "sync directory", dirname);
    if (!s.ok()) {
      return s;
    }
    if (dirs_.count(normalized) == 0) {
      return NoSuchEntry("open directory", dirname);
    }
    // Every change to the directory so far becomes durable, in the order it was made, and the log
    // starts again empty. Changes to other directories wait for their own SyncDir().
    const auto log = logs_.find(normalized);
    if (log != logs_.end()) {
      for (const NamespaceOp& op : log->second) {
        op.ApplyTo(&durable_);
      }
      logs_.erase(log);
    }
    NoteSuccess(Op::kSyncDir);
    return Status::OK();
  }

  // Takes the lock `fname` and stores the epoch it was taken in.
  Status LockFile(const std::string& fname, uint64_t* epoch) {
    MutexLock lock(&mu_);
    if (dirs_.count(NormalizeDir(fname)) != 0) {
      return Status::IOError("open lock file " + fname, "Is a directory");
    }
    // Like open(O_CREAT) on POSIX, taking the lock creates the file if it is missing.
    if (files_.count(fname) == 0) {
      CreateFile(fname);
    }
    if (!locks_.insert(fname).second) {
      return Status::IOError("lock " + fname, "already held by another process or handle");
    }
    *epoch = epoch_;
    return Status::OK();
  }

  // Releases a lock taken in `epoch`. A crash released every lock of the epochs before it, and the
  // new owner may hold the same name, so a lock from an older epoch must change nothing.
  void ReleaseLock(const std::string& fname, uint64_t epoch) {
    MutexLock lock(&mu_);
    if (epoch == epoch_) {
      locks_.erase(fname);
    }
  }

  // True unless a crash happened since `epoch`.
  bool IsCurrent(uint64_t epoch) const {
    MutexLock lock(&mu_);
    return epoch == epoch_;
  }

  uint64_t NowMicros() {
    MutexLock lock(&mu_);
    return now_micros_;
  }

  void Sleep(uint64_t micros) {
    MutexLock lock(&mu_);
    // Saturate instead of wrapping: a clock that goes backwards breaks what NowMicros() promises.
    now_micros_ = micros > std::numeric_limits<uint64_t>::max() - now_micros_
                      ? std::numeric_limits<uint64_t>::max()
                      : now_micros_ + micros;
  }

  // --- Handle operations -------------------------------------------------------------------------

  // Adds `data` to the handle's buffer, `*buffer`. Nothing reaches the file until the handle
  // flushes, syncs or closes.
  Status Append(const std::string& fname, uint64_t epoch, std::string* buffer,
                std::string_view data) {
    MutexLock lock(&mu_);
    if (epoch != epoch_) {
      return Invalidated("append to", fname);
    }
    Status s = Inject(Op::kAppend, "append to", fname);
    if (!s.ok()) {
      return s;
    }
    buffer->append(data);
    NoteSuccess(Op::kAppend);
    return Status::OK();
  }

  // The part of Skip() that a handle cannot do on its own: say whether the handle is still alive.
  Status CheckAlive(std::string_view operation, const std::string& fname, uint64_t epoch) {
    MutexLock lock(&mu_);
    return epoch == epoch_ ? Status::OK() : Invalidated(operation, fname);
  }

  // Hands the handle's buffer to the file, where readers see it. A failed Flush hands over nothing.
  Status Flush(const std::string& fname, uint64_t epoch, Inode* inode, std::string* buffer) {
    MutexLock lock(&mu_);
    if (epoch != epoch_) {
      return Invalidated("flush", fname);
    }
    Status s = Inject(Op::kFlush, "flush", fname);
    if (!s.ok()) {
      return s;
    }
    HandOver(inode, buffer);
    NoteSuccess(Op::kFlush);
    return Status::OK();
  }

  // Flushes, then makes every byte of the file durable.
  //
  // A Sync() that fails marks the file: from then on it reports success and makes nothing durable,
  // whatever it is asked. The Env's documentation says a Sync() that succeeds after a failed one
  // proves nothing, because some operating systems drop the data the first one failed to write and
  // then report success for the second. A caller that retries and trusts the result must lose its
  // bytes here, where a test can see it, and not on a machine that has lost power.
  Status Sync(const std::string& fname, uint64_t epoch, Inode* inode, std::string* buffer) {
    MutexLock lock(&mu_);
    if (epoch != epoch_) {
      return Invalidated("sync", fname);
    }
    Status s = Inject(Op::kSync, "sync", fname);
    if (!s.ok()) {
      inode->file->sync_failed = true;
      return s;
    }
    HandOver(inode, buffer);
    File& file = *inode->file;
    if (!file.sync_failed) {
      file.synced_size = file.data.size();
    }
    NoteSuccess(Op::kSync);
    return Status::OK();
  }

  // Flushes, and ends the handle's use of the file. A failed Close() flushes nothing, and the
  // handle is gone either way, so what it buffered is lost.
  Status Close(const std::string& fname, uint64_t epoch, Inode* inode, std::string* buffer) {
    MutexLock lock(&mu_);
    if (epoch != epoch_) {
      return Invalidated("close", fname);
    }
    Status s = Inject(Op::kClose, "close", fname);
    if (!s.ok()) {
      return s;
    }
    HandOver(inode, buffer);
    NoteSuccess(Op::kClose);
    return Status::OK();
  }

  // Copies up to `n` bytes of the file from `offset` into `scratch` and stores how many in
  // `*count`: none at or past the end, and none when the read fails.
  Status Read(const std::string& fname, uint64_t epoch, const Inode& inode, uint64_t offset,
              size_t n, char* scratch, size_t* count) {
    *count = 0;
    MutexLock lock(&mu_);
    if (epoch != epoch_) {
      return Invalidated("read", fname);
    }
    Status s = Inject(Op::kRead, "read", fname);
    if (!s.ok()) {
      return s;
    }
    const std::string& data = inode.file->data;
    if (offset < data.size()) {
      const size_t available = data.size() - static_cast<size_t>(offset);
      *count = std::min(n, available);
      std::memcpy(scratch, data.data() + static_cast<size_t>(offset), *count);
    }
    if (*count > 0 && Chance(corruption_probability_)) {
      FlipOneBit(scratch, *count);
      injected_corruptions_++;
    }
    NoteSuccess(Op::kRead);
    return Status::OK();
  }

  // --- Crashes -----------------------------------------------------------------------------------

  enum class Crash { kPowerLoss, kPowerLossDropUnsynced, kProcess };

  void SimulateCrash(Crash kind) {
    MutexLock lock(&mu_);
    crashes_++;
    // Every handle and lock of the epoch that ends here is dead. The locks are released, because
    // the process that held them is gone.
    epoch_++;
    locks_.clear();
    if (kind == Crash::kProcess) {
      return;
    }
    // Back to what the directories had made durable, plus a prefix of what they had not. Each
    // directory keeps a prefix of its own log, from none of it to all of it, of a length drawn
    // from the crash stream: a change is never kept without the ones made before it in the same
    // directory. The directories are taken in name order, and one draw is made for each that has a
    // log, so a seed always keeps the same changes. Dropping the unsynced keeps no prefix at all.
    for (const auto& [dir, log] : logs_) {
      uint64_t kept = 0;
      if (kind == Crash::kPowerLoss && !log.empty()) {
        kept = crash_random_.Next() % (log.size() + 1);
      }
      for (uint64_t i = 0; i < kept; i++) {
        log[static_cast<size_t>(i)].ApplyTo(&durable_);
      }
    }
    logs_.clear();
    // The live view is what is durable now. Its handles are dead, so it gets Inodes of its own.
    files_.clear();
    for (const auto& [name, file] : durable_) {
      auto inode = std::make_shared<Inode>();
      inode->file = file;
      files_[name] = std::move(inode);
    }
    // What each file keeps. The names are visited in order, and the draws are made per file that
    // has unsynced bytes, so a seed always loses the same bytes of the same files. A file that is
    // under two names in the durable view (a rename across directories that was kept on one side
    // only) is visited twice; the second visit finds nothing unsynced, because the first made what
    // survived durable. Bytes still in a handle never reached the file, so they are not here.
    for (const auto& entry : durable_) {
      File& file = *entry.second;
      const uint64_t unsynced = file.data.size() - file.synced_size;
      if (kind == Crash::kPowerLoss && unsynced > 0 && crash_random_.OneIn(kZeroFillOneIn)) {
        // The length of the file reached the disk and its data did not: the bytes appended since
        // the last sync read back as zeros, as many as were written.
        std::fill(file.data.begin() + static_cast<std::ptrdiff_t>(file.synced_size),
                  file.data.end(), '\0');
      } else {
        uint64_t kept = 0;
        if (kind == Crash::kPowerLoss && unsynced > 0) {
          kept = crash_random_.Next() % (unsynced + 1);
        }
        file.data.resize(static_cast<size_t>(file.synced_size + kept));
      }
      // What the disk kept is on the disk now.
      file.synced_size = file.data.size();
    }
  }

  // --- Faults ------------------------------------------------------------------------------------

  void FailOpAfter(Op op, int successes) {
    MutexLock lock(&mu_);
    OpFaults& faults = faults_[IndexOf(op)];
    faults.armed = true;
    faults.successes_left = successes;
  }

  void SetFailureProbability(Op op, double probability) {
    MutexLock lock(&mu_);
    faults_[IndexOf(op)].failure_probability = probability;
  }

  void SetReadCorruptionProbability(double probability) {
    MutexLock lock(&mu_);
    corruption_probability_ = probability;
  }

  void ClearFaults() {
    MutexLock lock(&mu_);
    faults_ = {};
    corruption_probability_ = 0.0;
  }

  uint64_t crashes() const {
    MutexLock lock(&mu_);
    return crashes_;
  }

  uint64_t injected_errors() const {
    MutexLock lock(&mu_);
    return injected_errors_;
  }

  uint64_t injected_corruptions() const {
    MutexLock lock(&mu_);
    return injected_corruptions_;
  }

  // --- Test helpers ------------------------------------------------------------------------------

  void SetFileContents(const std::string& fname, std::string data) {
    MutexLock lock(&mu_);
    SHALE_CHECK(dirs_.count(NormalizeDir(fname)) == 0, "SetFileContents on a directory: " + fname);
    // A new file, not a change to the old one: the old file may still be the durable view's file
    // under another name, if it was renamed and the rename was not synced.
    auto file = std::make_shared<File>();
    file->data = std::move(data);
    file->synced_size = file->data.size();
    auto inode = std::make_shared<Inode>();
    inode->file = file;
    files_[fname] = std::move(inode);
    durable_[fname] = std::move(file);
    // The name is durable as it is now, so the changes to it that were waiting for a SyncDir()
    // would only undo it.
    const auto log = logs_.find(ParentOf(fname));
    if (log != logs_.end()) {
      std::erase_if(log->second, [&fname](const NamespaceOp& op) {
        return op.name == fname || (op.kind == NamespaceOp::Kind::kRename && op.target == fname);
      });
    }
  }

  std::string FileContents(const std::string& fname) const {
    MutexLock lock(&mu_);
    const auto it = files_.find(fname);
    return it == files_.end() ? std::string() : it->second->file->data;
  }

 private:
  // What is configured for one kind of call.
  struct OpFaults {
    // True while a FailOpAfter() countdown is running.
    bool armed = false;
    // How many more successful calls to let through before the one that fails.
    int successes_left = 0;
    double failure_probability = 0.0;
  };

  // Returns an injected error if this call of `op` is to fail, and OK otherwise. Called with the
  // lock held, before the call does anything, so that a failed call has no effect. `what` and
  // `path` make the message.
  Status Inject(Op op, std::string_view what, const std::string& path) SHALE_REQUIRES(mu_) {
    OpFaults& faults = faults_[IndexOf(op)];
    const bool countdown_over = faults.armed && faults.successes_left == 0;
    if (countdown_over) {
      faults.armed = false;
    }
    if (countdown_over || Chance(faults.failure_probability)) {
      injected_errors_++;
      return Status::IOError(std::string(what) + " " + path, "injected I/O error");
    }
    return Status::OK();
  }

  // Counts one successful call of `op` against a running FailOpAfter() countdown. Called at the
  // end of a call that is about to return OK, so that a call that failed, for whatever reason,
  // never counts as one of the successes the countdown is waiting for.
  void NoteSuccess(Op op) SHALE_REQUIRES(mu_) {
    OpFaults& faults = faults_[IndexOf(op)];
    if (faults.armed && faults.successes_left > 0) {
      faults.successes_left--;
    }
  }

  // Returns true with probability `p`. The extremes draw nothing, so that a run with no fault
  // configured uses none of the fault stream.
  bool Chance(double p) SHALE_REQUIRES(mu_) {
    if (p <= 0.0) {
      return false;
    }
    if (p >= 1.0) {
      return true;
    }
    // The top 53 bits make a double in [0, 1) exactly, whatever the platform.
    const double draw =
        static_cast<double>(fault_random_.Next() >> 11) * (1.0 / 9007199254740992.0);
    return draw < p;
  }

  // Flips one bit, chosen uniformly, in the `count` bytes at `bytes`. `count` is at least 1.
  void FlipOneBit(char* bytes, size_t count) SHALE_REQUIRES(mu_) {
    const uint64_t bit = fault_random_.Next() % (static_cast<uint64_t>(count) * 8);
    char& target = bytes[static_cast<size_t>(bit / 8)];
    const unsigned flipped = static_cast<unsigned char>(target) ^ (1U << (bit % 8));
    target = static_cast<char>(static_cast<unsigned char>(flipped));
  }

  // Returns true if any file or directory lies below `prefix`, which ends in a slash.
  bool HasEntriesBelow(const std::string& prefix) const SHALE_REQUIRES(mu_) {
    const auto file = files_.lower_bound(prefix);
    if (file != files_.end() && file->first.starts_with(prefix)) {
      return true;
    }
    const auto dir = dirs_.lower_bound(prefix);
    return dir != dirs_.end() && dir->starts_with(prefix);
  }

  // Creates an empty file under a name that has none, and makes it the live view's.
  std::shared_ptr<Inode> CreateFile(const std::string& fname) SHALE_REQUIRES(mu_) {
    auto inode = std::make_shared<Inode>();
    inode->file = std::make_shared<File>();
    files_[fname] = inode;
    LogChange(NamespaceOp::Set(fname, inode->file));
    return inode;
  }

  // Records a change to the live namespace that the durable one does not have yet, in the log of
  // the directory it was made in.
  void LogChange(NamespaceOp op) SHALE_REQUIRES(mu_) {
    const std::string dir = ParentOf(op.name);
    logs_[dir].push_back(std::move(op));
  }

  // Moves what a handle buffered into the file it is open on, where readers see it.
  static void HandOver(Inode* inode, std::string* buffer) {
    inode->file->data.append(*buffer);
    buffer->clear();
  }

  // Adds the part of `path` after `prefix` to `*names` if it is a single name.
  static void AddIfChild(const std::string& path, const std::string& prefix,
                         std::vector<std::string>* names) {
    const std::string_view rest = std::string_view(path).substr(prefix.size());
    if (!rest.empty() && rest.find('/') == std::string_view::npos) {
      names->emplace_back(rest);
    }
  }

  mutable Mutex mu_;
  // The live view of the namespace, and the durable one. The live one maps names to what a handle
  // can be opened on; the durable one maps them to the files the disk would show after a crash.
  std::map<std::string, std::shared_ptr<Inode>> files_ SHALE_GUARDED_BY(mu_);
  DurableView durable_ SHALE_GUARDED_BY(mu_);
  // The changes to the live view that the durable view lacks, per directory, oldest first. The
  // durable view with every log applied in order is the live view.
  std::map<std::string, std::vector<NamespaceOp>> logs_ SHALE_GUARDED_BY(mu_);
  std::set<std::string> dirs_ SHALE_GUARDED_BY(mu_);
  std::set<std::string> locks_ SHALE_GUARDED_BY(mu_);
  uint64_t now_micros_ SHALE_GUARDED_BY(mu_) = 0;
  // Counts the crashes. A handle remembers the value it was opened under, and is dead once the
  // value has moved on.
  uint64_t epoch_ SHALE_GUARDED_BY(mu_) = 0;
  Random crash_random_ SHALE_GUARDED_BY(mu_);
  Random fault_random_ SHALE_GUARDED_BY(mu_);
  std::array<OpFaults, kOpCount> faults_ SHALE_GUARDED_BY(mu_);
  double corruption_probability_ SHALE_GUARDED_BY(mu_) = 0.0;
  uint64_t crashes_ SHALE_GUARDED_BY(mu_) = 0;
  uint64_t injected_errors_ SHALE_GUARDED_BY(mu_) = 0;
  uint64_t injected_corruptions_ SHALE_GUARDED_BY(mu_) = 0;
};

}  // namespace internal

namespace {

using internal::FaultState;
using internal::Inode;

class FaultSequentialFile final : public SequentialFile {
 public:
  FaultSequentialFile(std::shared_ptr<FaultState> state, std::string fname,
                      std::shared_ptr<Inode> inode, uint64_t epoch)
      : state_(std::move(state)),
        fname_(std::move(fname)),
        inode_(std::move(inode)),
        epoch_(epoch) {}

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    size_t count = 0;
    const Status s = state_->Read(fname_, epoch_, *inode_, position_, n, scratch, &count);
    *result = std::string_view(scratch, count);
    position_ += count;
    return s;
  }

  Status Skip(uint64_t n) override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    Status s = state_->CheckAlive("skip in", fname_, epoch_);
    if (!s.ok()) {
      return s;
    }
    // Skipping past the end is allowed, so the position may exceed the size; saturate rather than
    // wrap if someone skips by the largest possible amount.
    position_ = n > std::numeric_limits<uint64_t>::max() - position_
                    ? std::numeric_limits<uint64_t>::max()
                    : position_ + n;
    return Status::OK();
  }

 private:
  const std::shared_ptr<FaultState> state_;
  const std::string fname_;
  const std::shared_ptr<Inode> inode_;
  const uint64_t epoch_;
  uint64_t position_ = 0;
  ExclusiveUse use_;
};

class FaultRandomAccessFile final : public RandomAccessFile {
 public:
  FaultRandomAccessFile(std::shared_ptr<FaultState> state, std::string fname,
                        std::shared_ptr<Inode> inode, uint64_t epoch)
      : state_(std::move(state)),
        fname_(std::move(fname)),
        inode_(std::move(inode)),
        epoch_(epoch) {}

  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    size_t count = 0;
    const Status s = state_->Read(fname_, epoch_, *inode_, offset, n, scratch, &count);
    *result = std::string_view(scratch, count);
    return s;
  }

 private:
  const std::shared_ptr<FaultState> state_;
  const std::string fname_;
  const std::shared_ptr<Inode> inode_;
  const uint64_t epoch_;
};

class FaultWritableFile final : public WritableFile {
 public:
  FaultWritableFile(std::shared_ptr<FaultState> state, std::string fname,
                    std::shared_ptr<Inode> inode, uint64_t epoch)
      : state_(std::move(state)),
        fname_(std::move(fname)),
        inode_(std::move(inode)),
        epoch_(epoch) {}

  Status Append(std::string_view data) override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Append");
    return state_->Append(fname_, epoch_, &buffer_, data);
  }

  Status Flush() override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Flush");
    return state_->Flush(fname_, epoch_, inode_.get(), &buffer_);
  }

  Status Sync() override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Sync");
    return state_->Sync(fname_, epoch_, inode_.get(), &buffer_);
  }

  Status Close() override {
    const ExclusiveUse::Scope scope(&use_, fname_);
    CheckOpen("Close");
    // Closed whatever the outcome, as the Env's documentation says, and so whatever the buffer
    // still holds after a failure is gone.
    closed_ = true;
    const Status s = state_->Close(fname_, epoch_, inode_.get(), &buffer_);
    buffer_.clear();
    return s;
  }

 private:
  // The same rule as the other Envs', so that code which breaks it fails here too. It holds after
  // a crash as well: a handle the crash invalidated reports errors until it is closed, and is
  // still a bug to use after that.
  void CheckOpen(std::string_view operation) const {
    SHALE_CHECK(!closed_, std::string(operation) + " after Close on " + fname_);
  }

  const std::shared_ptr<FaultState> state_;
  const std::string fname_;
  const std::shared_ptr<Inode> inode_;
  const uint64_t epoch_;
  // The bytes appended and not yet flushed. A handle that is dropped, or that a crash invalidates,
  // takes them with it. Only the thread using the handle touches it.
  std::string buffer_;
  bool closed_ = false;
  ExclusiveUse use_;
};

// Destroying it releases the lock, if it is still the lock of the epoch it was taken in.
class FaultFileLock final : public FileLock {
 public:
  FaultFileLock(std::shared_ptr<FaultState> state, std::string fname, uint64_t epoch)
      : state_(std::move(state)), fname_(std::move(fname)), epoch_(epoch) {}

  FaultFileLock(const FaultFileLock&) = delete;
  FaultFileLock& operator=(const FaultFileLock&) = delete;

  ~FaultFileLock() override { state_->ReleaseLock(fname_, epoch_); }

  bool BelongsTo(const FaultState* state) const { return state_.get() == state; }
  const std::string& fname() const { return fname_; }
  uint64_t epoch() const { return epoch_; }

 private:
  const std::shared_ptr<FaultState> state_;
  const std::string fname_;
  const uint64_t epoch_;
};

}  // namespace

FaultInjectionEnv::FaultInjectionEnv(uint64_t seed)
    : state_(std::make_shared<internal::FaultState>(seed)) {}

Status FaultInjectionEnv::NewSequentialFile(const std::string& fname,
                                            std::unique_ptr<SequentialFile>* result) {
  result->reset();
  std::shared_ptr<Inode> inode;
  uint64_t epoch = 0;
  Status s = state_->OpenForRead(Op::kNewSequential, fname, &inode, &epoch);
  if (!s.ok()) {
    return s;
  }
  *result = std::make_unique<FaultSequentialFile>(state_, fname, std::move(inode), epoch);
  return Status::OK();
}

Status FaultInjectionEnv::NewRandomAccessFile(const std::string& fname,
                                              std::unique_ptr<RandomAccessFile>* result) {
  result->reset();
  std::shared_ptr<Inode> inode;
  uint64_t epoch = 0;
  Status s = state_->OpenForRead(Op::kNewRandomAccess, fname, &inode, &epoch);
  if (!s.ok()) {
    return s;
  }
  *result = std::make_unique<FaultRandomAccessFile>(state_, fname, std::move(inode), epoch);
  return Status::OK();
}

Status FaultInjectionEnv::NewWritableFile(const std::string& fname,
                                          std::unique_ptr<WritableFile>* result) {
  result->reset();
  std::shared_ptr<Inode> inode;
  uint64_t epoch = 0;
  Status s = state_->OpenForWrite(fname, &inode, &epoch);
  if (!s.ok()) {
    return s;
  }
  *result = std::make_unique<FaultWritableFile>(state_, fname, std::move(inode), epoch);
  return Status::OK();
}

bool FaultInjectionEnv::FileExists(const std::string& fname) { return state_->FileExists(fname); }

Status FaultInjectionEnv::GetChildren(const std::string& dir, std::vector<std::string>* result) {
  return state_->GetChildren(dir, result);
}

Status FaultInjectionEnv::RemoveFile(const std::string& fname) { return state_->RemoveFile(fname); }

Status FaultInjectionEnv::CreateDir(const std::string& dirname) {
  return state_->CreateDir(dirname);
}

Status FaultInjectionEnv::RemoveDir(const std::string& dirname) {
  return state_->RemoveDir(dirname);
}

Status FaultInjectionEnv::GetFileSize(const std::string& fname, uint64_t* size) {
  return state_->GetFileSize(fname, size);
}

Status FaultInjectionEnv::RenameFile(const std::string& src, const std::string& target) {
  return state_->RenameFile(src, target);
}

Status FaultInjectionEnv::SyncDir(const std::string& dirname) { return state_->SyncDir(dirname); }

Status FaultInjectionEnv::LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) {
  lock->reset();
  uint64_t epoch = 0;
  Status s = state_->LockFile(fname, &epoch);
  if (!s.ok()) {
    return s;
  }
  *lock = std::make_unique<FaultFileLock>(state_, fname, epoch);
  return Status::OK();
}

Status FaultInjectionEnv::UnlockFile(std::unique_ptr<FileLock> lock) {
  const auto* const fault_lock = dynamic_cast<const FaultFileLock*>(lock.get());
  SHALE_CHECK(fault_lock != nullptr && fault_lock->BelongsTo(state_.get()),
              "UnlockFile needs a lock that this Env's LockFile returned");
  // A lock from before a crash was released by the crash; saying so is the point of the call.
  const bool current = state_->IsCurrent(fault_lock->epoch());
  const std::string fname = fault_lock->fname();
  // Destroying the lock releases it. No lock of the state is held here: the destructor takes it.
  lock.reset();
  return current ? Status::OK() : internal::Invalidated("unlock", fname);
}

uint64_t FaultInjectionEnv::NowMicros() { return state_->NowMicros(); }

void FaultInjectionEnv::SleepForMicroseconds(uint64_t micros) { state_->Sleep(micros); }

void FaultInjectionEnv::SimulatePowerLoss() {
  state_->SimulateCrash(internal::FaultState::Crash::kPowerLoss);
}

void FaultInjectionEnv::SimulatePowerLossDropUnsynced() {
  state_->SimulateCrash(internal::FaultState::Crash::kPowerLossDropUnsynced);
}

void FaultInjectionEnv::SimulateProcessCrash() {
  state_->SimulateCrash(internal::FaultState::Crash::kProcess);
}

void FaultInjectionEnv::FailOpAfter(Op op, int successes) {
  SHALE_CHECK(successes >= 0, "FailOpAfter needs successes >= 0");
  state_->FailOpAfter(op, successes);
}

void FaultInjectionEnv::SetFailureProbability(Op op, double p) {
  SHALE_CHECK(p >= 0.0 && p <= 1.0, "a failure probability must be in [0, 1]");
  state_->SetFailureProbability(op, p);
}

void FaultInjectionEnv::SetReadCorruptionProbability(double p) {
  SHALE_CHECK(p >= 0.0 && p <= 1.0, "a read corruption probability must be in [0, 1]");
  state_->SetReadCorruptionProbability(p);
}

void FaultInjectionEnv::ClearFaults() { state_->ClearFaults(); }

uint64_t FaultInjectionEnv::crashes() const { return state_->crashes(); }

uint64_t FaultInjectionEnv::injected_errors() const { return state_->injected_errors(); }

uint64_t FaultInjectionEnv::injected_corruptions() const { return state_->injected_corruptions(); }

void FaultInjectionEnv::SetFileContents(const std::string& fname, std::string data) {
  state_->SetFileContents(fname, std::move(data));
}

std::string FaultInjectionEnv::FileContents(const std::string& fname) const {
  return state_->FileContents(fname);
}

}  // namespace shale
