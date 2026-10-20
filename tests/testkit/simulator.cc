#include "testkit/simulator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_state.h"
#include "db/db_impl.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/fault_env.h"
#include "testkit/invariants.h"
#include "testkit/model_db.h"
#include "util/check.h"
#include "util/dbformat.h"
#include "util/filename.h"
#include "util/hash.h"
#include "util/random.h"

namespace shale {

namespace {

using State = ModelDB::State;
using FaultOp = FaultInjectionEnv::Op;

constexpr const char* kDbName = "/db";

// What the Env puts in every error that it fails a call with on purpose.
constexpr const char* kInjectedMarker = "injected";

// What the LOG of an open says when it drops the zeros that a log or the MANIFEST ended in (D6),
// after the name of the file and the number of bytes; and how a line about the MANIFEST starts,
// after the time.
constexpr const char* kZeroTailText = " zero byte(s) at its end, from a record boundary";
constexpr const char* kManifestLine = " MANIFEST ";

// The limits of what a run holds open at once.
constexpr size_t kMaxSnapshots = 8;
constexpr size_t kMaxIterators = 4;

// A failure shows this many of the last operations.
constexpr size_t kTraceLines = 50;

// Read corruption, once enabled, is on for this many steps, and each read it touches is corrupted
// with this probability.
constexpr int kCorruptionSteps = 20;
constexpr double kCorruptionProbability = 0.05;

// Every this many steps, the whole database is read and compared with the model, and when nothing
// is pending the background work is run to the end and the directory is checked for orphan tables.
constexpr int kFullCheckEvery = 20;
constexpr int kIdleCheckEvery = 100;

// How many recently deleted keys are remembered, and how many of them are probed after a step.
constexpr size_t kRecentlyDeleted = 32;
constexpr size_t kDeletedProbes = 3;

// More background tasks than this in a row, with nothing but those tasks adding work, is a
// database that never becomes idle.
constexpr uint64_t kMaxIdleTasks = 100000;

// A value is this long, one time in kLargeValueOneIn, and else of a skewed length of up to 2 KiB.
constexpr size_t kLargeValue = 20 * 1024;
constexpr uint32_t kLargeValueOneIn = 50;
constexpr int kValueLengthBits = 11;
constexpr uint32_t kSyncPercent = 30;

// The generator of the driver is seeded apart from the Env's, so that the two streams do not walk
// in step.
constexpr uint64_t kDriverSalt = 0x7368616c65736d21ULL;

enum class Operation : uint8_t {
  kPut,
  kDelete,
  kBatch,
  kGet,
  kScan,
  kTakeSnapshot,
  kReadAtSnapshot,
  kReleaseSnapshot,
  kOpenIterator,
  kStepIterator,
  kFlush,
  kCompactRange,
  kBackgroundStep,
  kPowerLoss,
  kProcessCrash,
  kArmFailure,
  kEnableCorruption,
};

struct Weighted {
  Operation operation;
  uint32_t weight;
};

// What the driver does, and how often, in parts of 102.
constexpr Weighted kOperations[] = {
    {Operation::kPut, 30},
    {Operation::kDelete, 10},
    {Operation::kBatch, 8},
    {Operation::kGet, 15},
    {Operation::kScan, 5},
    {Operation::kTakeSnapshot, 2},
    {Operation::kReadAtSnapshot, 5},
    {Operation::kReleaseSnapshot, 2},
    {Operation::kOpenIterator, 2},
    {Operation::kStepIterator, 6},
    {Operation::kFlush, 2},
    {Operation::kCompactRange, 1},
    {Operation::kBackgroundStep, 10},
    {Operation::kPowerLoss, 1},
    {Operation::kProcessCrash, 1},
    {Operation::kArmFailure, 1},
    {Operation::kEnableCorruption, 1},
};

// Every kind of call that can be made to fail.
constexpr FaultOp kFaultOps[] = {
    FaultOp::kNewWritable,   FaultOp::kAppend,          FaultOp::kSync,    FaultOp::kClose,
    FaultOp::kRename,        FaultOp::kRemove,          FaultOp::kSyncDir, FaultOp::kRead,
    FaultOp::kNewSequential, FaultOp::kNewRandomAccess, FaultOp::kFlush,
};

const char* NameOf(FaultOp op) {
  switch (op) {
    case FaultOp::kNewWritable:
      return "NewWritableFile";
    case FaultOp::kAppend:
      return "Append";
    case FaultOp::kSync:
      return "Sync";
    case FaultOp::kClose:
      return "Close";
    case FaultOp::kRename:
      return "RenameFile";
    case FaultOp::kRemove:
      return "RemoveFile";
    case FaultOp::kSyncDir:
      return "SyncDir";
    case FaultOp::kRead:
      return "Read";
    case FaultOp::kNewSequential:
      return "NewSequentialFile";
    case FaultOp::kNewRandomAccess:
      return "NewRandomAccessFile";
    case FaultOp::kFlush:
      return "Flush";
  }
  return "?";
}

// How a database is made to stop, and be opened again.
enum class ReopenMode : uint8_t {
  kPowerLoss,     // the machine loses power: what was not synced may go
  kProcessCrash,  // the process dies: what was handed to the operating system stays
  kClose,         // the database is closed, and no crash is simulated
};

// ---------------------------------------------------------------------------------------------
// Small helpers.

uint64_t Mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

// A 64-bit digest of `bytes`: two chains of the repository's hash from different seeds.
uint64_t HashBytes(std::string_view bytes) {
  const uint64_t high = Hash(bytes.data(), bytes.size(), 0x6b657973);
  const uint64_t low = Hash(bytes.data(), bytes.size(), 0x76616c73);
  return (high << 32) | low;
}

// The key with number `i`.
std::string KeyName(int i) {
  char buffer[24];
  std::snprintf(buffer, sizeof buffer, "k%04d", i);
  return buffer;
}

// `bytes` quoted, with every byte that is not printable ASCII as \xNN, cut to 48 bytes with its
// length said, for a message.
std::string Show(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  constexpr size_t kShown = 48;
  std::string shown = "'";
  const size_t n = std::min(bytes.size(), kShown);
  for (size_t i = 0; i < n; i++) {
    const auto c = static_cast<unsigned char>(bytes[i]);
    if (c >= 0x20 && c <= 0x7e && c != '\'' && c != '\\') {
      shown.push_back(static_cast<char>(c));
    } else {
      shown += "\\x";
      shown.push_back(kHex[c >> 4]);
      shown.push_back(kHex[c & 0xf]);
    }
  }
  shown.push_back('\'');
  if (bytes.size() > n) {
    shown += "... (" + std::to_string(bytes.size()) + " bytes)";
  }
  return shown;
}

// An alarm of invariant I1, in the form that Classify() recognises.
Status AlarmI1(const std::string& message) { return Status::Corruption("I1: " + message); }

// The text of `s` without its code name, for a message that says what a check found.
std::string MessageOf(const Status& s) {
  const std::string text = s.ToString();
  const size_t colon = text.find(": ");
  return colon == std::string::npos ? text : text.substr(colon + 2);
}

// Enumerates what a batch does, in order: the key of each operation and whether it deletes.
class BatchOps final : public WriteBatch::Handler {
 public:
  void Put(std::string_view key, std::string_view /*value*/) override {
    ops.emplace_back(std::string(key), false);
  }
  void Delete(std::string_view key) override { ops.emplace_back(std::string(key), true); }

