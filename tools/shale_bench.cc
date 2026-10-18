// shale_bench: db_bench-style workloads against shale, with numbers that can be quoted.
//
// A benchmark here is a loop of operations on a database, timed. Keys are the 16 digits of an
// index, "%016llu", and values are --value_size bytes from a seeded generator, so a run is a
// function of its flags: the same flags make the same operations in the same order, on any machine,
// and what differs between two runs of them is the machine. The workloads call the engine through
// BenchEngine alone, so that the same workloads can be run on another engine.
//
// What is measured, since a number is only as good as its definition:
//
// - Each benchmark is run --repeat times, and the table shows the median of each column over the
//   runs. A run of a write benchmark starts from an empty database. Every run of a benchmark makes
//   the same operations, so the runs differ only in how long they took, and that is what the
//   spread column shows. The later runs of a read benchmark find the earlier runs' blocks in the
//   cache and in the operating system's, which the median over the runs includes.
// - ops/s is the operations of a run over its wall-clock time, as db_bench does: it includes
//   making each key and value, the two clock reads around each call, and checking each answer,
//   which is a fraction of a microsecond an operation, the same for every engine.
// - The latency columns are the nearest-rank percentiles of the time of the engine call alone, from
//   a clock read before it to one after, over every operation of a run. readseq has none: a scan
//   is one call, and the engine's interface does not say when each entry arrived.
// - MB/s is the key and value bytes of the entries an operation wrote, returned or visited, over
//   the same wall-clock time, with a megabyte of 2^20 bytes as in db_bench. A lookup of a key that
//   is not there moves no entry, so readmissing is 0.
// - A write benchmark runs on a database it has just made, so its write amplification is the
//   engine's own figure for that run: log, flushes and compactions, since the database was opened,
//   over the bytes the callers wrote. The reads show none.
//
// What is checked, since a benchmark of an engine that gives wrong answers measures nothing: the
// write benchmarks record which keys they wrote, and every read is compared with the record. A key
// that was written and is not found, a key that was never written and is, a value of the wrong
// size, a scan that visits another number of entries than the database holds, and a seek that
// lands where it should not, all end the run with status 1 and name the key.
//
// Run it with --help for the usage.

#include <sys/utsname.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "bench_engine.h"
#include "bench_stats.h"
#include "bench_workloads.h"
#include "cli_args.h"
#include "shale/options.h"
#include "shale/status.h"
#include "util/check.h"

// What this binary was built with, which CMake passes in. A build that does not know says so.
#ifndef SHALE_BENCH_COMPILER_ID
#define SHALE_BENCH_COMPILER_ID "unknown"
#endif
#ifndef SHALE_BENCH_COMPILER_VERSION
#define SHALE_BENCH_COMPILER_VERSION "unknown"
#endif
#ifndef SHALE_BENCH_BUILD_TYPE
#define SHALE_BENCH_BUILD_TYPE "unknown"
#endif
#ifndef SHALE_BENCH_SANITIZER
#define SHALE_BENCH_SANITIZER ""
#endif

