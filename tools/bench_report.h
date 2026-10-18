#ifndef SHALE_TOOLS_BENCH_REPORT_H_
#define SHALE_TOOLS_BENCH_REPORT_H_

// What shale_bench makes of the runs of a benchmark, and how it says it: the figures taken over the
// runs, one line of the table, and the JSON of the results. They are in a library of their own so
// that the tests can give them figures of their own choosing, all different, and see that each one
// lands under its own label.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bench_workloads.h"

namespace shale {

/// A benchmark's runs and the figures taken over them.
struct Summary {
  /// The benchmark.
  Benchmark benchmark = Benchmark::kFillSeq;
  /// The benchmark that wrote the database it ran on.
  Benchmark source = Benchmark::kFillSeq;
  /// How many keys that database held.
  uint64_t entries = 0;
  /// How long the compaction before this benchmark took, if there was one.
  std::optional<double> compact_seconds;
  /// Every run, in the order they were made.
  std::vector<RunResult> runs;

  /// The median over the runs of operations a second.
  double ops_per_sec = 0;
  /// The median over the runs of megabytes a second.
  double mb_per_sec = 0;
  /// The median over the runs of each latency, if the runs have any.
  std::optional<Latency> latency;
  /// The median of the runs' write amplification, as text, if the runs have any.
  std::optional<std::string> write_amplification;
  /// How far apart the fastest and the slowest run were, as a share of the median, in percent.
  double spread_percent = 0;
};

/// Fills in the figures of `*summary` from its runs, each as the median over the runs of that
/// figure: the rates, the latencies, the write amplification, and the spread. Needs at least one
/// run.
/// @param summary the summary, whose `runs` are set.
void TakeMedians(Summary* summary);

/// Returns `value` with `digits` digits after the point.
std::string Fixed(double value, int digits);

/// Returns the header line of the table, without the newline.
std::string FormatTableHeader();

/// Returns the line of the table for `summary`, without the newline. Its columns are the ones that
/// FormatTableHeader() names, and a column that the benchmark has no figure for is a dash.
/// @param summary the summary to show.
/// @param repeat how many runs there were; the notes show the spread only if more than one.
std::string FormatRow(const Summary& summary, uint64_t repeat);

/// Writes JSON text. It adds the commas and the indentation, and escapes strings; the caller says
/// what is in each object and array, and ends each one it began.
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
    text_ += std::isfinite(value) ? Fixed(value, 9) : "null";
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

/// Writes the array of the results of `summaries` as the value of the key that is open in `*json`:
/// for each benchmark its summary, with the figures of every run.
/// @param json where to write.
/// @param summaries the summaries, in the order they are shown.
void WriteResultsJson(JsonWriter* json, const std::vector<Summary>& summaries);

}  // namespace shale

#endif  // SHALE_TOOLS_BENCH_REPORT_H_
