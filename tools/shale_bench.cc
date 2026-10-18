// shale_bench: db_bench-style workloads against shale, and against LevelDB for comparison, with
// numbers that can be quoted.
//
// A benchmark here is a loop of operations on a database, timed. Keys are the 16 digits of an
// index, "%016llu", and values are --value_size bytes from a seeded generator, so a run is a
// function of its flags: the same flags make the same operations in the same order, on any machine,
// and what differs between two runs of them is the machine. The workloads call the engine through
// BenchEngine alone, so that the same workloads, timed by the same code, run on another engine:
// that is --engine=leveldb, in a build that found LevelDB, and what lets a row of one engine be set
// next to the same row of the other.
//
// What is measured, since a number is only as good as its definition:
//
// - Each benchmark is run --repeat times, and the table shows the median of each column over the
//   runs. A run of a write benchmark starts from an empty database. Every run of a benchmark makes
//   the same operations, so the runs differ only in how long they took, and that is what the
//   spread in the notes shows: (fastest - slowest) / median of the runs' ops/s. The later runs of
//   a read benchmark find the earlier runs' blocks in the engine's cache and in the operating
//   system's, which the median over the runs includes: with --repeat=3 it is a warm run.
// - ops/s is the operations of a run over the wall-clock time of its loop, as db_bench does. The
//   loop also makes each key and value, reads the clock twice around each call and checks each
//   answer, which is a few percent of the time at the default value size and more at a large one,
//   the same for every engine. The JSON gives engine_seconds, the time inside the engine's calls,
//   next to seconds for every run, so that the share of the harness can be seen.
// - The latency columns are the nearest-rank percentiles of the time of the engine call alone, from
//   a clock read before it to one after, over every operation of a run. The output and the JSON
//   give the nominal period of the clock and the smallest step seen between two readings of it.
//   Where the clock is coarse (41 ns on Apple Silicon) that step is its tick and a latency is a
//   multiple of it, so a call of a few ticks is a few ticks; where it is fine it is what one
//   reading costs, and says nothing about the resolution. readseq has no latencies: a scan is one
//   call, and the engine's interface does not say when each entry arrived.
// - MB/s is the key and value bytes of the entries an operation wrote, returned or visited, over
//   the same wall-clock time, with a megabyte of 2^20 bytes as in db_bench. A lookup of a key that
//   is not there moves no entry, so readmissing is 0.
// - A write benchmark runs on a database it has just made, so its write amplification is the
//   engine's own figure for that run: log, flushes and compactions, since the database was opened,
//   over the bytes the callers wrote. The reads show none.
//
// What is checked, since a benchmark of an engine that gives wrong answers measures nothing: the
// write benchmarks record which keys they wrote and which put wrote each value, and every read is
// compared with the record. A key that was written and is not found, a key that was never written
// and is, a value of the wrong size, a value that is not the one last put to its key, a scan that
// visits another number of entries than the database holds, and a seek that lands where it should
// not, all end the run with status 1 and name the key.
//
// What the output names of the machine is its system, release and architecture, its CPU and the
// figures of its clock, and of the database the last component of its path: nothing that says
// whose machine it was.
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
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "bench_engine.h"
#include "bench_report.h"
#include "bench_stats.h"
#include "bench_workloads.h"
#include "cli_args.h"
#include "shale/options.h"
#include "shale/status.h"
#include "util/check.h"

#if defined(SHALE_BENCH_HAVE_LEVELDB)
#include "bench_leveldb.h"
#endif

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
#ifndef SHALE_BENCH_COVERAGE
#define SHALE_BENCH_COVERAGE 0
#endif
#ifndef SHALE_BENCH_FLAW
#define SHALE_BENCH_FLAW ""
#endif