namespace shale {
namespace {

// The exit statuses.
constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;  // a benchmark failed, or its answers were wrong, or I/O failed
constexpr int kExitUsage = 2;   // the command line is wrong

constexpr char kUsage[] =
    "usage: shale_bench --db=PATH [--benchmarks=LIST] [--num=1000000] [--sync_num=10000]\n"
    "                   [--value_size=100] [--repeat=3] [--seed=301] [--json=FILE]\n"
    "                   [--compact_before_reads]\n"
    "\n"
    "Runs db_bench-style workloads against shale and prints one line for each benchmark:\n"
    "the median over --repeat runs of operations a second, megabytes a second (of 2^20 bytes,\n"
    "counting the key and value bytes of the entries moved), the p50, p99 and p99.9 latency of\n"
    "the engine call in microseconds (nearest rank, over every operation), and the engine's\n"
    "write amplification. Lines that start with # say what the run was and where; the table\n"
    "has no other kind.\n"
    "\n"
    "PATH is destroyed and made again by every write benchmark: give it a directory that holds\n"
    "nothing you want. Keys are 16 digits, \"%016llu\"; values are --value_size bytes of seeded\n"
    "pseudo-random data. The same flags make the same operations in the same order.\n"
    "\n"
    "--benchmarks is a comma-separated list, run in the order given; the default is all of them:\n"
    "  fillseq      --num puts of the keys 0, 1, 2, ... in order, into an empty database\n"
    "  fillrandom   --num puts of random keys out of the --num there are, so that some repeat,\n"
    "               into an empty database\n"
    "  fillsync     --sync_num puts of random keys, each one synced, into an empty database\n"
    "  overwrite    all --num keys are put in order into an empty database, which is compacted\n"
    "               and opened again (none of that is timed), then --num puts of random keys,\n"
    "               all of them there already, are timed\n"
    "  readrandom   --num gets of random keys out of the --num there are\n"
    "  readmissing  --num gets of keys that were never written (a key's 16 digits and a dot)\n"
    "  readseq      one scan of the whole database, from the first key to the last\n"
    "  seekrandom   --num seeks to random keys, each reading the entry it lands on\n"
    "\n"
    "A read benchmark reads the database that the most recent write benchmark before it in the\n"
    "list left, and its line says which. fillsync leaves at most --sync_num keys, so a read\n"
    "benchmark that follows it measures a database that small: put it before the others.\n"
    "--compact_before_reads compacts the database once between a write benchmark and the first\n"
    "read benchmark after it; that is not timed, and is in no line. The reads are checked\n"
    "against a record of what was written, and a wrong answer ends the run with status 1.\n"
    "\n"
    "--json=FILE writes the same figures, every run's own numbers, the machine (uname, CPU\n"
    "brand, logical cores), and the compiler and build type, to FILE. A build that is not an\n"
    "optimized one with no sanitizer says so in the output: its numbers are not worth quoting.\n"
    "FILE is made when the run starts, so that a path that cannot be written is known at once;\n"
    "a run that fails leaves it empty.\n"
    "\n"
    "exit status: 0 every benchmark ran, 1 a benchmark failed or answered wrongly, 2 usage error\n";

// The most operations --num may ask for: Random::Uniform takes a 32-bit bound.
constexpr uint64_t kMaxNum = 4294967295;
constexpr uint64_t kMaxBenchValueSize = uint64_t{1} << 20;
constexpr uint64_t kMaxRepeat = 1000;

// ---- The command line's view of the benchmarks. ------------------------------------------------

std::string BenchmarkNames() {
  std::string names;
  for (const Benchmark benchmark : kAllBenchmarks) {
    names += names.empty() ? "" : ", ";
    names += BenchmarkName(benchmark);
  }
  return names;
}

struct Config {
  std::vector<Benchmark> benchmarks;
  WorkloadOptions workload;
  uint64_t repeat = 3;
  std::string json;
  bool compact_before_reads = false;

  Config() {
    workload.num = 1000000;
    workload.sync_num = 10000;
    workload.value_size = 100;
    workload.seed = 301;
  }
};

using Clock = std::chrono::steady_clock;

// ---- The results of a benchmark. ----------------------------------------------------------------

// A benchmark's runs and the figures taken over them.
struct Summary {
  Benchmark benchmark = Benchmark::kFillSeq;
  // The database the benchmark ran on: what wrote it, and how many keys it held.
  Benchmark source = Benchmark::kFillSeq;
  uint64_t entries = 0;
  // How long the compaction before this benchmark took, if there was one.
  std::optional<double> compact_seconds;
  std::vector<RunResult> runs;

