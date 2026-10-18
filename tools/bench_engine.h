#ifndef SHALE_TOOLS_BENCH_ENGINE_H_
#define SHALE_TOOLS_BENCH_ENGINE_H_

// The one interface that shale_bench's workloads are written against. A workload never names a
// database: it calls this, so that the same workload, with the same keys and values in the same
// order, can be run on another engine and the two numbers compare.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "shale/options.h"
#include "shale/status.h"

namespace shale {

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

  /// Closes the database. Does nothing if none is open. What closing does with the engine's
  /// in-memory data is the engine's own: shale does not flush it, and the next open replays its
  /// log.
  virtual void Close() = 0;
};

/// Returns an engine that drives shale. `base` gives the options of every database it opens, with
/// create_if_missing set; `base.env` and `base.scheduler`, if set, must outlive the engine.
/// @param base the options to open databases with.
std::unique_ptr<BenchEngine> NewShaleBenchEngine(const Options& base);

}  // namespace shale

#endif  // SHALE_TOOLS_BENCH_ENGINE_H_