  std::vector<std::pair<std::string, bool>> ops;
};

// ---------------------------------------------------------------------------------------------

// A snapshot the run holds, with the state that the model had when it was taken.
struct SnapshotRecord {
  const Snapshot* snapshot = nullptr;
  std::shared_ptr<const State> view;
  uint64_t compactions_at = 0;  // the database's count of compactions when it was taken
  bool crossed = false;         // a compaction has run since, and was counted
};

// An iterator the run holds, with the state it must serve and where in it the iterator is. The
// iterator is on `position`, which has been checked: the next step moves both.
struct IteratorRecord {
  std::unique_ptr<Iterator> iterator;
  std::shared_ptr<const State> view;
  State::const_iterator position;
  uint64_t compactions_at = 0;
  bool crossed = false;
};

class Simulator {
 public:
  explicit Simulator(const SimOptions& options)
      : options_(options),
        rng_(options.seed ^ kDriverSalt),
        env_(std::make_unique<FaultInjectionEnv>(options.seed)) {
    db_options_.env = env_.get();
    db_options_.scheduler = &scheduler_;
    db_options_.create_if_missing = true;
    db_options_.write_buffer_size = 16384;
    db_options_.target_file_size = 16384;
    db_options_.max_bytes_for_level_base = 65536;
    db_options_.level_size_multiplier = 4;
    db_options_.l0_compaction_trigger = 2;
    db_options_.l0_slowdown_trigger = 4;
    db_options_.l0_stop_trigger = 6;
    db_options_.block_size = 1024;
    db_options_.block_restart_interval = 4;
    db_options_.block_cache_capacity = 65536;
    db_options_.max_open_files = 20;
  }

  Simulator(const Simulator&) = delete;
  Simulator& operator=(const Simulator&) = delete;

  // The database refuses to be closed with an iterator or a snapshot alive.
  ~Simulator() {
    if (db_ != nullptr) {
      ReleaseAll();
    }
  }

  SimResult Run() {
    ReopenMode mode = ReopenMode::kClose;
    bool damaged = false;
    if (OpenDb(&mode, &damaged)) {
      prev_sequence_ = impl_->TEST_LastSequence();
      for (int step = 0; step < options_.steps && !failed_; step++) {
        step_ = step;
        stats_.steps++;
        RunStep();
      }
      if (!failed_) {
        Finish();
      }
    }
    return MakeResult();
  }

 private:
  // ------------------------------------------------------------------------------------------
  // Failure, trace and counting.

  void Fail(const std::string& message) {
    if (!failed_) {
      failed_ = true;
      failure_ = message;
    }
  }

  void Mix(uint64_t value) { digest_ = Mix64(digest_ + 0x9e3779b97f4a7c15ULL + value); }

  // Adds one operation to the digest: its kind, its key, its outcome and the digest of the value it
  // returned or wrote.
  void Digest(const char* kind, std::string_view key, int code, uint64_t value_digest) {
    Mix(HashBytes(kind));
    Mix(HashBytes(key));
    Mix(static_cast<uint64_t>(code));
    Mix(value_digest);
  }

  // Adds a line about what the run just did to the list of the last operations.
  void Note(const std::string& line) {
    lines_.push_back("step " + std::to_string(step_) + ": " + line);
    if (lines_.size() > kTraceLines) {
      lines_.pop_front();
    }
  }

  // Records one operation, in the digest and in the list of the last operations, where `detail` is
  // what is worth saying of it besides its kind and key.
  void Trace(const char* kind, std::string_view key, int code, uint64_t value_digest,
             const std::string& detail = std::string()) {
    Digest(kind, key, code, value_digest);
    std::string line = kind;
    if (!key.empty()) {
      line += " " + std::string(key);
    }
    if (!detail.empty()) {
      line += " " + detail;
    }
    Note(line + " -> " + std::to_string(code));
  }

  // How many faults the Env has injected, of either kind: a call failed, a read corrupted. A call
  // that this has gone up across is one that a fault may explain.
  uint64_t FaultCount() const { return env_->injected_errors() + env_->injected_corruptions(); }

  // Remembers what the Env has injected so far, for an error that the step ends with to be
  // explained by what was injected since.
  void MarkStep() {
    step_errors_ = env_->injected_errors();
    step_corruptions_ = env_->injected_corruptions();
  }

  // What a check found, as the simulator acts on it.
  enum class Verdict : uint8_t {
    kPass,        // the invariant holds
    kReadFailed,  // the read failed for a fault that fired during it: no answer, no finding
    kRunFailed,   // the run has failed
  };

  // Decides what the result `s` of a check means. An alarm fails the run; a caller's bug fails the
  // run; a read that failed is allowed only if `fault_fired`, a fault having fired during the call,
  // and is an error otherwise.
  Verdict Judge(const Status& s, bool fault_fired, const std::string& what) {
    switch (Classify(s)) {
      case CheckOutcome::kOk:
        return Verdict::kPass;
      case CheckOutcome::kInvariantViolation:
        Fail(MessageOf(s));
        return Verdict::kRunFailed;
      case CheckOutcome::kCallerBug:
        Fail("the simulator misused a checker (" + what + "): " + MessageOf(s));
        return Verdict::kRunFailed;
      case CheckOutcome::kReadFailure:
        if (fault_fired) {
          stats_.read_failures++;
          return Verdict::kReadFailed;
        }
        Fail("I5: unexpected read error in " + what + ": " + s.ToString() +
             ", and no fault fired during it");
        return Verdict::kRunFailed;
    }
    return Verdict::kRunFailed;
  }

  // ------------------------------------------------------------------------------------------
  // Opening, closing and crashing.

