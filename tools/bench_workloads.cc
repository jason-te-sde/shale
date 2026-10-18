#include "bench_workloads.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "bench_engine.h"
#include "histogram.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/random.h"

namespace shale {

// The switches below have no default, so that a benchmark that is added does not compile until each
// of them says what it is.
const char* BenchmarkName(Benchmark benchmark) {
  switch (benchmark) {
    case Benchmark::kFillSeq:
      return "fillseq";
    case Benchmark::kFillRandom:
      return "fillrandom";
    case Benchmark::kFillSync:
      return "fillsync";
    case Benchmark::kOverwrite:
      return "overwrite";
    case Benchmark::kReadRandom:
      return "readrandom";
    case Benchmark::kReadMissing:
      return "readmissing";
    case Benchmark::kReadSeq:
      return "readseq";
    case Benchmark::kSeekRandom:
      return "seekrandom";
  }
  SHALE_CHECK(false, "a Benchmark that BenchmarkName() does not know");
}

bool BenchmarkWrites(Benchmark benchmark) {
  switch (benchmark) {
    case Benchmark::kFillSeq:
    case Benchmark::kFillRandom:
    case Benchmark::kFillSync:
    case Benchmark::kOverwrite:
      return true;
    case Benchmark::kReadRandom:
    case Benchmark::kReadMissing:
    case Benchmark::kReadSeq:
    case Benchmark::kSeekRandom:
      return false;
  }
  SHALE_CHECK(false, "a Benchmark that BenchmarkWrites() does not know");
}

std::optional<Benchmark> FindBenchmark(std::string_view name) {
  for (const Benchmark benchmark : kAllBenchmarks) {
    if (name == BenchmarkName(benchmark)) {
      return benchmark;
    }
  }
  return std::nullopt;
}

Latency SummarizeLatencies(const Histogram& histogram) {
  Latency latency;
  latency.p50 = histogram.Percentile(50);
  latency.p99 = histogram.Percentile(99);
  latency.p999 = histogram.Percentile(99.9);
  latency.average = histogram.Average();
  return latency;
}

namespace {

constexpr size_t kKeySize = kBenchKeySize;

// A key as the benchmarks write it, in a buffer that is reused: the 16 digits of the index, zero
// padded, which is what "%016llu" makes, or those digits and a dot, which sorts after the key and
// before the next one and is never written.
class KeyBuffer {
 public:
  std::string_view Present(uint64_t index) {
    Digits(index);
    return std::string_view(chars_.data(), kKeySize);
  }

  std::string_view Missing(uint64_t index) {
    Digits(index);
    chars_[kKeySize] = '.';
    return std::string_view(chars_.data(), kKeySize + 1);
  }

 private:
  void Digits(uint64_t index) {
    // An index below 2^32 has ten digits at most, so sixteen hold it.
    for (size_t place = kKeySize; place > 0; place--) {
      chars_[place - 1] = static_cast<char>('0' + index % 10);
      index /= 10;
    }
  }