  double ops_per_sec = 0;
  double mb_per_sec = 0;
  std::optional<Latency> latency;
  std::optional<std::string> write_amplification;
  // How far apart the fastest and the slowest run were, as a share of the median.
  double spread_percent = 0;
};

template <typename Get>
double MedianOf(const std::vector<RunResult>& runs, Get get) {
  std::vector<double> values;
  values.reserve(runs.size());
  for (const RunResult& run : runs) {
    values.push_back(get(run));
  }
  return Median(std::move(values));
}

std::string Fixed(double value, int digits) {
  std::array<char, 64> text{};
  std::snprintf(text.data(), text.size(), "%.*f", digits, value);
  return text.data();
}

void TakeMedians(Summary* summary) {
  const std::vector<RunResult>& runs = summary->runs;
  summary->ops_per_sec = MedianOf(runs, [](const RunResult& r) { return r.OpsPerSecond(); });
  summary->mb_per_sec = MedianOf(runs, [](const RunResult& r) { return r.MbPerSecond(); });

  if (runs.front().latency.has_value()) {
    Latency latency;
    latency.p50 = MedianOf(runs, [](const RunResult& r) { return r.latency->p50; });
    latency.p99 = MedianOf(runs, [](const RunResult& r) { return r.latency->p99; });
    latency.p999 = MedianOf(runs, [](const RunResult& r) { return r.latency->p999; });
    latency.average = MedianOf(runs, [](const RunResult& r) { return r.latency->average; });
    summary->latency = latency;
  }

  if (runs.front().write_amplification.has_value()) {
    std::vector<std::string> figures;
    figures.reserve(runs.size());
    for (const RunResult& run : runs) {
      if (run.write_amplification.has_value()) {
        figures.push_back(*run.write_amplification);
      }
    }
    summary->write_amplification = MedianFigure(figures);
  }

  double fastest = 0;
  double slowest = 0;
  for (size_t i = 0; i < runs.size(); i++) {
    const double rate = runs[i].OpsPerSecond();
    fastest = i == 0 ? rate : std::max(fastest, rate);
    slowest = i == 0 ? rate : std::min(slowest, rate);
  }
  summary->spread_percent = (fastest - slowest) / summary->ops_per_sec * 100;
}

// ---- The output. --------------------------------------------------------------------------------

std::string Notes(const Summary& summary, const Config& config) {
  std::string notes;
  if (BenchmarkWrites(summary.benchmark)) {
    notes = std::to_string(summary.entries) + " keys in the database";
  } else {
    notes = std::string("reads ") + BenchmarkName(summary.source) + "'s database (" +
            std::to_string(summary.entries) + " keys)";
    const std::optional<uint64_t>& found = summary.runs.front().found;
    switch (summary.benchmark) {
      case Benchmark::kReadRandom:
      case Benchmark::kReadMissing:
        notes += ", found " + std::to_string(found.value_or(0)) + " of " +
                 std::to_string(summary.runs.front().operations);
        break;
      case Benchmark::kSeekRandom:
        notes += ", " + std::to_string(found.value_or(0)) + " of " +
                 std::to_string(summary.runs.front().operations) + " seeks landed on an entry";
        break;
      case Benchmark::kReadSeq:
      case Benchmark::kFillSeq:
      case Benchmark::kFillRandom:
      case Benchmark::kFillSync:
      case Benchmark::kOverwrite:
        break;
    }
  }
  if (config.repeat > 1) {
    notes += ", spread " + Fixed(summary.spread_percent, 1) + "%";
  }
  return notes;
}

void PrintTableHeader() {
  std::printf("%-12s %12s %9s %9s %9s %10s %10s  %s\n", "benchmark", "ops/s", "MB/s", "p50(us)",
              "p99(us)", "p99.9(us)", "write-amp", "notes");
}

void PrintRow(const Summary& summary, const Config& config) {
  const std::string none = "-";
  const std::string p50 = summary.latency ? Fixed(summary.latency->p50, 2) : none;
  const std::string p99 = summary.latency ? Fixed(summary.latency->p99, 2) : none;
  const std::string p999 = summary.latency ? Fixed(summary.latency->p999, 2) : none;
  std::printf("%-12s %12s %9s %9s %9s %10s %10s  %s\n", BenchmarkName(summary.benchmark),
              Fixed(summary.ops_per_sec, 0).c_str(), Fixed(summary.mb_per_sec, 2).c_str(),
              p50.c_str(), p99.c_str(), p999.c_str(),
              summary.write_amplification ? summary.write_amplification->c_str() : "-",
              Notes(summary, config).c_str());
  std::fflush(stdout);
}

// ---- Where it ran. ------------------------------------------------------------------------------

struct Machine {
  std::string uname;
  std::string cpu;
  unsigned logical_cores = 0;
};

#if defined(__APPLE__)
std::string CpuBrand() {
  std::array<char, 256> brand{};
  size_t size = brand.size();
  if (sysctlbyname("machdep.cpu.brand_string", brand.data(), &size, nullptr, 0) == 0 && size > 1) {
    return brand.data();
  }
  return "unknown";
}
#else
std::string CpuBrand() {
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    const std::string_view text(line);
    if (text.starts_with("model name")) {
      const size_t colon = text.find(':');
      if (colon != std::string_view::npos) {
        std::string_view name = text.substr(colon + 1);
        name.remove_prefix(std::min(name.find_first_not_of(" \t"), name.size()));
        return std::string(name);
      }
    }
  }
  return "unknown";
}
#endif

Machine DescribeMachine() {
  Machine machine;
  // The fields that `uname -a` prints, in its order.
  utsname names{};
  if (::uname(&names) == 0) {
    machine.uname = std::string(names.sysname) + " " + names.nodename + " " + names.release + " " +
                    names.version + " " + names.machine;
  } else {
    machine.uname = "unknown";
  }
  machine.cpu = CpuBrand();
  machine.logical_cores = std::thread::hardware_concurrency();
  return machine;
}

// Whether the numbers of this build are worth quoting: an optimized build with no sanitizer.
bool IsRepresentative() {
  const std::string_view build_type = SHALE_BENCH_BUILD_TYPE;
  return (build_type == "Release" || build_type == "RelWithDebInfo") &&
         std::string_view(SHALE_BENCH_SANITIZER).empty();
}

// ---- The JSON. ----------------------------------------------------------------------------------

// Writes JSON text. It adds the commas and the indentation, and escapes strings; the caller says
// what is in each object and array, and ends each one it began.
class JsonWriter {
 public:
  void BeginObject() { Begin('{'); }
  void EndObject() { End('}'); }
  void BeginArray() { Begin('['); }
  void EndArray() { End(']'); }

