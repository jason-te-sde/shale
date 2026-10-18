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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bench_engine.h"
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
};

/// What a write benchmark left in the database: which of the keys 0 ... num - 1 are there. The read
/// benchmarks are checked against it.
struct Populated {
  /// The benchmark that wrote the database.
  Benchmark written_by = Benchmark::kFillSeq;
  /// Whether each key, by index, is in the database.
  std::vector<bool> present;
  /// How many keys are in it.
  uint64_t entries = 0;
  /// The largest index that is in it, or -1 if none: a seek to a key above it finds nothing.
  int64_t last_present = -1;

  /// Records that the key `index` was written.
  void Mark(uint64_t index) {
    if (!present[index]) {
      present[index] = true;
      entries++;
      last_present = std::max(last_present, static_cast<int64_t>(index));
    }
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

/// One run of a benchmark.
struct RunResult {
  /// The operations that were timed. For readseq, the entries that the scan visited.
  uint64_t operations = 0;
  /// The key and value bytes of the entries that the operations wrote, returned or visited.
  uint64_t bytes = 0;
  /// The wall-clock time of the loop, which is never zero.
  double seconds = 0;
  /// The latency of each call of the engine, if the benchmark times each: not readseq.
  std::optional<Latency> latency;
  /// The engine's write amplification as it reported it, after a write benchmark.
  std::optional<std::string> write_amplification;
  /// What the operations came to: the gets that found a key, the seeks that landed on an entry, the
  /// entries a scan visited. Nothing after a write.
  std::optional<uint64_t> found;

  /// Returns the operations over the time.
  double OpsPerSecond() const { return static_cast<double>(operations) / seconds; }
  /// Returns the megabytes (of kBenchMegabyte bytes) over the time.
  double MbPerSecond() const { return static_cast<double>(bytes) / kBenchMegabyte / seconds; }
};

/// Runs one run of the write benchmark `benchmark`: opens a database at `options.db` that is empty,
/// puts the keys, and records what it left in `*populated` and the run in `*result`. overwrite
/// first loads every key in order, which is not timed, then compacts the database and opens it
/// again, so that the engine's write amplification counts the overwrites alone and the overwrites
/// start from a database with no work owed. Returns the first error of the engine.
/// @param engine the engine to run on; its database is left open.
/// @param options what to run.
/// @param benchmark one for which BenchmarkWrites() is true.
/// @param populated receives the record of the keys that were written.
/// @param result receives the measurements.
Status RunWriteBenchmark(BenchEngine* engine, const WorkloadOptions& options, Benchmark benchmark,
                         Populated* populated, RunResult* result);

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