  // Opens the database and installs it, or fails the run. An open that fails while a fault fired
  // during it is allowed, since a fault is what made it fail: the faults are cleared and the
  // database is opened again, which must work. One that fails with no fault fired, or when
  // nothing is armed, is a finding. `*damaged` is set if the open that worked had a read corrupted
  // during it.
  bool OpenDb(ReopenMode* mode, bool* damaged) {
    *damaged = false;
    for (int attempt = 0; attempt < 2; attempt++) {
      std::unique_ptr<DB> real;
      const uint64_t before = FaultCount();
      const uint64_t corruptions_before = env_->injected_corruptions();
      const Status s = DB::Open(db_options_, kDbName, &real);
      if (s.ok()) {
        *damaged = env_->injected_corruptions() > corruptions_before;
        impl_ = dynamic_cast<DBImpl*>(real.get());
        SHALE_CHECK(impl_ != nullptr, "DB::Open returned something that is not a DBImpl");
        db_ = options_.wrap_db ? options_.wrap_db(std::move(real)) : std::move(real);
        counters_pending_ = true;
        CountZeroTails();
        // An open with no fault armed sweeps every table that no version names.
        if (!faults_armed_) {
          remove_dirty_ = false;
        }
        unswept_ = false;
        return true;
      }
      if (attempt == 0 && FaultCount() > before) {
        stats_.open_failures++;
        Trace("open failed", "", static_cast<int>(s.code()), 0, s.ToString());
        // The machine may go down again before the recovery is tried again, with what the failed
        // open left in the directory. After a power loss what was acknowledged without a sync
        // is no longer certain to be there.
        if (options_.crashes && rng_.OneIn(2)) {
          if (rng_.OneIn(2)) {
            LosePower();
            *mode = ReopenMode::kPowerLoss;
          } else {
            stats_.process_crashes++;
            env_->SimulateProcessCrash();
            if (*mode == ReopenMode::kClose) {
              *mode = ReopenMode::kProcessCrash;
            }
          }
        }
        ClearFaults();
        continue;
      }
      Fail("I4: opening the database failed " +
           std::string(attempt == 0 ? "with no fault fired during it" : "with the faults cleared") +
           ": " + s.ToString());
      return false;
    }
    return false;
  }

  // Counts the open that just worked if its LOG says that it dropped zeros at the end of a file. An
  // open renames the LOG of the one before, so the LOG holds this open's lines.
  void CountZeroTails() {
    const std::string log = env_->FileContents(InfoLogFileName(kDbName));
    bool any = false;
    bool manifest = false;
    for (size_t start = 0; start < log.size();) {
      const size_t end = std::min(log.find('\n', start), log.size());
      const std::string_view line(log.data() + start, end - start);
      if (line.find(kZeroTailText) != std::string_view::npos) {
        any = true;
        manifest = manifest || line.find(kManifestLine) != std::string_view::npos;
      }
      start = end + 1;
    }
    if (any) {
      stats_.zero_tail_opens++;
    }
    if (manifest) {
      stats_.manifest_zero_tail_opens++;
    }
  }

  // Simulates a power loss: one time in three the worst case, where nothing that was not made
  // durable survives, and else one where a prefix of it, or zeros in its place, survive.
  void LosePower() {
    stats_.power_losses++;
    if (rng_.OneIn(3)) {
      env_->SimulatePowerLossDropUnsynced();
    } else {
      env_->SimulatePowerLoss();
    }
  }

  void ClearFaults() {
    env_->ClearFaults();
    faults_armed_ = false;
    corruption_left_ = 0;
  }

  // Gives back every snapshot and iterator, which a database that is closed or crashed must not
  // have outstanding.
  void ReleaseAll() {
    const bool had_iterators = !iterators_.empty();
    iterators_.clear();
    for (const SnapshotRecord& record : snapshots_) {
      db_->ReleaseSnapshot(record.snapshot);
    }
    snapshots_.clear();
    if (had_iterators) {
      NoteIteratorReleased();
    }
  }

  // Adds what the database counted to the totals of the run, once per database.
  void AccumulateCounters() {
    if (!counters_pending_) {
      return;
    }
    counters_pending_ = false;
    const DBCounters c = impl_->TEST_Counters();
    total_.flushes += c.flushes;
    total_.compactions += c.compactions;
    total_.trivial_moves += c.trivial_moves;
    total_.tombstones_dropped += c.tombstones_dropped;
    total_.shadowed_dropped += c.shadowed_dropped;
    total_.user_bytes_written += c.user_bytes_written;
    total_.wal_bytes_written += c.wal_bytes_written;
    total_.flush_bytes_written += c.flush_bytes_written;
    total_.compaction_bytes_read += c.compaction_bytes_read;
    total_.compaction_bytes_written += c.compaction_bytes_written;
    total_.write_groups += c.write_groups;
    total_.wal_syncs += c.wal_syncs;
    total_.write_slowdowns += c.write_slowdowns;
    total_.write_stops += c.write_stops;
    total_.bg_errors += c.bg_errors;
  }

  // Stops the database as `mode` says, opens it again, and checks what it holds against what a
  // stop of that kind may leave (I4). The model goes on from what it came back with. Returns
  // whether the run is still going.
  bool Reopen(ReopenMode mode) {
    ReleaseAll();
    AccumulateCounters();
    switch (mode) {
      case ReopenMode::kPowerLoss:
        LosePower();
        break;
      case ReopenMode::kProcessCrash:
        stats_.process_crashes++;
        env_->SimulateProcessCrash();
        break;
      case ReopenMode::kClose:
        break;
    }
    // The crash comes first, then the database is dropped: what it does when it is destroyed goes
    // through handles that the crash has invalidated, as a dead process would not have made it.
    db_.reset();
    impl_ = nullptr;

    // Read corruption is off for the open, except on one process crash in four, when it is on
    // for the open alone (D6: a bit flipped in what the open reads must fail it, and then
    // nothing is lost, which the clean open that follows shows by I4; or the open works, and
    // then it holds I4 in full, whether or not a read was corrupted in it, since the reader of a
    // log finds a length that a flipped bit has damaged when the record is whole). The faults of
    // the write side may stay armed, and are cleared half the time, so that the open then runs
    // with nothing armed.
    env_->SetReadCorruptionProbability(0.0);
    corruption_left_ = 0;
    if (rng_.OneIn(2)) {
      ClearFaults();
    }
    const bool corrupt_open =
        mode == ReopenMode::kProcessCrash && options_.read_corruption && rng_.OneIn(4);
    if (corrupt_open) {
      env_->SetReadCorruptionProbability(kCorruptionProbability);
    }
    bool damaged = false;
    const bool opened = OpenDb(&mode, &damaged);
    env_->SetReadCorruptionProbability(0.0);
    Note(std::string("opened after ") +
         (mode == ReopenMode::kPowerLoss
              ? "a power loss"
              : (mode == ReopenMode::kProcessCrash ? "a process crash" : "a close")) +
         (corrupt_open ? ", with read corruption armed" : "") +
         (damaged ? ", and a read was corrupted during it" : ""));
    if (!opened) {
      return false;
    }
    stats_.reopens++;
    if (damaged) {
      stats_.damaged_opens++;
    }

    // The sequence number may go back across a crash, but not below that of the newest write that
    // was made durable. It is asked first: a database that has lost where it was shows here, and
    // not only as the writes it then fails to have.
    const Status version =
        CheckVersion(env_.get(), kDbName, impl_->TEST_VersionState(), impl_->TEST_LiveFiles(),
                     impl_->TEST_LastSequence(), durable_sequence_);
    if (Judge(version, false, "the version after a reopen") != Verdict::kPass) {
      return false;
    }

    // After a process crash everything that was acknowledged is there; after a power loss nothing
    // is certain but what was synced.
    const size_t min_prefix = mode == ReopenMode::kPowerLoss ? 0 : acked_;
    State matched;
    if (!CheckRecovery(min_prefix, &matched)) {
      return false;
    }
    Trace("reopened", "", 0, ModelDB::Digest(matched),
          std::string(mode == ReopenMode::kPowerLoss
                          ? "after a power loss"
                          : (mode == ReopenMode::kProcessCrash ? "after a process crash"
                                                               : "after a close")));
    model_.Reset(matched);
    durable_ = std::move(matched);
    uncertain_.clear();
    acked_ = 0;
    error_pending_ = false;
    error_text_.clear();

    // What recovery brought back is durable now: it was written to a table and the MANIFEST that
    // names it was synced before the open returned.
    durable_sequence_ = impl_->TEST_LastSequence();
    prev_sequence_ = durable_sequence_;

    if (!faults_armed_) {
      CheckOrphans();
    }
    return !failed_;
  }