  void Key(std::string_view name) {
    Separate();
    AppendString(name);
    text_ += ": ";
    after_key_ = true;
  }

  void String(std::string_view value) {
    Separate();
    AppendString(value);
  }

  void Integer(uint64_t value) {
    Separate();
    text_ += std::to_string(value);
  }

  // A number that is not finite cannot be said in JSON, so it is null.
  void Number(double value) {
    Separate();
    text_ += std::isfinite(value) ? Fixed(value, 6) : "null";
  }

  void Bool(bool value) {
    Separate();
    text_ += value ? "true" : "false";
  }

  void Null() {
    Separate();
    text_ += "null";
  }

  void OptionalNumber(const std::optional<double>& value) {
    if (value.has_value()) {
      Number(*value);
    } else {
      Null();
    }
  }

  void OptionalString(const std::optional<std::string>& value) {
    if (value.has_value()) {
      String(*value);
    } else {
      Null();
    }
  }

  void OptionalInteger(const std::optional<uint64_t>& value) {
    if (value.has_value()) {
      Integer(*value);
    } else {
      Null();
    }
  }

  // The document: what was written, and a newline.
  std::string Finish() const { return text_ + "\n"; }

 private:
  // Before a value or a key: the comma after the one before it, and a new indented line. The value
  // of a key follows it on its line.
  void Separate() {
    if (after_key_) {
      after_key_ = false;
      return;
    }
    if (!has_items_.empty()) {
      if (has_items_.back()) {
        text_ += ',';
      }
      has_items_.back() = true;
      NewLine(has_items_.size());
    }
  }

  void NewLine(size_t depth) {
    text_ += '\n';
    text_.append(depth * 2, ' ');
  }

  void Begin(char open) {
    Separate();
    text_ += open;
    has_items_.push_back(false);
  }

  void End(char close) {
    const bool had_items = has_items_.back();
    has_items_.pop_back();
    if (had_items) {
      NewLine(has_items_.size());
    }
    text_ += close;
  }