namespace shale {
namespace {

// The exit statuses.
constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;  // a benchmark failed, or its answers were wrong, or I/O failed
constexpr int kExitUsage = 2;   // the command line is wrong

constexpr char kUsage[] =
    "usage: shale_bench --db=PATH [--engine=shale|leveldb] [--benchmarks=LIST]\n"
    "                   [--num=1000000] [--sync_num=10000] [--value_size=100] [--repeat=3]\n"
    "                   [--seed=301] [--json=FILE] [--compact_before_reads]\n"
    "\n"
    "Runs db_bench-style workloads against shale and prints one line for each benchmark:\n"
    "the median over --repeat runs of operations a second, megabytes a second (of 2^20 bytes,\n"
    "counting the key and value bytes of the entries moved), the p50, p99 and p99.9 latency of\n"
    "the engine call in microseconds (nearest rank, over every operation), and the engine's\n"
    "write amplification. Lines that start with # say what the run was and where; the table\n"
    "has no other kind.\n"
    "\n"
    "--engine=shale|leveldb picks the database that the workloads run on; shale is the default.\n"
    "leveldb is there only in a build that found LevelDB (see docs/benchmarks.md). LevelDB gets "
    "the\n"
    "same workloads, keys, values and timing as shale, and settings that put it level with\n"
    "shale's defaults: a write buffer of 4 MiB, blocks of 4096 bytes, a Bloom filter of 10 bits a\n"
    "key, a block cache of 8 MiB, 1000 open files, no compression, and checksums verified on "
    "every\n"
    "read. It reports no write amplification: its column shows n/a.\n"
    "\n"
    "PATH is destroyed and made again by every write benchmark: give it a directory that holds\n"
    "nothing you want. The output and the JSON name it by the last component of PATH alone.\n"
    "Keys are 16 digits, \"%016llu\". Values are --value_size bytes of seeded pseudo-random data,\n"
    "the first eight of which are the number of the put that wrote them, so that a read can tell\n"
    "a stale value, or another key's, from the right one. The same flags make the same\n"
    "operations in the same order.\n"
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
    "The default order is fillseq, fillrandom, fillsync, overwrite, then the four reads.\n"
    "It runs fillsync where the reads still see the large database written before it: they\n"
    "follow overwrite, and read all --num keys. A read benchmark reads the database that the\n"
    "most recent write benchmark before it in the list left, and its line says which; one\n"
    "placed after fillsync reads at most --sync_num keys, a database that small.\n"
    "--compact_before_reads compacts the database once between a write benchmark and the first\n"
    "read benchmark after it; that is not timed, and is in no line. The reads are checked\n"
    "against a record of what was written, and a wrong answer ends the run with status 1.\n"
    "\n"
    "How the figures are made. ops/s is the operations of a run over the wall-clock time of its\n"
    "loop, which also makes each key and value and checks each answer; the JSON gives\n"
    "engine_seconds, the time inside the engine's calls, next to seconds for every run. A\n"
    "latency is the time of one engine call, a difference of two std::chrono::steady_clock\n"
    "readings. The run prints the smallest step it saw between two readings of that clock (41\n"
    "ns on Apple Silicon). Where the clock is coarse that step is its tick, and every latency\n"
    "is a multiple of it; where the clock is fine it is what one reading costs, and says\n"
    "nothing about the resolution, which the clock's nominal period gives. MB/s counts the\n"
    "entries moved, and readmissing moves none, so it shows 0. readseq shows no latencies: a\n"
    "scan is one call.\n"
    "\n"
    "Each benchmark runs --repeat times, every run from the same keys and values. The notes\n"
    "show the spread of the runs: (fastest - slowest) / median of the runs' ops/s, in percent.\n"
    "The runs of a read benchmark follow one another, so the later ones find the blocks of the\n"
    "earlier ones in the engine's cache and in the operating system's: with --repeat=3\n"
    "the median is a warm run, and only --repeat=1 is a cold one.\n"
    "\n"
    "--json=FILE writes the same figures, every run's own numbers, the machine (the system's\n"
    "name, release and architecture, the CPU brand, the logical cores, the clock's figures) and\n"
    "the compiler and build type, to FILE, and no host name. A build that is not an optimized\n"
    "one without a sanitizer, coverage or a flaw says so in the output: its numbers are not\n"
    "worth quoting. FILE is made when the run starts, so that a path that cannot be written is\n"
    "known at once; a run that fails leaves it empty.\n"
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
  std::string engine = "shale";
  bool compact_before_reads = false;