  // I4: the database came back with the durable state and a prefix of the writes since. A read
  // that a fault made fail is allowed, and the check is made again with the faults cleared.
  bool CheckRecovery(size_t min_prefix, State* matched) {
    for (int attempt = 0; attempt < 2; attempt++) {
      const uint64_t before = FaultCount();
      const Status s = CheckRecoveredPrefix(db_.get(), durable_, uncertain_, min_prefix, matched);
      const bool fired = FaultCount() > before && attempt == 0;
      const Verdict verdict = Judge(s, fired, "the check of the recovered state");
      if (verdict == Verdict::kPass) {
        return true;
      }
      if (verdict == Verdict::kRunFailed) {
        return false;
      }
      ClearFaults();
    }
    return false;
  }

  // ------------------------------------------------------------------------------------------
  // The steps.

  Operation PickOperation() {
    uint32_t total = 0;
    for (const Weighted& entry : kOperations) {
      total += WeightOf(entry);
    }
    uint32_t pick = rng_.Uniform(total);
    for (const Weighted& entry : kOperations) {
      const uint32_t weight = WeightOf(entry);
      if (pick < weight) {
        return entry.operation;
      }
      pick -= weight;
    }
    return Operation::kGet;
  }

  uint32_t WeightOf(const Weighted& entry) const {
    switch (entry.operation) {
      case Operation::kPowerLoss:
      case Operation::kProcessCrash:
        return options_.crashes ? entry.weight : 0;
      case Operation::kArmFailure:
        return options_.io_errors ? entry.weight : 0;
      case Operation::kEnableCorruption:
        return options_.read_corruption ? entry.weight : 0;
      default:
        return entry.weight;
    }
  }

  void RunStep() {
    MarkStep();
    if (corruption_left_ > 0 && --corruption_left_ == 0) {
      env_->SetReadCorruptionProbability(0.0);
    }
    switch (PickOperation()) {
      case Operation::kPut:
        DoPut();
        break;
      case Operation::kDelete:
        DoDelete();
        break;
      case Operation::kBatch:
        DoBatch();
        break;
      case Operation::kGet:
        CheckedGet(RandomKey(), nullptr, model_.state(), "get");  // one draw: see kScan
        break;
      case Operation::kScan: {
        // One draw to a statement: the order in which the arguments of a call are evaluated is not
        // fixed, and a run must be the same under every compiler.
        const std::string start = RandomScanStart();
        const size_t limit = 1 + rng_.Skewed(7);
        CheckedScan(start, nullptr, model_.state(), limit, "scan");
        break;
      }
      case Operation::kTakeSnapshot:
        DoTakeSnapshot();
        break;
      case Operation::kReadAtSnapshot:
        DoReadAtSnapshot();
        break;
      case Operation::kReleaseSnapshot:
        DoReleaseSnapshot();
        break;
      case Operation::kOpenIterator:
        DoOpenIterator();
        break;
      case Operation::kStepIterator:
        DoStepIterator();
        break;
      case Operation::kFlush:
        DoFlush();
        break;
      case Operation::kCompactRange:
        DoCompactRange();
        break;
      case Operation::kBackgroundStep:
        Trace("background step", "", scheduler_.RunOne() ? 1 : 0, 0);
        break;
      case Operation::kPowerLoss:
        Reopen(ReopenMode::kPowerLoss);
        break;
      case Operation::kProcessCrash:
        Reopen(ReopenMode::kProcessCrash);
        break;
      case Operation::kArmFailure:
        DoArmFailure();
        break;
      case Operation::kEnableCorruption:
        env_->SetReadCorruptionProbability(kCorruptionProbability);
        corruption_left_ = kCorruptionSteps + 1;
        Trace("enable corruption", "", 0, 0);
        break;
    }
    if (!failed_) {
      AfterStep();
    }
  }

  // The checks that follow every step.
  void AfterStep() {
    UpdateCrossings();
    NoticeBackgroundError();
    CheckVersionNow();
    if (failed_) {
      return;
    }
    if (!error_pending_) {
      ProbeDeletedKeys();
      if (step_ % kFullCheckEvery == kFullCheckEvery - 1) {
        FullReadCheck();
      }
      if (!failed_ && step_ % kIdleCheckEvery == kIdleCheckEvery - 1) {
        RunToIdleAndCheckOrphans();
      }
    }
    if (!failed_ && error_pending_) {
      HandleError();
    }
  }

  // I2, for the database as it is. The sequence number it has reached must not be below what it
  // was at the last check.
  void CheckVersionNow() {
    const SequenceNumber sequence = impl_->TEST_LastSequence();
    const VersionState state = impl_->TEST_VersionState();
    for (size_t level = 0; level < state.files.size(); level++) {
      if (!state.files[level].empty()) {
        stats_.deepest_level = std::max(stats_.deepest_level, static_cast<uint64_t>(level));
      }
    }
    const Status s =
        CheckVersion(env_.get(), kDbName, state, impl_->TEST_LiveFiles(), sequence, prev_sequence_);
    Judge(s, false, "the version");
    prev_sequence_ = sequence;
  }

