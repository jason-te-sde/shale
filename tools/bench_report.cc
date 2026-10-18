#include "bench_report.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bench_stats.h"
#include "bench_workloads.h"

namespace shale {

namespace {

template <typename Get>
double MedianOf(const std::vector<RunResult>& runs, Get get) {
  std::vector<double> values;
  values.reserve(runs.size());
  for (const RunResult& run : runs) {
    values.push_back(get(run));
  }
  return Median(std::move(values));
}

}  // namespace

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

namespace {

std::string Notes(const Summary& summary, uint64_t repeat) {
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
  if (repeat > 1) {
    notes += ", spread " + Fixed(summary.spread_percent, 1) + "%";
  }
  return notes;
}

}  // namespace

std::string FormatTableHeader() {
  std::array<char, 256> line{};
  std::snprintf(line.data(), line.size(), "%-12s %12s %9s %9s %9s %10s %10s  %s", "benchmark",
                "ops/s", "MB/s", "p50(us)", "p99(us)", "p99.9(us)", "write-amp", "notes");
  return line.data();
}

std::string FormatRow(const Summary& summary, uint64_t repeat) {
  const std::string none = "-";
  const std::string p50 = summary.latency ? Fixed(summary.latency->p50, 2) : none;
  const std::string p99 = summary.latency ? Fixed(summary.latency->p99, 2) : none;
  const std::string p999 = summary.latency ? Fixed(summary.latency->p999, 2) : none;
  const std::string ops = Fixed(summary.ops_per_sec, 0);
  const std::string mb = Fixed(summary.mb_per_sec, 2);
  const std::string notes = Notes(summary, repeat);
  // The row can be long, with notes that are not bounded, so it is made in a string.
  std::string line(512 + notes.size(), '\0');
  const int written = std::snprintf(
      line.data(), line.size(), "%-12s %12s %9s %9s %9s %10s %10s  %s",
      BenchmarkName(summary.benchmark), ops.c_str(), mb.c_str(), p50.c_str(), p99.c_str(),
      p999.c_str(), summary.write_amplification ? summary.write_amplification->c_str() : "-",
      notes.c_str());
  line.resize(written > 0 ? static_cast<size_t>(written) : 0);
  return line;
}

namespace {

void WriteLatencyFields(JsonWriter* json, const std::optional<Latency>& latency) {
  const auto field = [&](const char* name, double Latency::* member) {
    json->Key(name);
    json->OptionalNumber(latency ? std::optional<double>(latency.value().*member) : std::nullopt);
  };
  field("p50_us", &Latency::p50);
  field("p99_us", &Latency::p99);
  field("p999_us", &Latency::p999);
}

}  // namespace

void WriteResultsJson(JsonWriter* json, const std::vector<Summary>& summaries) {
  json->BeginArray();
  for (const Summary& summary : summaries) {
    json->BeginObject();
    json->Key("name");
    json->String(BenchmarkName(summary.benchmark));
    json->Key("kind");
    json->String(BenchmarkWrites(summary.benchmark) ? "write" : "read");
    json->Key("source");
    json->String(BenchmarkName(summary.source));
    json->Key("entries");
    json->Integer(summary.entries);
    json->Key("compact_seconds");
    json->OptionalNumber(summary.compact_seconds);
    json->Key("ops_per_sec");
    json->Number(summary.ops_per_sec);
    json->Key("mb_per_sec");
    json->Number(summary.mb_per_sec);
    WriteLatencyFields(json, summary.latency);
    json->Key("write_amplification");
    json->OptionalString(summary.write_amplification);
    json->Key("spread_percent");
    json->Number(summary.spread_percent);

    json->Key("runs");
    json->BeginArray();
    for (const RunResult& run : summary.runs) {
      json->BeginObject();
      json->Key("operations");
      json->Integer(run.operations);
      json->Key("bytes");
      json->Integer(run.bytes);
      json->Key("seconds");
      json->Number(run.seconds);
      json->Key("engine_seconds");
      json->Number(run.engine_seconds);
      json->Key("ops_per_sec");
      json->Number(run.OpsPerSecond());
      json->Key("mb_per_sec");
      json->Number(run.MbPerSecond());
      WriteLatencyFields(json, run.latency);
      json->Key("average_us");
      json->OptionalNumber(run.latency ? std::optional<double>(run.latency->average)
                                       : std::nullopt);
      json->Key("write_amplification");
      json->OptionalString(run.write_amplification);
      json->Key("found");
      json->OptionalInteger(run.found);
      json->EndObject();
    }
    json->EndArray();
    json->EndObject();
  }
  json->EndArray();
}

}  // namespace shale