  // The string in quotes. Bytes below 0x20, the quote and the backslash are escaped; the others are
  // kept, which is right for the UTF-8 that a path is.
  void AppendString(std::string_view value) {
    text_ += '"';
    for (const char c : value) {
      const auto byte = static_cast<unsigned char>(c);
      if (c == '"' || c == '\\') {
        text_ += '\\';
        text_ += c;
      } else if (byte < 0x20) {
        std::array<char, 8> escape{};
        std::snprintf(escape.data(), escape.size(), "\\u%04x", static_cast<unsigned>(byte));
        text_ += escape.data();
      } else {
        text_ += c;
      }
    }
    text_ += '"';
  }

  std::string text_;
  // For each object or array that is open: whether it has an item yet.
  std::vector<bool> has_items_;
  bool after_key_ = false;
};

void WriteLatencyFields(JsonWriter* json, const std::optional<Latency>& latency) {
  const auto field = [&](const char* name, double Latency::* member) {
    json->Key(name);
    json->OptionalNumber(latency ? std::optional<double>(latency.value().*member) : std::nullopt);
  };
  field("p50_us", &Latency::p50);
  field("p99_us", &Latency::p99);
  field("p999_us", &Latency::p999);
}

std::string BuildJson(const Config& config, const Machine& machine,
                      const std::vector<Summary>& summaries) {
  JsonWriter json;
  json.BeginObject();
  json.Key("tool");
  json.String("shale_bench");
  json.Key("engine");
  json.String("shale");

  json.Key("config");
  json.BeginObject();
  json.Key("benchmarks");
  json.BeginArray();
  for (const Benchmark benchmark : config.benchmarks) {
    json.String(BenchmarkName(benchmark));
  }
  json.EndArray();
  json.Key("num");
  json.Integer(config.workload.num);
  json.Key("sync_num");
  json.Integer(config.workload.sync_num);
  json.Key("value_size");
  json.Integer(config.workload.value_size);
  json.Key("repeat");
  json.Integer(config.repeat);
  json.Key("seed");
  json.Integer(config.workload.seed);
  json.Key("db");
  json.String(config.workload.db);
  json.Key("compact_before_reads");
  json.Bool(config.compact_before_reads);
  json.EndObject();

  json.Key("machine");
  json.BeginObject();
  json.Key("uname");
  json.String(machine.uname);
  json.Key("cpu");
  json.String(machine.cpu);
  json.Key("logical_cores");
  json.Integer(machine.logical_cores);
  json.EndObject();

  json.Key("build");
  json.BeginObject();
  json.Key("compiler_id");
  json.String(SHALE_BENCH_COMPILER_ID);
  json.Key("compiler_version");
  json.String(SHALE_BENCH_COMPILER_VERSION);
  json.Key("build_type");
  json.String(SHALE_BENCH_BUILD_TYPE);
  json.Key("sanitizer");
  json.String(SHALE_BENCH_SANITIZER);
  json.EndObject();

  json.Key("megabyte_bytes");
  json.Integer(static_cast<uint64_t>(kBenchMegabyte));

  json.Key("results");
  json.BeginArray();
  for (const Summary& summary : summaries) {
    json.BeginObject();
    json.Key("name");
    json.String(BenchmarkName(summary.benchmark));
    json.Key("kind");
    json.String(BenchmarkWrites(summary.benchmark) ? "write" : "read");
    json.Key("source");
    json.String(BenchmarkName(summary.source));
    json.Key("entries");
    json.Integer(summary.entries);
    json.Key("compact_seconds");
    json.OptionalNumber(summary.compact_seconds);
    json.Key("ops_per_sec");
    json.Number(summary.ops_per_sec);
    json.Key("mb_per_sec");
    json.Number(summary.mb_per_sec);
    WriteLatencyFields(&json, summary.latency);
    json.Key("write_amplification");
    json.OptionalString(summary.write_amplification);
    json.Key("spread_percent");
    json.Number(summary.spread_percent);

    json.Key("runs");
    json.BeginArray();
    for (const RunResult& run : summary.runs) {
      json.BeginObject();
      json.Key("operations");
      json.Integer(run.operations);
      json.Key("bytes");
      json.Integer(run.bytes);
      json.Key("seconds");
      json.Number(run.seconds);
      json.Key("ops_per_sec");
      json.Number(run.OpsPerSecond());
      json.Key("mb_per_sec");
      json.Number(run.MbPerSecond());
      WriteLatencyFields(&json, run.latency);
      json.Key("average_us");
      json.OptionalNumber(run.latency ? std::optional<double>(run.latency->average) : std::nullopt);
      json.Key("write_amplification");
      json.OptionalString(run.write_amplification);
      json.Key("found");
      json.OptionalInteger(run.found);
      json.EndObject();
    }
    json.EndArray();
    json.EndObject();
  }
  json.EndArray();
  json.EndObject();
  return json.Finish();
}

// ---- The run. -----------------------------------------------------------------------------------

Status Errno(std::string_view what) { return Status::IOError(what, std::strerror(errno)); }

// A file that is closed when it goes out of scope, if WriteAndClose() has not closed it first;
// WriteAndClose() says whether everything reached the file.
class OutputFile {
 public:
  OutputFile() = default;
  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;
  ~OutputFile() {
    if (file_ != nullptr) {
      std::fclose(file_);  // only on the way out of a failure that is being reported
    }
  }