  // Counts the snapshots and iterators that were alive while a compaction ran, once each.
  void UpdateCrossings() {
    if (snapshots_.empty() && iterators_.empty()) {
      return;
    }
    const uint64_t compactions = impl_->TEST_Counters().compactions;
    for (SnapshotRecord& record : snapshots_) {
      if (!record.crossed && compactions > record.compactions_at) {
        record.crossed = true;
        stats_.snapshots_across_compaction++;
      }
    }
    for (IteratorRecord& record : iterators_) {
      if (!record.crossed && compactions > record.compactions_at) {
        record.crossed = true;
        stats_.iterators_across_compaction++;
      }
    }
  }

  // The database stops writes at its first error of the log, a flush or a compaction, which a
  // background task may meet where no call returns it.
  void NoticeBackgroundError() {
    if (error_pending_) {
      return;
    }
    std::string value;
    SHALE_CHECK(db_->GetProperty("shale.background-error", &value),
                "the database does not have the property shale.background-error");
    if (value != "OK") {
      error_pending_ = true;
      error_text_ = "background error: " + value;
    }
  }

  // The database has stopped writing, and an injected fault must be why: one that fired during
  // this step, and, where only calls were failed and no read was corrupted, an error that says so,
  // since the Env marks every failure it injects and a database that turns it into something else
  // has lost what went wrong. The database goes on serving reads while it is stopped, and they
  // must be right (D4, D5): they are read. The next write must fail too (I6), and then the
  // database is closed, or crashed, and opened again.
  void HandleError() {
    stats_.write_failures++;
    const uint64_t errors = env_->injected_errors() - step_errors_;
    const uint64_t corruptions = env_->injected_corruptions() - step_corruptions_;
    if (errors == 0 && corruptions == 0) {
      Fail("I5: unexpected error, " + error_text_ + ", and no fault fired during this step");
      return;
    }
    if (corruptions == 0 && error_text_.find(kInjectedMarker) == std::string::npos) {
      Fail("I5: unexpected error, " + error_text_ +
           ", which does not say it was injected, "
           "although only failed calls and no corrupted read fired during this step");
      return;
    }
    WriteBatch probe;
    probe.Put("probe", "after an error");
    WriteOptions write_options;
    write_options.sync = rng_.OneIn(2);
    const Status s = db_->Write(write_options, &probe);
    Trace("probe write", "probe", static_cast<int>(s.code()), 0);
    if (s.ok()) {
      Fail("I6: a write succeeded after the database stopped writes (" + error_text_ + ")");
      return;
    }
    FullReadCheck();
    if (failed_) {
      return;
    }
    // Most of the time the power goes: the files then hold what the failed work left, as far as
    // it was synced and a prefix of the rest, and recovery has to make sense of them.
    ReopenMode mode = ReopenMode::kClose;
    if (options_.crashes) {
      const uint32_t pick = rng_.Uniform(6);
      mode = pick < 4 ? ReopenMode::kPowerLoss
                      : (pick == 4 ? ReopenMode::kProcessCrash : ReopenMode::kClose);
    }
    Reopen(mode);
  }

  // ------------------------------------------------------------------------------------------
  // Writes.

  std::string RandomKey() {
    return KeyName(static_cast<int>(rng_.Uniform(static_cast<uint32_t>(options_.key_space))));
  }

  // A key to start a scan at: any in the key space, or a little past its end.
  std::string RandomScanStart() {
    return KeyName(static_cast<int>(rng_.Uniform(static_cast<uint32_t>(options_.key_space) + 3)));
  }

  // A value of a skewed length, and now and then a large one, that begins with the number of the
  // write so that no two values of any size are alike.
  std::string RandomValue() {
    const size_t length =
        rng_.OneIn(kLargeValueOneIn) ? kLargeValue : rng_.Skewed(kValueLengthBits);
    std::string value(length, '\0');
    for (size_t i = 0; i < length; i += 8) {
      const uint64_t bits = rng_.Next();
      for (size_t j = 0; j < 8 && i + j < length; j++) {
        value[i + j] = static_cast<char>((bits >> (8 * j)) & 0xff);
      }
    }
    const uint64_t stamp = ++write_number_;
    for (size_t j = 0; j < 8 && j < length; j++) {
      value[j] = static_cast<char>((stamp >> (8 * j)) & 0xff);
    }
    return value;
  }

  void DoPut() {
    std::string key = RandomKey();
    std::string value = RandomValue();
    const uint64_t digest = HashBytes(value);
    const size_t length = value.size();
    WriteBatch batch;
    batch.Put(key, value);
    Submit(std::move(batch), "put", key, digest, "len=" + std::to_string(length));
  }

  void DoDelete() {
    const std::string key = RandomKey();
    WriteBatch batch;
    batch.Delete(key);
    Submit(std::move(batch), "delete", key, 0, "");
  }

  void DoBatch() {
    const uint32_t count = 2 + rng_.Uniform(7);
    WriteBatch batch;
    uint64_t digest = 0;
    std::string detail = "ops=";
    for (uint32_t i = 0; i < count; i++) {
      const std::string key = RandomKey();
      if (rng_.Uniform(10) < 7) {
        const std::string value = RandomValue();
        batch.Put(key, value);
        digest = Mix64(digest + HashBytes(key) + HashBytes(value));
        detail += "P";
      } else {
        batch.Delete(key);
        digest = Mix64(digest + HashBytes(key));
        detail += "D";
      }
    }
    Submit(std::move(batch), "batch", "", digest, detail);
  }

  // Writes `batch`, with or without a sync. A write that is acknowledged is applied to the model;
  // one that is acknowledged with a sync makes the model durable. One that fails is kept among the
  // writes that a crash may or may not have kept, and the database has stopped.
  void Submit(WriteBatch batch, const char* kind, const std::string& key, uint64_t digest,
              const std::string& detail) {
    const bool sync = rng_.Uniform(100) < kSyncPercent;
    stats_.writes++;
    if (sync) {
      stats_.sync_writes++;
    }
    WriteBatch sent = batch;  // the database numbers the batch it is given, which changes it
    WriteOptions write_options;
    write_options.sync = sync;
    const Status s = db_->Write(write_options, &sent);
    Trace(kind, key, static_cast<int>(s.code()), digest, detail + (sync ? " sync" : " nosync"));
    uncertain_.push_back(std::move(batch));
    if (!s.ok()) {
      error_pending_ = true;
      error_text_ = std::string(kind) + " failed: " + s.ToString();
      return;
    }
    acked_++;
    model_.Apply(uncertain_.back());
    NoteWritten(uncertain_.back());
    if (sync) {
      MarkDurable();
    }
  }

  // The writes so far are durable: a write that was synced has been acknowledged, or the memtable
  // has been written out. Everything before it is made durable with it.
  void MarkDurable() {
    durable_ = model_.state();
    uncertain_.clear();
    acked_ = 0;
    durable_sequence_ = impl_->TEST_LastSequence();
  }