  std::array<char, kKeySize + 1> chars_{};
};

// Makes `size` bytes of the generator's output, the same bytes on every platform.
void FillValue(Random* random, size_t size, std::string* value) {
  value->resize(size);
  char* const out = value->data();
  for (size_t at = 0; at < size; at += 8) {
    std::array<char, 8> word{};
    EncodeFixed64(word.data(), random->Next());
    std::memcpy(out + at, word.data(), std::min<size_t>(8, size - at));
  }
}

// Puts the number of a put in the first bytes of its value: the whole fixed64, or its low bytes if
// the value is shorter than eight.
void StampValue(uint64_t put_number, std::string* value) {
  std::array<char, 8> word{};
  EncodeFixed64(word.data(), put_number);
  std::memcpy(value->data(), word.data(), std::min<size_t>(8, value->size()));
}

// Whether `value` begins with the number of the put, as StampValue() wrote it.
bool HasStamp(std::string_view value, uint64_t put_number) {
  std::array<char, 8> word{};
  EncodeFixed64(word.data(), put_number);
  const size_t stamped = std::min<size_t>(8, value.size());
  return std::memcmp(value.data(), word.data(), stamped) == 0;
}

// The streams of random numbers of a benchmark. Each is a function of the seed and the benchmark,
// and of nothing else: not of the run, so that every run makes the same operations, and not of
// which other benchmarks are in the list, so that one benchmark repeats on its own.
enum class Stream : uint8_t { kKeys = 1, kValues = 2, kLoadValues = 3 };

uint64_t StreamSeed(const WorkloadOptions& options, Benchmark benchmark, Stream stream) {
  const uint64_t benchmark_part = (static_cast<uint64_t>(benchmark) + 1) * 0xd6e8feb86659fd93ULL;
  const uint64_t stream_part = static_cast<uint64_t>(stream) * 0x2545f4914f6cdd1dULL;
  return options.seed ^ benchmark_part ^ stream_part;
}

using Clock = std::chrono::steady_clock;

double MicrosBetween(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double, std::micro>(to - from).count();
}

// Fills in what every run has from the clock readings around it and the operations it made.
void Finish(Clock::time_point begin, Clock::time_point end, uint64_t operations, uint64_t bytes,
            double engine_micros, RunResult* result) {
  result->operations = operations;
  result->bytes = bytes;
  result->engine_seconds = engine_micros * 1e-6;
  // A clock that did not move would make every rate infinite, which no figure can say.
  result->seconds = std::max(std::chrono::duration<double>(end - begin).count(), 1e-9);
}

Status Wrong(Benchmark benchmark, std::string_view what) {
  return Status::Corruption(BenchmarkName(benchmark), what);
}

// Puts every key in order, unsynced, which is how overwrite gets keys to overwrite. It is not
// timed. `*puts` counts the puts of the run, and is the number that the next put is given.
Status LoadAllKeys(BenchEngine* engine, const WorkloadOptions& options, Populated* populated,
                   uint64_t* puts) {
  Random values(StreamSeed(options, Benchmark::kOverwrite, Stream::kLoadValues));
  KeyBuffer key;
  std::string value;
  for (uint64_t index = 0; index < options.num; index++) {
    FillValue(&values, options.value_size, &value);
    StampValue(++*puts, &value);
    if (const Status s = engine->Put(key.Present(index), value, /*sync=*/false); !s.ok()) {
      return s;
    }
    populated->Mark(index, *puts);
  }
  return Status::OK();
}

Status RunReadRandom(BenchEngine* engine, const WorkloadOptions& options,
                     const Populated& populated, RunResult* result) {
  Random keys(StreamSeed(options, Benchmark::kReadRandom, Stream::kKeys));
  KeyBuffer key;
  std::string value;
  Histogram histogram;
  uint64_t found = 0;
  uint64_t bytes = 0;
  double engine_micros = 0;

  const Clock::time_point begin = Clock::now();
  for (uint64_t i = 0; i < options.num; i++) {
    const uint64_t index = keys.Uniform(static_cast<uint32_t>(options.num));
    const std::string_view key_bytes = key.Present(index);
    const Clock::time_point before = Clock::now();
    Status s = engine->Get(key_bytes, &value);
    const Clock::time_point after = Clock::now();
    const double micros = MicrosBetween(before, after);
    histogram.Add(micros);
    engine_micros += micros;
    if (s.ok()) {
      if (!populated.Has(index)) {
        return Wrong(Benchmark::kReadRandom,
                     "key " + std::string(key_bytes) + " was found and was never written");
      }
      if (value.size() != options.value_size) {
        return Wrong(Benchmark::kReadRandom, "key " + std::string(key_bytes) + " has a value of " +
                                                 std::to_string(value.size()) + " bytes, not " +
                                                 std::to_string(options.value_size));
      }
      if (!HasStamp(value, populated.stamp[index])) {
        return Wrong(
            Benchmark::kReadRandom,
            "key " + std::string(key_bytes) + " has a value that is not the value last put to it");
      }
      found++;
      bytes += kKeySize + value.size();
    } else if (s.IsNotFound()) {
      if (populated.Has(index)) {
        return Wrong(Benchmark::kReadRandom,
                     "key " + std::string(key_bytes) + " was written and is not found");
      }
    } else {
      return s;
    }
  }
  const Clock::time_point end = Clock::now();

  Finish(begin, end, options.num, bytes, engine_micros, result);
  result->latency = SummarizeLatencies(histogram);
  result->found = found;
  return Status::OK();
}

Status RunReadMissing(BenchEngine* engine, const WorkloadOptions& options, RunResult* result) {
  Random keys(StreamSeed(options, Benchmark::kReadMissing, Stream::kKeys));
  KeyBuffer key;
  std::string value;
  Histogram histogram;
  double engine_micros = 0;

  const Clock::time_point begin = Clock::now();
  for (uint64_t i = 0; i < options.num; i++) {
    const std::string_view key_bytes =
        key.Missing(keys.Uniform(static_cast<uint32_t>(options.num)));
    const Clock::time_point before = Clock::now();
    Status s = engine->Get(key_bytes, &value);
    const Clock::time_point after = Clock::now();
    const double micros = MicrosBetween(before, after);
    histogram.Add(micros);
    engine_micros += micros;
    if (s.ok()) {
      return Wrong(Benchmark::kReadMissing,
                   "key " + std::string(key_bytes) + " was found and was never written");
    }
    if (!s.IsNotFound()) {
      return s;
    }
  }
  const Clock::time_point end = Clock::now();

  Finish(begin, end, options.num, /*bytes=*/0, engine_micros, result);
  result->latency = SummarizeLatencies(histogram);
  result->found = 0;
  return Status::OK();
}

Status RunReadSeq(BenchEngine* engine, const WorkloadOptions& options, const Populated& populated,
                  RunResult* result) {
  size_t seen = 0;
  const Clock::time_point begin = Clock::now();
  Status s = engine->Scan("", 0, &seen);
  const Clock::time_point end = Clock::now();
  if (!s.ok()) {
    return s;
  }
  if (seen != populated.entries) {
    return Wrong(Benchmark::kReadSeq, "the scan visited " + std::to_string(seen) +
                                          " entries, and the database holds " +
                                          std::to_string(populated.entries));
  }
  Finish(begin, end, seen, seen * (kKeySize + options.value_size), /*engine_micros=*/0, result);
  // A scan is one call: all of the loop is the engine's.
  result->engine_seconds = result->seconds;
  result->found = seen;
  return Status::OK();
}

Status RunSeekRandom(BenchEngine* engine, const WorkloadOptions& options,
                     const Populated& populated, RunResult* result) {
  Random keys(StreamSeed(options, Benchmark::kSeekRandom, Stream::kKeys));
  KeyBuffer key;
  Histogram histogram;
  uint64_t landed = 0;
  double engine_micros = 0;

  const Clock::time_point begin = Clock::now();
  for (uint64_t i = 0; i < options.num; i++) {
    const uint64_t index = keys.Uniform(static_cast<uint32_t>(options.num));
    const std::string_view key_bytes = key.Present(index);
    size_t seen = 0;
    const Clock::time_point before = Clock::now();
    Status s = engine->Scan(key_bytes, 1, &seen);
    const Clock::time_point after = Clock::now();
    if (!s.ok()) {
      return s;
    }
    const double micros = MicrosBetween(before, after);
    histogram.Add(micros);
    engine_micros += micros;
    // A seek lands on the first key at or after its target, so it finds an entry unless every key
    // that is there is below it.
    const size_t want = static_cast<int64_t>(index) <= populated.last_present ? 1 : 0;
    if (seen != want) {
      return Wrong(Benchmark::kSeekRandom, "a seek to " + std::string(key_bytes) + " visited " +
                                               std::to_string(seen) + " entries, not " +
                                               std::to_string(want));
    }
    landed += seen;
  }
  const Clock::time_point end = Clock::now();

  Finish(begin, end, options.num, landed * (kKeySize + options.value_size), engine_micros, result);
  result->latency = SummarizeLatencies(histogram);
  result->found = landed;
  return Status::OK();
}

}  // namespace

Status RunWriteBenchmark(BenchEngine* engine, const WorkloadOptions& options, Benchmark benchmark,
                         Populated* populated, RunResult* result) {
  if (const Status s = engine->Open(options.db, /*fresh=*/true); !s.ok()) {
    return s;
  }
  *populated = Populated();
  populated->written_by = benchmark;
  populated->stamp.assign(options.num, 0);
  uint64_t puts = 0;
  if (benchmark == Benchmark::kOverwrite) {
    if (const Status s = LoadAllKeys(engine, options, populated, &puts); !s.ok()) {
      return s;
    }
    // The engine counts its write amplification from the open. Opened again, it counts the
    // overwrites and not the load that they overwrite, as long as the load has nothing left to do:
    // what is still in the memtable would be written out by the open, and the compaction that the
    // load is owed would run during the overwrites, and both would be counted against them.
    if (const Status s = engine->CompactAll(); !s.ok()) {
      return s;
    }
    if (const Status s = engine->Open(options.db, /*fresh=*/false); !s.ok()) {
      return s;
    }
  }

  const bool sync = benchmark == Benchmark::kFillSync;
  const uint64_t operations = sync ? options.sync_num : options.num;
  Random keys(StreamSeed(options, benchmark, Stream::kKeys));
  Random values(StreamSeed(options, benchmark, Stream::kValues));
  KeyBuffer key;
  std::string value;
  Histogram histogram;
  uint64_t bytes = 0;
  double engine_micros = 0;

  const Clock::time_point begin = Clock::now();
  for (uint64_t i = 0; i < operations; i++) {
    const uint64_t index =
        benchmark == Benchmark::kFillSeq ? i : keys.Uniform(static_cast<uint32_t>(options.num));
    const std::string_view key_bytes = key.Present(index);
    FillValue(&values, options.value_size, &value);
    StampValue(++puts, &value);
    const Clock::time_point before = Clock::now();
    Status s = engine->Put(key_bytes, value, sync);
    const Clock::time_point after = Clock::now();
    if (!s.ok()) {
      return s;
    }
    const double micros = MicrosBetween(before, after);
    histogram.Add(micros);
    engine_micros += micros;
    populated->Mark(index, puts);
    bytes += kKeySize + value.size();
  }
  const Clock::time_point end = Clock::now();

  Finish(begin, end, operations, bytes, engine_micros, result);
  result->latency = SummarizeLatencies(histogram);
  result->write_amplification = engine->WriteAmplification();
  return Status::OK();
}

Status RunReadBenchmark(BenchEngine* engine, const WorkloadOptions& options, Benchmark benchmark,
                        const Populated& populated, RunResult* result) {
  switch (benchmark) {
    case Benchmark::kReadRandom:
      return RunReadRandom(engine, options, populated, result);
    case Benchmark::kReadMissing:
      return RunReadMissing(engine, options, result);
    case Benchmark::kReadSeq:
      return RunReadSeq(engine, options, populated, result);
    case Benchmark::kSeekRandom:
      return RunSeekRandom(engine, options, populated, result);
    case Benchmark::kFillSeq:
    case Benchmark::kFillRandom:
    case Benchmark::kFillSync:
    case Benchmark::kOverwrite:
      break;
  }
  SHALE_CHECK(false, "RunReadBenchmark of a benchmark that writes");
}

}  // namespace shale