  Status Open(const std::string& path) {
    file_ = std::fopen(path.c_str(), "w");
    return file_ == nullptr ? Errno("cannot write " + path) : Status::OK();
  }

  Status WriteAndClose(const std::string& path, const std::string& text) {
    const bool written = std::fwrite(text.data(), 1, text.size(), file_) == text.size();
    const bool flushed = std::fflush(file_) == 0;
    const bool closed = std::fclose(file_) == 0;
    file_ = nullptr;
    return written && flushed && closed ? Status::OK() : Errno("cannot write " + path);
  }

 private:
  std::FILE* file_ = nullptr;
};

void PrintHeader(const Config& config, const Machine& machine) {
  std::printf(
      "# shale_bench: engine shale, num %llu, sync_num %llu, value_size %llu, repeat %llu, seed "
      "%llu\n",
      static_cast<unsigned long long>(config.workload.num),
      static_cast<unsigned long long>(config.workload.sync_num),
      static_cast<unsigned long long>(config.workload.value_size),
      static_cast<unsigned long long>(config.repeat),
      static_cast<unsigned long long>(config.workload.seed));
  std::printf("# database: %s\n", config.workload.db.c_str());
  std::printf("# build: %s, %s %s%s%s\n", SHALE_BENCH_BUILD_TYPE, SHALE_BENCH_COMPILER_ID,
              SHALE_BENCH_COMPILER_VERSION,
              std::string_view(SHALE_BENCH_SANITIZER).empty() ? "" : ", ", SHALE_BENCH_SANITIZER);
  std::printf("# machine: %s, %u logical cores\n", machine.cpu.c_str(), machine.logical_cores);
  std::printf("# system: %s\n", machine.uname.c_str());
  if (!IsRepresentative()) {
    std::printf(
        "# WARNING: this build is not an optimized one with no sanitizer: do not quote it\n");
  }
  std::fflush(stdout);
}

// Runs the benchmarks in the order of the list. Returns the status of the first one that fails, and
// says which in `*failed`.
Status RunAll(const Config& config, BenchEngine* engine, std::vector<Summary>* summaries,
              Benchmark* failed) {
  std::optional<Populated> source;
  bool compact_pending = false;
  PrintTableHeader();

  for (const Benchmark benchmark : config.benchmarks) {
    *failed = benchmark;
    Summary summary;
    summary.benchmark = benchmark;

    if (BenchmarkWrites(benchmark)) {
      Populated populated;
      for (uint64_t run = 0; run < config.repeat; run++) {
        RunResult result;
        if (const Status s =
                RunWriteBenchmark(engine, config.workload, benchmark, &populated, &result);
            !s.ok()) {
          return s;
        }
        summary.runs.push_back(std::move(result));
      }
      source = std::move(populated);
      compact_pending = config.compact_before_reads;
    } else {
      // Parsing the flags has seen to it that a write benchmark comes first.
      SHALE_CHECK(source.has_value(), "a read benchmark with no database to read");
      if (compact_pending) {
        const Clock::time_point begin = Clock::now();
        if (const Status s = engine->CompactAll(); !s.ok()) {
          return s;
        }
        summary.compact_seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        compact_pending = false;
        std::printf("# compacted after %s in %s s, which is in no line\n",
                    BenchmarkName(source->written_by), Fixed(*summary.compact_seconds, 3).c_str());
      }
      for (uint64_t run = 0; run < config.repeat; run++) {
        RunResult result;
        if (const Status s = RunReadBenchmark(engine, config.workload, benchmark, *source, &result);
            !s.ok()) {
          return s;
        }
        summary.runs.push_back(std::move(result));
      }
    }

    summary.source = source->written_by;
    summary.entries = source->entries;
    TakeMedians(&summary);
    PrintRow(summary, config);
    summaries->push_back(std::move(summary));
  }
  return Status::OK();
}

int Run(const Config& config) {
  OutputFile json_file;
  if (!config.json.empty()) {
    // Before any benchmark, so that a path that cannot be written is not found out at the end of a
    // long run.
    if (const Status s = json_file.Open(config.json); !s.ok()) {
      std::fprintf(stderr, "shale_bench: %s\n", s.ToString().c_str());
      return kExitFailed;
    }
  }

  const Machine machine = DescribeMachine();
  PrintHeader(config, machine);

  const std::unique_ptr<BenchEngine> engine = NewShaleBenchEngine(Options());
  std::vector<Summary> summaries;
  Benchmark failed = config.benchmarks.front();
  if (const Status s = RunAll(config, engine.get(), &summaries, &failed); !s.ok()) {
    std::fprintf(stderr, "shale_bench: %s failed: %s\n", BenchmarkName(failed),
                 s.ToString().c_str());
    return kExitFailed;
  }

  if (!config.json.empty()) {
    if (const Status s =
            json_file.WriteAndClose(config.json, BuildJson(config, machine, summaries));
        !s.ok()) {
      std::fprintf(stderr, "shale_bench: %s\n", s.ToString().c_str());
      return kExitFailed;
    }
    std::printf("# wrote %s\n", config.json.c_str());
  }
  return kExitOk;
}

// ---- The command line. --------------------------------------------------------------------------

bool ParseNumber(std::string_view text, uint64_t* out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *out);
  return error == std::errc() && end == text.data() + text.size();
}