  // Remembers the keys that `batch` deleted, and forgets those it wrote.
  void NoteWritten(const WriteBatch& batch) {
    BatchOps ops;
    const Status s = batch.Iterate(&ops);
    SHALE_CHECK(s.ok(), "a batch the simulator built does not decode: " + s.ToString());
    for (const auto& [key, deleted] : ops.ops) {
      const auto found = std::find(recently_deleted_.begin(), recently_deleted_.end(), key);
      if (found != recently_deleted_.end()) {
        recently_deleted_.erase(found);
      }
      if (deleted) {
        recently_deleted_.push_back(key);
        if (recently_deleted_.size() > kRecentlyDeleted) {
          recently_deleted_.erase(recently_deleted_.begin());
        }
      }
    }
  }

  void DoFlush() {
    const Status s = db_->Flush();
    Trace("flush", "", static_cast<int>(s.code()), 0);
    if (s.ok()) {
      MarkDurable();
    } else {
      error_pending_ = true;
      error_text_ = "flush failed: " + s.ToString();
    }
  }

  void DoCompactRange() {
    std::optional<std::string> begin;
    std::optional<std::string> end;
    if (rng_.OneIn(2)) {
      int low = static_cast<int>(rng_.Uniform(static_cast<uint32_t>(options_.key_space)));
      int high = static_cast<int>(rng_.Uniform(static_cast<uint32_t>(options_.key_space)));
      if (low > high) {
        std::swap(low, high);
      }
      begin = KeyName(low);
      end = KeyName(high);
    }
    const Status s = db_->CompactRange(
        begin.has_value() ? std::optional<std::string_view>(*begin) : std::nullopt,
        end.has_value() ? std::optional<std::string_view>(*end) : std::nullopt);
    Trace("compact range", begin.value_or(""), static_cast<int>(s.code()), 0,
          end.value_or("") + (begin.has_value() ? "" : "everything"));
    if (s.ok()) {
      // It writes the memtable out first, so everything written before is in a table.
      MarkDurable();
    } else {
      error_pending_ = true;
      error_text_ = "compact range failed: " + s.ToString();
    }
  }

  void DoArmFailure() {
    const FaultOp op = kFaultOps[rng_.Uniform(static_cast<uint32_t>(std::size(kFaultOps)))];
    const int successes = static_cast<int>(rng_.Skewed(5));
    env_->FailOpAfter(op, successes);
    faults_armed_ = true;
    if (op == FaultOp::kRemove) {
      remove_dirty_ = true;
    }
    Trace("arm failure", NameOf(op), successes, 0);
  }

  // ------------------------------------------------------------------------------------------
  // Reads, and I1.

  // Gets `key` as of `snapshot` and compares the answer with `view`.
  void CheckedGet(const std::string& key, const Snapshot* snapshot, const State& view,
                  const char* what, bool note = true) {
    ReadOptions read_options;
    read_options.snapshot = snapshot;
    std::string value;
    const uint64_t before = FaultCount();
    const Status s = db_->Get(read_options, key, &value);
    const bool fired = FaultCount() > before;
    const std::string where = snapshot != nullptr ? " at a snapshot" : "";
    const auto want = view.find(key);
    uint64_t digest = 0;
    if (s.ok()) {
      digest = HashBytes(value);
      if (want == view.end()) {
        Fail("I1: " + std::string(what) + ": key " + Show(key) + where + " returned value " +
             Show(value) + ", expected nothing");
      } else if (value != want->second) {
        Fail("I1: " + std::string(what) + ": key " + Show(key) + where + " returned value " +
             Show(value) + ", expected " + Show(want->second));
      }
    } else if (s.IsNotFound()) {
      if (want != view.end()) {
        Fail("I1: " + std::string(what) + ": key " + Show(key) + where +
             " found nothing, expected value " + Show(want->second));
      }
    } else {
      Judge(s, fired, what);
    }
    if (note) {
      Trace(what, key, static_cast<int>(s.code()), digest);
    } else {
      Digest(what, key, static_cast<int>(s.code()), digest);
    }
  }

  // Compares the entry that `iterator` is on with `*want`, the entry of `view` it must be on, or
  // with the end of the data if `*want` is its end. OK if they agree; an alarm of I1 if the answer
  // is wrong; and the iterator's own failure, which is not an answer, if it has failed.
  static Status CheckAt(const Iterator& iterator, const State& view, State::const_iterator want,
                        uint64_t* digest) {
    if (want == view.end()) {
      if (iterator.Valid()) {
        return AlarmI1("an iterator is on key " + Show(iterator.key()) +
                       " after the last entry of the model");
      }
      return iterator.status();
    }
    if (!iterator.Valid()) {
      if (!iterator.status().ok()) {
        return iterator.status();
      }
      return AlarmI1("an iterator ended before key " + Show(want->first));
    }
    if (iterator.key() < want->first) {
      return AlarmI1("an iterator is on key " + Show(iterator.key()) +
                     " that the model does not hold, before " + Show(want->first));
    }
    if (iterator.key() > want->first) {
      return AlarmI1("an iterator skipped key " + Show(want->first) + " and is on " +
                     Show(iterator.key()));
    }
    if (iterator.value() != want->second) {
      return AlarmI1("an iterator has value " + Show(iterator.value()) + " for key " +
                     Show(want->first) + ", expected " + Show(want->second));
    }
    *digest = Mix64(*digest + HashBytes(iterator.key()) + HashBytes(iterator.value()));
    return Status::OK();
  }

  // Scans from `start` as of `snapshot`, up to `limit` entries or the end, and compares what it
  // serves with `view`.
  void CheckedScan(const std::string& start, const Snapshot* snapshot, const State& view,
                   size_t limit, const char* what) {
    ReadOptions read_options;
    read_options.snapshot = snapshot;
    const uint64_t before = FaultCount();
    Status result;
    uint64_t digest = 0;
    {
      const std::unique_ptr<Iterator> iterator = db_->NewIterator(read_options);
      iterator->Seek(start);
      State::const_iterator want = view.lower_bound(start);
      for (size_t i = 0; i < limit; i++) {
        result = CheckAt(*iterator, view, want, &digest);
        if (!result.ok() || want == view.end()) {
          break;
        }
        iterator->Next();
        ++want;
      }
    }
    Judge(result, FaultCount() > before,
          std::string(what) + (snapshot != nullptr ? " at a snapshot" : ""));
    Trace(what, start, static_cast<int>(result.code()), digest, "limit=" + std::to_string(limit));
  }