  Config() {
    workload.num = 1000000;
    workload.sync_num = 10000;
    workload.value_size = 100;
    workload.seed = 301;
  }
};

using Clock = std::chrono::steady_clock;

// ---- The engines.
// --------------------------------------------------------------------------------

// The names of the engines that this build has, as the usage lists them.
std::string EngineNames() {
#if defined(SHALE_BENCH_HAVE_LEVELDB)
  return "shale, leveldb";
#else
  return "shale";
#endif
}

// Returns what is wrong with the engine name, or an empty string. The engine that is not in this
// build is named for what it is, and not called unknown: it exists, and the build is what lacks it.
std::string CheckEngine(const std::string& name) {
  if (name == "shale") {
    return "";
  }
#if defined(SHALE_BENCH_HAVE_LEVELDB)
  if (name == "leveldb") {
    return "";
  }
#else
  if (name == "leveldb") {
    return "this build has no leveldb engine: it is built when SHALE_BUILD_BENCH is ON and CMake "
           "finds LevelDB, with CMAKE_PREFIX_PATH set to where it is installed";
  }
#endif
  return "unknown engine '" + EscapeBytes(name) + "'; the engines are " + EngineNames();
}

// Makes the engine that CheckEngine() accepted.
std::unique_ptr<BenchEngine> MakeEngine(const std::string& name) {
#if defined(SHALE_BENCH_HAVE_LEVELDB)
  if (name == "leveldb") {
    return NewLevelDBBenchEngine();
  }
#endif
  SHALE_CHECK(name == "shale",
              "an engine that CheckEngine() accepted and MakeEngine() does not know");
  return NewShaleBenchEngine(Options());
}

// ---- The output. --------------------------------------------------------------------------------

void PrintTableHeader() { std::printf("%s\n", FormatTableHeader().c_str()); }

void PrintRow(const Summary& summary, const Config& config) {
  std::printf("%s\n", FormatRow(summary, config.repeat).c_str());
  std::fflush(stdout);
}

// ---- Where it ran. ------------------------------------------------------------------------------

struct Machine {
  // The system's name, release and architecture, as `uname -srm` prints them.
  std::string uname;
  std::string cpu;
  unsigned logical_cores = 0;
  // The nominal period of the steady clock, in nanoseconds, as its type says.
  uint64_t clock_nominal_period_ns = 0;
  // The smallest step seen between two readings of the steady clock, in nanoseconds: its tick if it
  // is coarse, and what a reading costs if it is fine.
  uint64_t clock_smallest_step_ns = 0;
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

// The smallest step seen between two readings of std::chrono::steady_clock, in nanoseconds: the
// shortest time, over many tries, from one reading to the next one that differs from it. On a clock
// that is coarse this is its tick, and every latency is a multiple of it. On a clock that is fine
// two readings never agree, and this is what one reading costs, which says nothing about the
// resolution.
uint64_t SmallestClockStepNanos() {
  uint64_t step = std::numeric_limits<uint64_t>::max();
  for (int i = 0; i < 1000; i++) {
    const Clock::time_point first = Clock::now();
    Clock::time_point next = Clock::now();
    while (next == first) {
      next = Clock::now();
    }
    step = std::min(
        step, static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(next - first).count()));
  }
  return step;
}

// The period that the type of the steady clock gives it, in nanoseconds.
constexpr uint64_t ClockNominalPeriodNanos() {
  return static_cast<uint64_t>(Clock::period::num) * 1'000'000'000ULL /
         static_cast<uint64_t>(Clock::period::den);
}

Machine DescribeMachine() {
  Machine machine;
  // The system's name, release and architecture, which `uname -srm` prints. The host name is not
  // among them, nor is the kernel's version string, which names the host that built it on some
  // systems: a result that is quoted must not say whose machine it ran on.
  utsname names{};
  if (::uname(&names) == 0) {
    machine.uname = std::string(names.sysname) + " " + names.release + " " + names.machine;
  } else {
    machine.uname = "unknown";
  }
  machine.cpu = CpuBrand();
  machine.logical_cores = std::thread::hardware_concurrency();
  machine.clock_nominal_period_ns = ClockNominalPeriodNanos();
  machine.clock_smallest_step_ns = SmallestClockStepNanos();
  return machine;
}

// Whether the numbers of this build are worth quoting.
bool IsRepresentative() {
  return IsRepresentativeBuild(SHALE_BENCH_BUILD_TYPE, SHALE_BENCH_SANITIZER,
                               SHALE_BENCH_COVERAGE != 0, SHALE_BENCH_FLAW);
}

// The last component of `path`: what follows the last slash, trailing slashes not counted. A path
// is how a result would say whose account and directory it ran in, and the last component is what
// is needed to tell one database from another.
std::string LastComponent(const std::string& path) {
  const size_t end = path.find_last_not_of('/');
  if (end == std::string::npos) {
    return path.empty() ? "" : "/";
  }
  const size_t slash = path.find_last_of('/', end);
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  return path.substr(start, end - start + 1);
}

// ---- The JSON. ----------------------------------------------------------------------------------

std::string BuildJson(const Config& config, const std::string& engine, const Machine& machine,
                      const std::vector<Summary>& summaries) {
  JsonWriter json;
  json.BeginObject();
  json.Key("tool");
  json.String("shale_bench");
  json.Key("engine");
  json.String(engine);

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
  json.String(LastComponent(config.workload.db));
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
  json.Key("clock_nominal_period_ns");
  json.Integer(machine.clock_nominal_period_ns);
  json.Key("clock_smallest_step_ns");
  json.Integer(machine.clock_smallest_step_ns);
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
  json.Key("coverage");
  json.Bool(SHALE_BENCH_COVERAGE != 0);
  json.Key("flaw");
  json.String(SHALE_BENCH_FLAW);
  json.EndObject();

  json.Key("megabyte_bytes");
  json.Integer(static_cast<uint64_t>(kBenchMegabyte));

  json.Key("results");
  WriteResultsJson(&json, summaries);
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

void PrintHeader(const Config& config, const std::string& engine, const Machine& machine) {
  std::printf(
      "# shale_bench: engine %s, num %llu, sync_num %llu, value_size %llu, repeat %llu, seed "
      "%llu\n",
      engine.c_str(), static_cast<unsigned long long>(config.workload.num),
      static_cast<unsigned long long>(config.workload.sync_num),
      static_cast<unsigned long long>(config.workload.value_size),
      static_cast<unsigned long long>(config.repeat),
      static_cast<unsigned long long>(config.workload.seed));
  std::printf("# database: %s\n", LastComponent(config.workload.db).c_str());
  // What the build has that changes what is measured is named after the compiler, each if it is
  // there.
  std::string extras;
  if (!std::string_view(SHALE_BENCH_SANITIZER).empty()) {
    extras += std::string(", sanitizer ") + SHALE_BENCH_SANITIZER;
  }
  if (SHALE_BENCH_COVERAGE != 0) {
    extras += ", coverage";
  }
  if (!std::string_view(SHALE_BENCH_FLAW).empty()) {
    extras += std::string(", flaw ") + SHALE_BENCH_FLAW;
  }
  std::printf("# build: %s, %s %s%s\n", SHALE_BENCH_BUILD_TYPE, SHALE_BENCH_COMPILER_ID,
              SHALE_BENCH_COMPILER_VERSION, extras.c_str());
  std::printf("# machine: %s, %u logical cores\n", machine.cpu.c_str(), machine.logical_cores);
  std::printf("# system: %s\n", machine.uname.c_str());
  std::printf(
      "# clock: steady_clock, nominal period %llu ns, smallest step seen between two readings "
      "%llu ns (the tick if the clock is coarse, else the cost of a reading)\n",
      static_cast<unsigned long long>(machine.clock_nominal_period_ns),
      static_cast<unsigned long long>(machine.clock_smallest_step_ns));
  if (!IsRepresentative()) {
    std::printf(
        "# WARNING: this build is not an optimized one with no sanitizer, coverage or flaw: do not "
        "quote it\n");
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
  const std::unique_ptr<BenchEngine> engine = MakeEngine(config.engine);
  PrintHeader(config, engine->Name(), machine);

  std::vector<Summary> summaries;
  Benchmark failed = config.benchmarks.front();
  if (const Status s = RunAll(config, engine.get(), &summaries, &failed); !s.ok()) {
    std::fprintf(stderr, "shale_bench: %s failed: %s\n", BenchmarkName(failed),
                 s.ToString().c_str());
    return kExitFailed;
  }

  if (!config.json.empty()) {
    if (const Status s = json_file.WriteAndClose(
            config.json, BuildJson(config, engine->Name(), machine, summaries));
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
    const bool is_text_flag =
        name == "db" || name == "json" || name == "benchmarks" || name == "engine";
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
    } else if (name == "engine") {
      if (const std::string wrong = CheckEngine(*value); !wrong.empty()) {
        return wrong;
      }
      config->engine = *value;
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