std::string NeedsValue(const std::string& name) {
  return "--" + name + " needs a value, written --" + name + "=VALUE";
}

// Reads a flag that is true or false: bare, it is true. Returns what is wrong with it, or an empty
// string.
std::string ParseBool(const std::string& name, const std::optional<std::string>& value, bool* out) {
  if (value.has_value() && *value != "true" && *value != "false") {
    return "--" + name + " is true or false, not '" + EscapeBytes(*value) + "'";
  }
  *out = !value.has_value() || *value == "true";
  return "";
}

// Reads --benchmarks=a,b,c. Returns what is wrong with it, or an empty string.
std::string ParseBenchmarks(std::string_view list, std::vector<Benchmark>* out) {
  out->clear();
  size_t start = 0;
  while (true) {
    const size_t comma = list.find(',', start);
    const std::string_view name =
        list.substr(start, comma == std::string_view::npos ? comma : comma - start);
    if (name.empty()) {
      return "--benchmarks is a comma-separated list of benchmark names, and has an empty one";
    }
    const std::optional<Benchmark> benchmark = FindBenchmark(name);
    if (!benchmark.has_value()) {
      return "unknown benchmark '" + EscapeBytes(name) + "'; the benchmarks are " +
             BenchmarkNames();
    }
    out->push_back(*benchmark);
    if (comma == std::string_view::npos) {
      return "";
    }
    start = comma + 1;
  }
}

struct NumberFlag {
  const char* name;
  uint64_t* target;
  uint64_t least;
  uint64_t most;
};

