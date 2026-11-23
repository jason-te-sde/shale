#ifndef SHALE_TOOLS_BENCH_WORKLOADS_H_
#define SHALE_TOOLS_BENCH_WORKLOADS_H_

// The workloads of shale_bench, and what they record about a run. They are in a library of their
// own, and take the engine as a BenchEngine, so that the tests can run them against an engine that
// lies and see each wrong answer caught.
//
// Keys are the 16 digits of an index, "%016llu"; a key that a read benchmark asks for and that was
// never written is those digits and a dot. Values are `value_size` bytes of seeded pseudo-random
// data. Every stream of random numbers is a function of the seed and the benchmark alone, so a
// benchmark makes the same operations in every run, whatever else runs with it.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bench_engine.h"
#include "histogram.h"
#include "shale/status.h"

namespace shale {

/// The workloads, in the order that shale_bench runs them by default: the ones that write, and then
/// the ones that read what the last of those left.
enum class Benchmark {
  kFillSeq,
  kFillRandom,
  kFillSync,
  kOverwrite,
  kReadRandom,
  kReadMissing,
  kReadSeq,
  kSeekRandom,
};

/// Every benchmark, in the order of the enumeration.
inline constexpr std::array<Benchmark, 8> kAllBenchmarks = {
    Benchmark::kFillSeq,    Benchmark::kFillRandom,  Benchmark::kFillSync, Benchmark::kOverwrite,
    Benchmark::kReadRandom, Benchmark::kReadMissing, Benchmark::kReadSeq,  Benchmark::kSeekRandom,
};

/// The size of a key in bytes.
inline constexpr size_t kBenchKeySize = 16;

/// The number of bytes in a megabyte in MB/s, which is 2^20 as in db_bench.
inline constexpr double kBenchMegabyte = 1048576.0;

/// Returns the name that the command line and the output use for `benchmark`.
const char* BenchmarkName(Benchmark benchmark);

/// Returns whether `benchmark` writes to the database, and so starts from an empty one, as opposed
/// to reading what a write left.
bool BenchmarkWrites(Benchmark benchmark);

/// Returns the benchmark that has `name`, or nothing if there is none.
std::optional<Benchmark> FindBenchmark(std::string_view name);

/// How long Settle() waits for an engine's background work, and how often it looks.
struct SettleOptions {
  /// The time between two looks.
  std::chrono::milliseconds poll{10};
  /// The longest it waits, from its first look.
  std::chrono::milliseconds cap{300000};
};

/// What the workloads need to know, apart from the engine.
struct WorkloadOptions {
  /// The directory of the database, which every write benchmark destroys and makes again.
  std::string db;
  /// The operations of a benchmark, and the number of keys there can be: from 1 to 2^32 - 1.
  uint64_t num = 0;
  /// The operations of fillsync, which syncs each one.
  uint64_t sync_num = 0;
  /// The size of every value in bytes.
  uint64_t value_size = 0;
  /// Seeds every stream of random numbers.
  uint64_t seed = 0;
  /// How a write run waits for the engine's background once its last write has returned, and
  /// overwrite once its load has been compacted.
  SettleOptions settle;
};

/// What a write benchmark left in the database: which of the keys 0 ... num - 1 are there, and
/// which put wrote the value each one has. The read benchmarks are checked against it.
///
/// The puts of a run are numbered from 1 in the order they are made, the load of overwrite before
/// its timed puts, and every value begins with the number of its put as a fixed64 (its low bytes,
/// if the value is shorter than eight). So a value of the right size that is another key's, or an
/// older version of its own key, is told from the right one by its first bytes.
struct Populated {
  /// The benchmark that wrote the database.
  Benchmark written_by = Benchmark::kFillSeq;
  /// For each key, by index, the number of the put that wrote the value it has now, or 0 if the
  /// key is not in the database.
  std::vector<uint64_t> stamp;
  /// How many keys are in it.
  uint64_t entries = 0;
  /// The largest index that is in it, or -1 if none: a seek to a key above it finds nothing.
  int64_t last_present = -1;

  /// Returns whether the key `index` is in the database.
  bool Has(uint64_t index) const { return stamp[index] != 0; }

