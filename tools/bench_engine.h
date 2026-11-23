#ifndef SHALE_TOOLS_BENCH_ENGINE_H_
#define SHALE_TOOLS_BENCH_ENGINE_H_

// The one interface that shale_bench's workloads are written against. A workload never names a
// database: it calls this, so that the same workload, with the same keys and values in the same
// order, can be run on another engine and the two numbers compare.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shale/options.h"
#include "shale/status.h"

namespace shale {

/// What an engine's database is at one moment, as the engine's public interface gives it: its
/// tables, level by level, what it has counted, and whether it has background work to do.
struct EngineState {
  /// Whether a background task, a flush or a compaction, is queued or running.
  bool background_busy = false;
  /// The highest compaction score of the levels that can be compacted, by the engine's own rule: a
  /// level whose score is 1 or more is due. The engine does not report it; the adapter works it out
  /// from the tables and the engine's limits.
  double highest_score = 0;
  /// The number of table files in each level, from level 0 down, every level of the engine.
  std::vector<uint64_t> files_per_level;
  /// The bytes of the table files in each level, as `files_per_level`.
  std::vector<uint64_t> bytes_per_level;
  /// The engine's own counters, by name, in the order that the engine gives them.
  std::vector<std::pair<std::string, uint64_t>> counters;
  /// The engine's write amplification, as WriteAmplification() reports it.
  std::string write_amplification = "n/a";

  /// Returns whether the engine has nothing to do in the background: no task queued or running,
  /// and no level due.
  bool Idle() const { return !background_busy && highest_score < 1.0; }
};

/// Returns the files of each level as text, "3/5/50/8/0/0/0", from level 0 down.
std::string FilesPerLevelText(const std::vector<uint64_t>& files_per_level);

/// A key-value engine as the benchmark workloads use it. Every method is called from one thread at
/// a time. An implementation reads what it is asked to read with the engine's defaults, and does no
/// work of its own that the engine would not do for any caller: an adapter that cached or batched
/// would make its engine look faster than it is.
class BenchEngine {
 public:
  /// Destroys the engine, closing the database it has open as Close() does. An engine is destroyed
  /// through this base class.
  virtual ~BenchEngine();

  /// Returns the engine's name as the benchmark output shows it, such as "shale".
  virtual std::string Name() const = 0;

  /// Opens the database at `path`, creating it if there is none. If `fresh`, whatever database is
  /// at `path` is destroyed first, so that the engine starts empty. A database that this engine has
  /// open is closed first.
  /// @param path the directory of the database.
  /// @param fresh whether to destroy an existing database at `path` before opening it.
  virtual Status Open(const std::string& path, bool fresh) = 0;

  /// Opens the database at `path` as it is, without making one: a path that holds no database is
  /// an error, and nothing is made there. A database that this engine has open is closed first.
  /// @param path the directory of the database.
  virtual Status OpenExisting(const std::string& path) = 0;

  /// Sets `key` to `value`.
  /// @param sync whether the write is on stable storage when the call returns, which is what the
  ///   engine means by a synced write: the same guarantee that its own default for a synced write
  ///   gives, not a weaker one.
  virtual Status Put(std::string_view key, std::string_view value, bool sync) = 0;

  /// Looks up `key`. A key that is not there is a NotFound status, which is an answer and not a
  /// failure; any other error is a failure of the engine.
  /// @param value receives the value when the status is OK.
  virtual Status Get(std::string_view key, std::string* value) = 0;

  /// Makes an iterator, positions it at the first key that is not less than `from` (at the first
  /// key of all when `from` is empty), and steps through up to `limit` entries, reading the key and
  /// the value of each. Making the iterator is part of the call: that is the cost of a seek.
  /// @param from where to start.
  /// @param limit the most entries to visit, the one the seek lands on included; 0 means to the end
  ///   of the database.
  /// @param seen receives the number of entries visited, which is less than `limit` when the
  ///   database ends first. It is set on success.
  virtual Status Scan(std::string_view from, size_t limit, size_t* seen) = 0;

  /// Compacts the whole key range and returns when that is done, so that what follows is not
  /// measured against work the writes left behind.
  virtual Status CompactAll() = 0;

  /// Returns the engine's own figure for the bytes it wrote to disk over the bytes that callers
  /// gave it, as the engine reports it, or "n/a" if it cannot report one, or if no database is
  /// open. It covers the work since the database was opened.
  virtual std::string WriteAmplification() = 0;

  /// Reads the state of the open database: its tables, its counters, and whether its background
  /// has work. The tables and the counters are read together, after whether the background is
  /// busy, so that a state that is idle shows the tables that the background left. Fails if no
  /// database is open.
  /// @param state receives the state.
  virtual Status GetState(EngineState* state) = 0;

  /// Closes the database. Does nothing if none is open. What closing does with the engine's
  /// in-memory data is the engine's own: shale does not flush it, and the next open replays its
  /// log.
  virtual void Close() = 0;
};

/// Returns an engine that drives shale. `base` gives the options of every database it opens, with
/// create_if_missing set; `base.env` and `base.scheduler`, if set, must outlive the engine.
///
/// Its background work runs where shale's would: on `base.scheduler` if it is set, and otherwise on
/// a scheduler with a thread of its own, made for each database that the engine opens, as shale
/// makes one for a database opened without one. The engine hands each task on and counts the tasks
/// that are queued or running, which is how GetState() knows whether the background is busy.
/// @param base the options to open databases with.
std::unique_ptr<BenchEngine> NewShaleBenchEngine(const Options& base);

/// Returns the highest compaction score among shale's levels by the rule of its picker, which
/// compacts a level whose score is 1 or more: level 0 scores its files over the smaller of
/// l0_compaction_trigger and l0_stop_trigger, each level below it its bytes over
/// max_bytes_for_level_base times level_size_multiplier to the power level - 1, and the last level,
/// which has nowhere to compact into, is not counted.
/// @param files_per_level the files of each level, from level 0.
/// @param bytes_per_level the bytes of each level, from level 0.
/// @param options the options that the database was opened with.
double ShaleHighestScore(const std::vector<uint64_t>& files_per_level,
                         const std::vector<uint64_t>& bytes_per_level, const Options& options);

}  // namespace shale

#endif  // SHALE_TOOLS_BENCH_ENGINE_H_