// Reads the flags into `*config`, and sets `*help` for --help. Returns what is wrong with them, or
// an empty string.
std::string ParseFlags(const std::vector<std::string>& words, Config* config, bool* help) {
  const std::array<NumberFlag, 5> number_flags = {{
      {"num", &config->workload.num, 1, kMaxNum},
      {"sync_num", &config->workload.sync_num, 1, kMaxNum},
      {"value_size", &config->workload.value_size, 1, kMaxBenchValueSize},
      {"repeat", &config->repeat, 1, kMaxRepeat},
      {"seed", &config->workload.seed, 0, UINT64_MAX},
  }};
  config->benchmarks.assign(kAllBenchmarks.begin(), kAllBenchmarks.end());
  *help = false;

  for (const std::string& word : words) {
    if (!word.starts_with("--")) {
      return "'" + EscapeBytes(word) + "' is not a flag";
    }
    const size_t equals = word.find('=');
    const std::string name = word.substr(2, equals == std::string::npos ? equals : equals - 2);
    const std::optional<std::string> value =
        equals == std::string::npos ? std::nullopt
                                    : std::optional<std::string>(word.substr(equals + 1));

    if (name == "help" || name == "compact_before_reads") {
      const std::string wrong =
          ParseBool(name, value, name == "help" ? help : &config->compact_before_reads);
      if (!wrong.empty()) {
        return wrong;
      }
      continue;
    }

    const auto number_flag =
        std::find_if(number_flags.begin(), number_flags.end(),
                     [&name](const NumberFlag& flag) { return name == flag.name; });
    const bool is_text_flag = name == "db" || name == "json" || name == "benchmarks";
    if (number_flag == number_flags.end() && !is_text_flag) {
      return "unknown flag --" + EscapeBytes(name);
    }
    if (!value.has_value()) {
      return NeedsValue(name);
    }

    if (number_flag != number_flags.end()) {
      uint64_t number = 0;
      if (!ParseNumber(*value, &number) || number < number_flag->least ||
          number > number_flag->most) {
        return "--" + name + " is a whole number from " + std::to_string(number_flag->least) +
               " to " + std::to_string(number_flag->most) + ", not '" + EscapeBytes(*value) + "'";
      }
      *number_flag->target = number;
    } else if (name == "benchmarks") {
      if (const std::string wrong = ParseBenchmarks(*value, &config->benchmarks); !wrong.empty()) {
        return wrong;
      }
    } else if (value->empty()) {
      return "--" + name + " needs a path, and it is empty";
    } else {
      (name == "db" ? config->workload.db : config->json) = *value;
    }
  }

  if (*help) {
    return "";
  }
  if (config->workload.db.empty()) {
    return "--db=PATH is needed to say where the database goes";
  }
  // Checked before anything runs, so that a list that cannot work is not found out half way.
  bool written = false;
  for (const Benchmark benchmark : config->benchmarks) {
    if (BenchmarkWrites(benchmark)) {
      written = true;
    } else if (!written) {
      return std::string(BenchmarkName(benchmark)) +
             " needs a database to read: put a write benchmark before it in --benchmarks";
    }
  }
  return "";
}

int Main(const std::vector<std::string>& words) {
  Config config;
  bool help = false;
  if (const std::string wrong = ParseFlags(words, &config, &help); !wrong.empty()) {
    std::fprintf(stderr, "shale_bench: %s\n%s", wrong.c_str(), kUsage);
    return kExitUsage;
  }
  if (help) {
    std::fputs(kUsage, stdout);
    return std::fflush(stdout) == 0 ? kExitOk : kExitFailed;
  }
  const int exit_status = Run(config);
  // A table that did not get written is a failure: a run that says OK after losing its output is
  // worse than one that fails.
  const bool flushed = std::fflush(stdout) == 0;
  if ((!flushed || std::ferror(stdout) != 0) && exit_status == kExitOk) {
    std::fprintf(stderr, "shale_bench: cannot write to standard output\n");
    return kExitFailed;
  }
  return exit_status;
}

}  // namespace
}  // namespace shale

int main(int argc, char** argv) {
  try {
    std::vector<std::string> words;
    for (int i = 1; i < argc; i++) {
      words.emplace_back(argv[i]);
    }
    return shale::Main(words);
  } catch (const std::exception& e) {
    // The engine does not throw; the standard library does, for memory it cannot get.
    std::fprintf(stderr, "shale_bench: %s\n", e.what());
    return shale::kExitFailed;
  }
}
