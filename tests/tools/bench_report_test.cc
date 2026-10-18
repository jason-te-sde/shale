// What shale_bench prints and writes, with figures made up so that every one is different from
// every other. A figure that lands under another figure's label is then a wrong word in the table
// or a wrong value under a key, and not a number that happens to be equal. The real runs are
// checked against their own JSON by shale_bench_test.sh, which cannot choose its figures: two of
// them may be the same, and a swap of those goes unseen.

#include "bench_report.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "bench_workloads.h"

namespace shale {
namespace {

std::vector<std::string> Words(const std::string& line) {
  std::istringstream stream(line);
  std::vector<std::string> words;
  for (std::string word; stream >> word;) {
    words.push_back(word);
  }
  return words;
}

bool Has(const std::string& text, const std::string& fragment) {
  return text.find(fragment) != std::string::npos;
}

// A run with the figures given. Its rates follow from them: operations over seconds, and bytes over
// a megabyte of 2^20 over seconds.
RunResult MakeRun(uint64_t operations, uint64_t megabytes, double seconds, double p50, double p99,
                  double p999, double average, const char* write_amplification) {
  RunResult run;
  run.operations = operations;
  run.bytes = megabytes * 1048576;
  run.seconds = seconds;
  run.latency = Latency{p50, p99, p999, average};
  run.write_amplification = write_amplification;
  return run;
}

// A write benchmark's summary in which no figure equals another, nor any figure of its runs.
Summary DistinctSummary() {
  Summary summary;
  summary.benchmark = Benchmark::kFillRandom;
  summary.source = Benchmark::kFillRandom;
  summary.entries = 777;
  summary.ops_per_sec = 123456.4;
  summary.mb_per_sec = 22.226;
  summary.latency = Latency{3.333, 44.444, 555.551, 9.999};
  summary.write_amplification = "6.66";
  summary.spread_percent = 12.34;
  summary.runs.push_back(MakeRun(1000, 3, 0.5, 1.1, 2.2, 3.3, 1.5, "2.50"));
  summary.runs.push_back(MakeRun(1000, 5, 0.25, 4.4, 5.5, 6.6, 4.5, "2.75"));
  return summary;
}

// A read benchmark's summary with no latencies, as readseq has.
Summary ScanSummary() {
  Summary summary;
  summary.benchmark = Benchmark::kReadSeq;
  summary.source = Benchmark::kOverwrite;
  summary.entries = 777;
  summary.ops_per_sec = 4000000;
  summary.mb_per_sec = 440.5;
  RunResult run;
  run.operations = 777;
  run.bytes = 90132;
  run.seconds = 0.001;
  run.found = 777;
  summary.runs.push_back(run);
  return summary;
}

// The columns of a table row, by the label that the header gives each.
std::map<std::string, std::string> ByLabel(const std::string& header, const std::string& row) {
  const std::vector<std::string> labels = Words(header);
  const std::vector<std::string> cells = Words(row);
  std::map<std::string, std::string> columns;
  // The last label, "notes", is the rest of the line, which has spaces in it.
  for (size_t i = 0; i + 1 < labels.size(); i++) {
    columns[labels[i]] = i < cells.size() ? cells[i] : "";
  }
  return columns;
}

// ---- The table. ---------------------------------------------------------------------------------

TEST(BenchReportTest, EveryColumnOfTheTableHoldsTheFigureItIsLabelledWith) {
  const std::map<std::string, std::string> columns =
      ByLabel(FormatTableHeader(), FormatRow(DistinctSummary(), 3));
  EXPECT_EQ(columns.size(), 7u) << "the benchmark, ops/s, MB/s, three latencies and write-amp";
  EXPECT_EQ(columns.at("benchmark"), "fillrandom");
  EXPECT_EQ(columns.at("ops/s"), "123456");
  EXPECT_EQ(columns.at("MB/s"), "22.23");
  EXPECT_EQ(columns.at("p50(us)"), "3.33");
  EXPECT_EQ(columns.at("p99(us)"), "44.44");
  EXPECT_EQ(columns.at("p99.9(us)"), "555.55");
  EXPECT_EQ(columns.at("write-amp"), "6.66");
}

TEST(BenchReportTest, ABenchmarkWithNoLatenciesAndNoWriteAmplificationShowsDashes) {
  const std::map<std::string, std::string> columns =
      ByLabel(FormatTableHeader(), FormatRow(ScanSummary(), 3));
  EXPECT_EQ(columns.at("benchmark"), "readseq");
  EXPECT_EQ(columns.at("ops/s"), "4000000");
  EXPECT_EQ(columns.at("MB/s"), "440.50");
  EXPECT_EQ(columns.at("p50(us)"), "-");
  EXPECT_EQ(columns.at("p99(us)"), "-");
  EXPECT_EQ(columns.at("p99.9(us)"), "-");
  EXPECT_EQ(columns.at("write-amp"), "-");
}

TEST(BenchReportTest, TheNotesSayWhatTheDatabaseWasAndHowFarApartTheRunsWere) {
  EXPECT_TRUE(Has(FormatRow(DistinctSummary(), 3), "777 keys in the database, spread 12.3%"));
  EXPECT_FALSE(Has(FormatRow(DistinctSummary(), 1), "spread")) << "one run has no spread to show";
  EXPECT_TRUE(Has(FormatRow(ScanSummary(), 3), "reads overwrite's database (777 keys), spread"));

  Summary random_reads = ScanSummary();
  random_reads.benchmark = Benchmark::kReadRandom;
  random_reads.runs.front().operations = 900;
  random_reads.runs.front().found = 555;
  EXPECT_TRUE(
      Has(FormatRow(random_reads, 1), "reads overwrite's database (777 keys), found 555 of 900"));
  random_reads.benchmark = Benchmark::kSeekRandom;
  EXPECT_TRUE(Has(FormatRow(random_reads, 1), "555 of 900 seeks landed on an entry"));
}

// ---- The JSON. ----------------------------------------------------------------------------------

std::string ResultsJson(const std::vector<Summary>& summaries) {
  JsonWriter json;
  WriteResultsJson(&json, summaries);
  return json.Finish();
}

TEST(BenchReportTest, EveryKeyOfASummaryHoldsTheFigureItIsNamedFor) {
  const std::string json = ResultsJson({DistinctSummary()});
  EXPECT_TRUE(Has(json, "\"name\": \"fillrandom\""));
  EXPECT_TRUE(Has(json, "\"kind\": \"write\""));
  EXPECT_TRUE(Has(json, "\"source\": \"fillrandom\""));
  EXPECT_TRUE(Has(json, "\"entries\": 777"));
  EXPECT_TRUE(Has(json, "\"compact_seconds\": null"));
  EXPECT_TRUE(Has(json, "\"ops_per_sec\": 123456.400000000"));
  EXPECT_TRUE(Has(json, "\"mb_per_sec\": 22.226000000"));
  EXPECT_TRUE(Has(json, "\"p50_us\": 3.333000000"));
  EXPECT_TRUE(Has(json, "\"p99_us\": 44.444000000"));
  EXPECT_TRUE(Has(json, "\"p999_us\": 555.551000000"));
  EXPECT_TRUE(Has(json, "\"write_amplification\": \"6.66\""));
  EXPECT_TRUE(Has(json, "\"spread_percent\": 12.340000000"));
}

TEST(BenchReportTest, EveryKeyOfARunHoldsTheFigureItIsNamedFor) {
  const std::string json = ResultsJson({DistinctSummary()});
  // The first run: 1000 operations and 3 megabytes in half a second.
  EXPECT_TRUE(Has(json, "\"operations\": 1000"));
  EXPECT_TRUE(Has(json, "\"bytes\": 3145728"));
  EXPECT_TRUE(Has(json, "\"seconds\": 0.500000000"));
  EXPECT_TRUE(Has(json, "\"ops_per_sec\": 2000.000000000"));
  EXPECT_TRUE(Has(json, "\"mb_per_sec\": 6.000000000"));
  EXPECT_TRUE(Has(json, "\"p50_us\": 1.100000000"));
  EXPECT_TRUE(Has(json, "\"p99_us\": 2.200000000"));
  EXPECT_TRUE(Has(json, "\"p999_us\": 3.300000000"));
  EXPECT_TRUE(Has(json, "\"average_us\": 1.500000000"));
  EXPECT_TRUE(Has(json, "\"write_amplification\": \"2.50\""));
  // The second: 5 megabytes in a quarter of a second.
  EXPECT_TRUE(Has(json, "\"seconds\": 0.250000000"));
  EXPECT_TRUE(Has(json, "\"ops_per_sec\": 4000.000000000"));
  EXPECT_TRUE(Has(json, "\"mb_per_sec\": 20.000000000"));
  EXPECT_TRUE(Has(json, "\"p50_us\": 4.400000000"));
  EXPECT_TRUE(Has(json, "\"p99_us\": 5.500000000"));
  EXPECT_TRUE(Has(json, "\"p999_us\": 6.600000000"));
  EXPECT_TRUE(Has(json, "\"average_us\": 4.500000000"));
}

TEST(BenchReportTest, AFigureThatIsNotThereIsNullAndNotZero) {
  const std::string json = ResultsJson({ScanSummary()});
  EXPECT_TRUE(Has(json, "\"p50_us\": null"));
  EXPECT_TRUE(Has(json, "\"p99_us\": null"));
  EXPECT_TRUE(Has(json, "\"p999_us\": null"));
  EXPECT_TRUE(Has(json, "\"average_us\": null"));
  EXPECT_TRUE(Has(json, "\"write_amplification\": null"));
  EXPECT_TRUE(Has(json, "\"found\": 777"));
  EXPECT_TRUE(Has(json, "\"kind\": \"read\""));
  EXPECT_TRUE(Has(json, "\"source\": \"overwrite\""));
}

// ---- The medians. -------------------------------------------------------------------------------

TEST(BenchReportTest, EachFigureOfASummaryIsTheMedianOfThatFigureOverTheRuns) {
  Summary summary;
  summary.benchmark = Benchmark::kFillSeq;
  // Seconds 1, 2 and 4 give 1000, 500 and 250 operations a second and 8, 4 and 2 megabytes a
  // second; every other figure is different in each run, and different from the others'.
  summary.runs.push_back(MakeRun(1000, 8, 1, 10, 100, 1000, 5, "1.10"));
  summary.runs.push_back(MakeRun(1000, 8, 2, 30, 300, 3000, 7, "3.30"));
  summary.runs.push_back(MakeRun(1000, 8, 4, 20, 200, 2000, 6, "2.20"));

  TakeMedians(&summary);

  EXPECT_DOUBLE_EQ(summary.ops_per_sec, 500);
  EXPECT_DOUBLE_EQ(summary.mb_per_sec, 4);
  ASSERT_TRUE(summary.latency.has_value());
  EXPECT_EQ(summary.latency->p50, 20);
  EXPECT_EQ(summary.latency->p99, 200);
  EXPECT_EQ(summary.latency->p999, 2000);
  EXPECT_EQ(summary.latency->average, 6);
  EXPECT_EQ(summary.write_amplification, "2.20");
  EXPECT_DOUBLE_EQ(summary.spread_percent, 150) << "(1000 - 250) / 500";
}

TEST(BenchReportTest, ASummaryOfRunsWithNoLatenciesHasNone) {
  Summary summary = ScanSummary();
  TakeMedians(&summary);
  EXPECT_FALSE(summary.latency.has_value());
  EXPECT_FALSE(summary.write_amplification.has_value());
  EXPECT_DOUBLE_EQ(summary.spread_percent, 0) << "one run";
}

}  // namespace
}  // namespace shale