  // Gets up to kDeletedProbes of the keys that were deleted lately and that the model does not
  // hold, so that a tombstone that has been dropped while it still shadowed a value would show: a
  // scan hides what it does not serve, and a Get of the key is what finds it.
  void ProbeDeletedKeys() {
    if (recently_deleted_.empty()) {
      return;
    }
    std::string probed;
    for (size_t i = 0; i < kDeletedProbes && !failed_; i++) {
      const std::string key =
          recently_deleted_[rng_.Uniform(static_cast<uint32_t>(recently_deleted_.size()))];
      if (model_.Get(key).has_value()) {
        continue;
      }
      CheckedGet(key, nullptr, model_.state(), "probe of a deleted key", /*note=*/false);
      probed += " " + key;
    }
    if (!probed.empty()) {
      Note("probe of deleted keys:" + probed);
    }
  }

  // Every key and the whole scan against the model, and the same at one snapshot.
  void FullReadCheck() {
    FullCheck(nullptr, model_.state(), "the full check");
    if (!failed_ && !snapshots_.empty()) {
      const SnapshotRecord& record =
          snapshots_[rng_.Uniform(static_cast<uint32_t>(snapshots_.size()))];
      FullCheck(record.snapshot, *record.view, "the full check at a snapshot");
    }
  }

  void FullCheck(const Snapshot* snapshot, const State& view, const char* what) {
    const uint64_t before = FaultCount();
    const Status s = CheckReadsMatch(db_.get(), view, snapshot);
    Judge(s, FaultCount() > before, what);
    Trace(what, "", static_cast<int>(s.code()), ModelDB::Digest(view));
  }

  // ------------------------------------------------------------------------------------------
  // Snapshots and iterators.

  void DoTakeSnapshot() {
    if (snapshots_.size() >= kMaxSnapshots) {
      Trace("take snapshot", "", 1, 0, "too many");
      return;
    }
    SnapshotRecord record;
    record.snapshot = db_->GetSnapshot();
    record.view = std::make_shared<const State>(model_.state());
    record.compactions_at = impl_->TEST_Counters().compactions;
    snapshots_.push_back(std::move(record));
    Trace("take snapshot", "", 0, ModelDB::Digest(model_.state()));
  }

  void DoReadAtSnapshot() {
    if (snapshots_.empty()) {
      Trace("read at snapshot", "", 1, 0, "none");
      return;
    }
    const SnapshotRecord record =
        snapshots_[rng_.Uniform(static_cast<uint32_t>(snapshots_.size()))];
    const uint32_t gets = 1 + rng_.Uniform(4);
    for (uint32_t i = 0; i < gets && !failed_; i++) {
      const std::string key = RandomKey();
      CheckedGet(key, record.snapshot, *record.view, "get at a snapshot");
    }
    if (!failed_) {
      const std::string start = RandomScanStart();
      const size_t limit = 1 + rng_.Skewed(6);
      CheckedScan(start, record.snapshot, *record.view, limit, "scan at a snapshot");
    }
  }

  void DoReleaseSnapshot() {
    if (snapshots_.empty()) {
      Trace("release snapshot", "", 1, 0, "none");
      return;
    }
    const size_t index = rng_.Uniform(static_cast<uint32_t>(snapshots_.size()));
    // A last look at what the snapshot saw, with everything that has happened since.
    FullCheck(snapshots_[index].snapshot, *snapshots_[index].view, "the check at a release");
    if (failed_) {
      return;
    }
    db_->ReleaseSnapshot(snapshots_[index].snapshot);
    snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(index));
    Trace("release snapshot", "", 0, 0);
  }

  void DoOpenIterator() {
    if (iterators_.size() >= kMaxIterators) {
      Trace("open iterator", "", 1, 0, "too many");
      return;
    }
    ReadOptions read_options;
    IteratorRecord record;
    if (!snapshots_.empty() && rng_.OneIn(4)) {
      const SnapshotRecord& snapshot =
          snapshots_[rng_.Uniform(static_cast<uint32_t>(snapshots_.size()))];
      read_options.snapshot = snapshot.snapshot;
      record.view = snapshot.view;
    } else {
      record.view = std::make_shared<const State>(model_.state());
    }
    record.compactions_at = impl_->TEST_Counters().compactions;

    const uint64_t before = FaultCount();
    record.iterator = db_->NewIterator(read_options);
    std::string start;
    if (rng_.OneIn(2)) {
      record.iterator->SeekToFirst();
      record.position = record.view->begin();
    } else {
      start = RandomScanStart();
      record.iterator->Seek(start);
      record.position = record.view->lower_bound(start);
    }
    uint64_t digest = 0;
    const Status s = CheckAt(*record.iterator, *record.view, record.position, &digest);
    const Verdict verdict = Judge(s, FaultCount() > before, "an iterator after it was opened");
    Trace("open iterator", start, static_cast<int>(s.code()), digest);
    // One that failed, or that has nothing to serve, is of no use to hold.
    if (verdict == Verdict::kPass && record.position != record.view->end()) {
      iterators_.push_back(std::move(record));
    } else if (verdict != Verdict::kRunFailed) {
      record.iterator.reset();
      NoteIteratorReleased();
    }
  }

  void DoStepIterator() {
    if (iterators_.empty()) {
      Trace("step iterator", "", 1, 0, "none");
      return;
    }
    const size_t index = rng_.Uniform(static_cast<uint32_t>(iterators_.size()));
    IteratorRecord& record = iterators_[index];
    const uint32_t steps = 1 + rng_.Uniform(10);
    const uint64_t before = FaultCount();
    Status result;
    uint64_t digest = 0;
    for (uint32_t i = 0; i < steps && record.position != record.view->end(); i++) {
      record.iterator->Next();
      ++record.position;
      result = CheckAt(*record.iterator, *record.view, record.position, &digest);
      if (!result.ok()) {
        break;
      }
    }
    const Verdict verdict = Judge(result, FaultCount() > before, "an iterator step");
    Trace("step iterator", "", static_cast<int>(result.code()), digest,
          "steps=" + std::to_string(steps));
    if (verdict == Verdict::kRunFailed) {
      return;
    }
    if (verdict == Verdict::kReadFailed || record.position == record.view->end()) {
      iterators_.erase(iterators_.begin() + static_cast<std::ptrdiff_t>(index));
      NoteIteratorReleased();
    }
  }

  // An iterator that is gone leaves the tables of the version it held for the next sweep, so
  // until one runs, a table in the directory that no version names is not an orphan.
  void NoteIteratorReleased() {
    const DBCounters c = impl_->TEST_Counters();
    unswept_ = true;
    unswept_baseline_ = c.flushes + c.compactions;
  }

  // Whether an iterator has been released and no sweep has run since: a flush and a compaction
  // each end in one.
  bool Unswept() {
    if (!unswept_) {
      return false;
    }
    const DBCounters c = impl_->TEST_Counters();
    if (c.flushes + c.compactions > unswept_baseline_) {
      unswept_ = false;
    }
    return unswept_;
  }

  // ------------------------------------------------------------------------------------------
  // I3, and the end of the run.