  /// Records that put number `put_number`, which is not 0, wrote the key `index`.
  void Mark(uint64_t index, uint64_t put_number) {
    if (stamp[index] == 0) {
      entries++;
      last_present = std::max(last_present, static_cast<int64_t>(index));
    }
    stamp[index] = put_number;
  }
};

/// The latencies of the engine's calls in a run, in microseconds.
struct Latency {
  /// The median, by nearest rank.
  double p50 = 0;
  /// The 99th percentile, by nearest rank.
  double p99 = 0;
  /// The 99.9th percentile, by nearest rank.
  double p999 = 0;
  /// The mean.
  double average = 0;
};

/// Returns the latencies of `histogram`: the nearest-rank 50th, 99th and 99.9th percentiles and the
/// mean. Aborts if it has no samples.
/// @param histogram the latencies of the engine's calls in a run, in microseconds.
Latency SummarizeLatencies(const Histogram& histogram);

/// One run of a benchmark.
struct RunResult {
  /// The operations that were timed. For readseq, the entries that the scan visited.
  uint64_t operations = 0;
  /// The key and value bytes of the entries that the operations wrote, returned or visited.
  uint64_t bytes = 0;
  /// The wall-clock time of the loop, which is never zero. It holds making each key and value and
  /// checking each answer as well as the engine's calls.
  double seconds = 0;
  /// The time inside the engine's calls: the sum of their latencies, and for readseq, which is one
  /// call, all of `seconds`. It is never more than `seconds`.
  double engine_seconds = 0;
  /// The latency of each call of the engine, if the benchmark times each: not readseq.
  std::optional<Latency> latency;
  /// The engine's write amplification as it reported it, after a write benchmark.
  std::optional<std::string> write_amplification;
  /// What the operations came to: the gets that found a key, the seeks that landed on an entry, the
  /// entries a scan visited. Nothing after a write.
  std::optional<uint64_t> found;
  /// After a write: the time from the return of the last write to the look that found the engine's
  /// background idle (Settle()).
  std::optional<double> settle_seconds;
  /// After a write: the engine's state when the last write returned, and once it had settled.
  std::optional<EngineState> returned;
  std::optional<EngineState> settled;

  /// Returns the operations over the time.
  double OpsPerSecond() const { return static_cast<double>(operations) / seconds; }
  /// Returns the operations over the time and the wait for the background, after a write.
  double SettledOpsPerSecond() const {
    return static_cast<double>(operations) / (seconds + settle_seconds.value_or(0));
  }
  /// Returns the megabytes (of kBenchMegabyte bytes) over the time.
  double MbPerSecond() const { return static_cast<double>(bytes) / kBenchMegabyte / seconds; }
};

/// What Settle() saw.
struct SettleResult {
  /// The time from its first look to its last.
  double seconds = 0;
  /// How many times it looked.
  uint64_t looks = 0;
  /// The state of its last look: idle, unless it gave up.
  EngineState state;
};

/// Waits until `engine` is idle, as EngineState::Idle() says: no background task queued or
/// running, and no level due for compaction. It looks at once, and then every `options.poll`, and
/// returns at the first look that finds the engine idle. `result` is filled in whatever it returns.
/// @param engine the engine, with a database open.
/// @param options how often to look, and for how long.
/// @param result receives the time it waited, the looks, and the last state it saw.
/// @return OK once the engine is idle; an IOError that says what it saw last, if the engine was
///   not idle `options.cap` after the first look; and the engine's own error if a look fails.
Status Settle(BenchEngine* engine, const SettleOptions& options, SettleResult* result);

/// Runs one run of the write benchmark `benchmark`: opens a database at `options.db` that is empty,
/// puts the keys, and records what it left in `*populated` and the run in `*result`. overwrite
/// first loads every key in order, which is not timed, then compacts the database, opens it again
/// and waits for its background (Settle()), so that the engine's write amplification counts the
/// overwrites alone and the overwrites start from a database with no work owed. Once the last
/// write has returned, the run reads the engine's state, waits for its background with
/// `options.settle`, and reads it again: the run's time stops when the last write returns, as
/// db_bench's does, and the wait is given beside it. Returns the first error of the engine, or
/// Settle()'s error if the background did not finish.
/// @param engine the engine to run on; its database is left open.
/// @param options what to run.
/// @param benchmark one for which BenchmarkWrites() is true.
/// @param populated receives the record of the keys that were written.
/// @param result receives the measurements.
Status RunWriteBenchmark(BenchEngine* engine, const WorkloadOptions& options, Benchmark benchmark,
                         Populated* populated, RunResult* result);

/// Returns the record of what the write benchmark `benchmark` leaves in a database, worked out from
/// `options` without an engine: the record that RunWriteBenchmark() keeps when it runs it with the
/// same options. It is what the reads of a database that is already there are checked against.
/// @param options the options that the database was written with.
/// @param benchmark one for which BenchmarkWrites() is true.
Populated RecordOfWrite(const WorkloadOptions& options, Benchmark benchmark);

/// Runs one run of the read benchmark `benchmark` on the database that `populated` describes, which
/// the engine has open, and checks every answer against it. Returns Corruption, naming the
/// benchmark and the key, for a key that was written and is not found, one that was not written
/// and is, a value of the wrong size, a scan that visits other than the entries there are, and a
/// seek that lands where it should not; and the engine's own error for a call that fails.
/// @param engine the engine to run on.
/// @param options what to run; the same as for the write that made `populated`.
/// @param benchmark one for which BenchmarkWrites() is false.
/// @param populated the record of what is in the database.
/// @param result receives the measurements.
Status RunReadBenchmark(BenchEngine* engine, const WorkloadOptions& options, Benchmark benchmark,
                        const Populated& populated, RunResult* result);

}  // namespace shale

#endif  // SHALE_TOOLS_BENCH_WORKLOADS_H_