  // Runs every background task there is, and then looks for tables that nothing names.
  void RunToIdleAndCheckOrphans() {
    uint64_t ran = 0;
    while (scheduler_.RunOne()) {
      if (++ran > kMaxIdleTasks) {
        Fail("background work did not become idle after " + std::to_string(kMaxIdleTasks) +
             " tasks");
        return;
      }
    }
    Trace("run to idle", "", 0, ran);
    NoticeBackgroundError();
    if (!error_pending_) {
      CheckOrphans();
    }
  }

  // I3, at a moment when nothing can explain a table left behind: no background work runs, no
  // error is set, no failed RemoveFile may have left one for the next sweep, and no iterator has
  // been released without a sweep since. Where that cannot be told, it is not checked.
  void CheckOrphans() {
    if (error_pending_ || remove_dirty_ || Unswept()) {
      return;
    }
    stats_.orphan_checks++;
    const Status s = CheckNoOrphanTables(env_.get(), kDbName, impl_->TEST_LiveFiles());
    Judge(s, false, "the check for orphan tables");
  }

  // The last checks of a run, with every fault cleared so that nothing can fail without a finding:
  // the iterators that are still held are read to their ends, then the whole database, then every
  // snapshot, and the directory.
  void Finish() {
    ClearFaults();
    env_->SetReadCorruptionProbability(0.0);
    MarkStep();
    for (IteratorRecord& record : iterators_) {
      uint64_t digest = 0;
      Status result;
      while (record.position != record.view->end() && result.ok()) {
        record.iterator->Next();
        ++record.position;
        result = CheckAt(*record.iterator, *record.view, record.position, &digest);
      }
      if (Judge(result, false, "an iterator read to its end") != Verdict::kPass) {
        return;
      }
    }
    FullCheck(nullptr, model_.state(), "the final check");
    for (size_t i = 0; i < snapshots_.size() && !failed_; i++) {
      FullCheck(snapshots_[i].snapshot, *snapshots_[i].view, "the final check at a snapshot");
    }
    if (failed_) {
      return;
    }
    CheckVersionNow();
    if (!failed_) {
      RunToIdleAndCheckOrphans();
    }
    if (!failed_ && error_pending_) {
      Fail("I5: unexpected error at the end of the run, " + error_text_ +
           ", with every fault cleared");
    }
  }

  SimResult MakeResult() {
    if (impl_ != nullptr && db_ != nullptr) {
      AccumulateCounters();
    }
    stats_.flushes = total_.flushes;
    stats_.compactions = total_.compactions;
    stats_.trivial_moves = total_.trivial_moves;
    stats_.tombstones_dropped = total_.tombstones_dropped;
    stats_.injected_errors = env_->injected_errors();
    stats_.injected_corruptions = env_->injected_corruptions();

    // The digest ends with the counters: what the operations did and what the engine counted.
    for (const uint64_t counter : {stats_.steps,
                                   stats_.writes,
                                   stats_.sync_writes,
                                   stats_.power_losses,
                                   stats_.process_crashes,
                                   stats_.flushes,
                                   stats_.compactions,
                                   stats_.trivial_moves,
                                   stats_.tombstones_dropped,
                                   stats_.snapshots_across_compaction,
                                   stats_.iterators_across_compaction,
                                   stats_.injected_errors,
                                   stats_.injected_corruptions,
                                   stats_.reopens,
                                   stats_.write_failures,
                                   stats_.read_failures,
                                   stats_.open_failures,
                                   stats_.orphan_checks,
                                   stats_.deepest_level,
                                   stats_.damaged_opens,
                                   stats_.zero_tail_opens,
                                   stats_.manifest_zero_tail_opens,
                                   total_.shadowed_dropped,
                                   total_.user_bytes_written,
                                   total_.wal_bytes_written,
                                   total_.flush_bytes_written,
                                   total_.compaction_bytes_read,
                                   total_.compaction_bytes_written,
                                   total_.write_groups,
                                   total_.wal_syncs,
                                   total_.write_slowdowns,
                                   total_.write_stops,
                                   total_.bg_errors,
                                   env_->crashes(),
                                   ModelDB::Digest(model_.state())}) {
      Mix(counter);
    }

    SimResult result;
    result.ok = !failed_;
    result.stats = stats_;
    result.trace_digest = digest_;
    if (failed_) {
      result.failure = failure_ + "\nseed=" + std::to_string(options_.seed) +
                       " step=" + std::to_string(step_) + "\nlast " + std::to_string(kTraceLines) +
                       " operations:\n";
      for (const std::string& line : lines_) {
        result.failure += line + "\n";
      }
    }
    return result;
  }

  // ------------------------------------------------------------------------------------------

  const SimOptions options_;
  Random rng_;
  // Declared in the order they must go in and reversed on the way out: the database uses the Env
  // and the scheduler, and what the run holds of the database goes before it.
  std::unique_ptr<FaultInjectionEnv> env_;
  ManualScheduler scheduler_;
  Options db_options_;
  std::unique_ptr<DB> db_;  // what the run drives: the database, or the test's wrapper of it
  DBImpl* impl_ = nullptr;  // the database itself, for the test hooks
  std::vector<SnapshotRecord> snapshots_;
  std::vector<IteratorRecord> iterators_;

  // The model: what a read must see now; the state at the last durable point; and the writes
  // since, in order, of which the first `acked_` were acknowledged and one more may have failed.
  ModelDB model_;
  State durable_;
  std::vector<WriteBatch> uncertain_;
  size_t acked_ = 0;
  std::vector<std::string> recently_deleted_;
  uint64_t write_number_ = 0;

  // The sequence number the database had reached at the last check, and at the newest durable
  // point, which a reopened database must not be below.
  SequenceNumber prev_sequence_ = 0;
  SequenceNumber durable_sequence_ = 0;

  // What is armed, and what that does to what can be checked.
  bool faults_armed_ = false;  // a failure was armed since the faults were last cleared
  bool remove_dirty_ = false;  // a RemoveFile failure may have left a table for a later sweep
  int corruption_left_ = 0;
  uint64_t step_errors_ = 0;       // the Env's injected errors at the start of the step
  uint64_t step_corruptions_ = 0;  // and its corrupted reads
  bool unswept_ = false;
  uint64_t unswept_baseline_ = 0;

  // The database has stopped writing, and why; handled at the end of the step.
  bool error_pending_ = false;
  std::string error_text_;

  // The counters of the databases that were closed, and of the one that is open once it is.
  DBCounters total_;
  bool counters_pending_ = false;

  SimStats stats_;
  uint64_t digest_ = 0x73696d756c61746fULL;
  std::deque<std::string> lines_;
  int step_ = 0;
  bool failed_ = false;
  std::string failure_;
};

}  // namespace

SimResult RunSimulation(const SimOptions& options) {
  Simulator simulator(options);
  return simulator.Run();
}

}  // namespace shale
