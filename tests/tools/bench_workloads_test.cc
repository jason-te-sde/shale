// The workloads of shale_bench, run against engines made for the purpose. A fake engine that tells
// the truth shows that the workloads make the operations they say they do, in a fixed order, and
// account for them; an engine that lies in one way at a time shows that every way is noticed. That
// second half is the point: a benchmark that has never been shown to fail on a wrong answer would
// time an engine that is broken just as happily as one that works.

#include "bench_workloads.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "bench_engine.h"
#include "histogram.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/coding.h"

namespace shale {
namespace {

// What a fake engine does wrong. Each is one way that an engine can give a wrong answer, or fail.
enum class Flaw {
  kNone,
  kLosesKeys,             // Get says NotFound for the keys that end in 7
  kInventsKeys,           // Get returns a value for a key that was never written
  kShortValues,           // Get returns the value without its last byte
  kScanCountsExtra,       // Scan reports one entry more than it visited
  kSeekFindsNothing,      // Scan of one entry reports none
  kPutFails,              // Put returns an IOError from the third on
  kGetFails,              // Get returns an IOError for a key that is there
  kKeepsTheFirstVersion,  // Put of a key that is there does nothing, so Get serves a stale value
};

struct PutCall {
  std::string key;
  std::string value;
  bool sync = false;
};

struct OpenCall {
  std::string path;
  bool fresh = false;
};

// An engine that keeps its keys in a std::map and records every call the workloads make.
class FakeEngine final : public BenchEngine {
 public:
  explicit FakeEngine(Flaw flaw = Flaw::kNone) : flaw_(flaw) {}

  std::string Name() const override { return "fake"; }

  Status Open(const std::string& path, bool fresh) override {
    opens.push_back({path, fresh});
    if (fresh) {
      data_.clear();
    }
    return Status::OK();
  }

  Status Put(std::string_view key, std::string_view value, bool sync) override {
    Delay();
    puts.push_back({std::string(key), std::string(value), sync});
    if (flaw_ == Flaw::kPutFails && puts.size() >= 3) {
      return Status::IOError("the disk is full");
    }
    if (flaw_ == Flaw::kKeepsTheFirstVersion && data_.count(std::string(key)) != 0) {
      return Status::OK();
    }
    data_[std::string(key)] = std::string(value);
    return Status::OK();
  }

  Status Get(std::string_view key, std::string* value) override {
    Delay();
    gets.emplace_back(key);
    const auto it = data_.find(std::string(key));
    if (it == data_.end()) {
      if (flaw_ == Flaw::kInventsKeys) {
        *value = std::string(data_.empty() ? 1 : data_.begin()->second.size(), 'x');
        return Status::OK();
      }
      return Status::NotFound("no such key");
    }
    if (flaw_ == Flaw::kLosesKeys && key.back() == '7') {
      return Status::NotFound("lost");
    }
    if (flaw_ == Flaw::kGetFails) {
      return Status::IOError("the table cannot be read");
    }
    *value = it->second;
    if (flaw_ == Flaw::kShortValues && !value->empty()) {
      value->pop_back();
    }
    return Status::OK();
  }

  Status Scan(std::string_view from, size_t limit, size_t* seen) override {
    Delay();
    scans.push_back({std::string(from), limit});
    size_t visited = 0;
    for (auto it = data_.lower_bound(std::string(from)); it != data_.end(); ++it) {
      if (limit != 0 && visited == limit) {
        break;
      }
      visited++;
    }
    if (flaw_ == Flaw::kScanCountsExtra) {
      visited++;
    }
    if (flaw_ == Flaw::kSeekFindsNothing && limit == 1) {
      visited = 0;
    }
    *seen = visited;
    return Status::OK();
  }

  Status CompactAll() override {
    puts_before_compactions.push_back(puts.size());
    return Status::OK();
  }
  std::string WriteAmplification() override { return "1.50"; }
  void Close() override {}

  const std::map<std::string, std::string>& data() const { return data_; }

  std::vector<OpenCall> opens;
  std::vector<PutCall> puts;
  // How many puts there had been at each CompactAll().
  std::vector<size_t> puts_before_compactions;
  std::vector<std::string> gets;
  struct ScanCall {
    std::string from;
    size_t limit;
  };
  std::vector<ScanCall> scans;
  // How long each Put, Get and Scan takes at least, in microseconds, so that a test can tell the
  // time inside the engine from the rest of the loop.
  uint64_t call_delay_micros = 0;

 private:
  void Delay() const {
    if (call_delay_micros == 0) {
      return;
    }
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::microseconds(call_delay_micros);
    while (std::chrono::steady_clock::now() < until) {
    }
  }

  const Flaw flaw_;
  std::map<std::string, std::string> data_;
};

WorkloadOptions Options(uint64_t num = 60, uint64_t seed = 301) {
  WorkloadOptions options;
  options.db = "/fake/db";
  options.num = num;
  options.sync_num = 9;
  options.value_size = 24;
  options.seed = seed;
  return options;
}

bool IsKeyOf16Digits(const std::string& key) {
  return key.size() == 16 &&
         std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; });
}

uint64_t IndexOf(const std::string& key) { return std::stoull(key); }

// Runs a write benchmark on a fresh fake engine and returns the engine, with `*populated` and
// `*result` filled in.
FakeEngine WriteOn(Benchmark benchmark, const WorkloadOptions& options, Populated* populated,
                   RunResult* result) {
  FakeEngine engine;
  EXPECT_OK(RunWriteBenchmark(&engine, options, benchmark, populated, result));
  return engine;
}

TEST(BenchWorkloadsTest, BenchmarksHaveNamesAndAreFoundByThem) {
  for (const Benchmark benchmark : kAllBenchmarks) {
    EXPECT_EQ(FindBenchmark(BenchmarkName(benchmark)), benchmark) << BenchmarkName(benchmark);
  }
  EXPECT_EQ(FindBenchmark("nosuch"), std::nullopt);
  EXPECT_EQ(FindBenchmark(""), std::nullopt);
  EXPECT_TRUE(BenchmarkWrites(Benchmark::kFillSeq));
  EXPECT_TRUE(BenchmarkWrites(Benchmark::kFillSync));
  EXPECT_FALSE(BenchmarkWrites(Benchmark::kReadSeq));
}

// ---- The write benchmarks. ----------------------------------------------------------------------

TEST(BenchWorkloadsTest, FillSeqPutsEveryKeyInOrderUnsyncedIntoAFreshDatabase) {
  Populated populated;
  RunResult result;
  const FakeEngine engine = WriteOn(Benchmark::kFillSeq, Options(), &populated, &result);

  ASSERT_EQ(engine.opens.size(), 1u);
  EXPECT_EQ(engine.opens[0].path, "/fake/db");
  EXPECT_TRUE(engine.opens[0].fresh);
  ASSERT_EQ(engine.puts.size(), 60u);
  for (size_t i = 0; i < engine.puts.size(); i++) {
    ASSERT_TRUE(IsKeyOf16Digits(engine.puts[i].key)) << engine.puts[i].key;
    EXPECT_EQ(IndexOf(engine.puts[i].key), i);
    EXPECT_EQ(engine.puts[i].value.size(), 24u);
    EXPECT_FALSE(engine.puts[i].sync);
  }
  EXPECT_EQ(populated.written_by, Benchmark::kFillSeq);
  EXPECT_EQ(populated.entries, 60u);
  EXPECT_EQ(populated.last_present, 59);
  EXPECT_TRUE(engine.puts_before_compactions.empty()) << "only overwrite settles a load first";
}

TEST(BenchWorkloadsTest, FillRandomDrawsKeysFromTheKeySpaceSoSomeRepeat) {
  Populated populated;
  RunResult result;
  const FakeEngine engine = WriteOn(Benchmark::kFillRandom, Options(2000), &populated, &result);

  ASSERT_EQ(engine.puts.size(), 2000u);
  std::set<std::string> distinct;
  for (const PutCall& put : engine.puts) {
    ASSERT_TRUE(IsKeyOf16Digits(put.key)) << put.key;
    EXPECT_LT(IndexOf(put.key), 2000u);
    EXPECT_FALSE(put.sync);
    distinct.insert(put.key);
  }
  // 2000 draws of 2000 keys leave about 63% distinct. The bounds are far from that, and only say
  // that the keys are neither all different, as in a fill in order, nor few.
  EXPECT_LT(distinct.size(), 1900u);
  EXPECT_GT(distinct.size(), 1000u);
  EXPECT_EQ(populated.entries, distinct.size());
  EXPECT_EQ(populated.entries, engine.data().size());
  EXPECT_EQ(populated.last_present, static_cast<int64_t>(IndexOf(engine.data().rbegin()->first)));
}

TEST(BenchWorkloadsTest, FillSyncPutsSyncNumKeysAndSyncsEveryOne) {
  Populated populated;
  RunResult result;
  const FakeEngine engine = WriteOn(Benchmark::kFillSync, Options(), &populated, &result);

  ASSERT_EQ(engine.puts.size(), 9u) << "sync_num of them, not num";
  for (const PutCall& put : engine.puts) {
    EXPECT_TRUE(put.sync) << put.key;
    EXPECT_LT(IndexOf(put.key), 60u);
  }
  EXPECT_EQ(result.operations, 9u);
  EXPECT_EQ(populated.entries, engine.data().size());
}

TEST(BenchWorkloadsTest, OverwriteLoadsEveryKeyFirstAndThenOverwritesThemAtRandom) {
  Populated populated;
  RunResult result;
  const FakeEngine engine = WriteOn(Benchmark::kOverwrite, Options(), &populated, &result);

  // The load: every key once, in order. Then the same number of puts of keys that are all there.
  ASSERT_EQ(engine.puts.size(), 120u);
  for (size_t i = 0; i < 60; i++) {
    EXPECT_EQ(IndexOf(engine.puts[i].key), i);
  }
  for (size_t i = 60; i < 120; i++) {
    EXPECT_LT(IndexOf(engine.puts[i].key), 60u);
  }
  EXPECT_TRUE(std::any_of(engine.puts.begin() + 60, engine.puts.end(), [](const PutCall& put) {
    return IndexOf(put.key) != 0;
  })) << "the overwrites are of random keys";
  for (const PutCall& put : engine.puts) {
    EXPECT_FALSE(put.sync);
  }

  // Only the second 60 are the benchmark: the first were not timed, and are not counted. What the
  // load left is settled before they start, with a compaction and an open of the database again,
  // so that the engine counts the overwrites alone, and not the flush and the compaction that the
  // load was owed.
  EXPECT_EQ(result.operations, 60u);
  EXPECT_EQ(engine.puts_before_compactions, std::vector<size_t>{60});
  ASSERT_EQ(engine.opens.size(), 2u);
  EXPECT_TRUE(engine.opens[0].fresh);
  EXPECT_FALSE(engine.opens[1].fresh);
  EXPECT_EQ(populated.entries, 60u);
  EXPECT_EQ(populated.last_present, 59);

  // The overwrites put other values than the load did.
  EXPECT_NE(engine.puts[0].value, engine.puts[60].value);
}

TEST(BenchWorkloadsTest, TheSameOptionsMakeTheSameOperationsAndAnotherSeedDoesNot) {
  const auto keys_and_values = [](const WorkloadOptions& options) {
    Populated populated;
    RunResult result;
    const FakeEngine engine = WriteOn(Benchmark::kFillRandom, options, &populated, &result);
    std::vector<std::string> operations;
    for (const PutCall& put : engine.puts) {
      operations.push_back(put.key + "=" + put.value);
    }
    return operations;
  };
  EXPECT_EQ(keys_and_values(Options(500, 7)), keys_and_values(Options(500, 7)));
  EXPECT_NE(keys_and_values(Options(500, 7)), keys_and_values(Options(500, 8)));
}

TEST(BenchWorkloadsTest, TheKeysOfOneBenchmarkDoNotDependOnWhichOthersRun) {
  // fillrandom after fillseq draws what fillrandom alone does: a benchmark repeats by itself.
  Populated populated;
  RunResult result;
  const FakeEngine alone = WriteOn(Benchmark::kFillRandom, Options(), &populated, &result);

  FakeEngine after;
  ASSERT_OK(RunWriteBenchmark(&after, Options(), Benchmark::kFillSeq, &populated, &result));
  after.puts.clear();
  ASSERT_OK(RunWriteBenchmark(&after, Options(), Benchmark::kFillRandom, &populated, &result));
  ASSERT_EQ(after.puts.size(), alone.puts.size());
  for (size_t i = 0; i < alone.puts.size(); i++) {
    EXPECT_EQ(after.puts[i].key, alone.puts[i].key);
    EXPECT_EQ(after.puts[i].value, alone.puts[i].value);
  }
}

TEST(BenchWorkloadsTest, AWriteRunAccountsForItsOperationsBytesAndLatencies) {
  Populated populated;
  RunResult result;
  WriteOn(Benchmark::kFillSeq, Options(), &populated, &result);

  EXPECT_EQ(result.operations, 60u);
  EXPECT_EQ(result.bytes, 60u * (16 + 24));
  EXPECT_TRUE(result.latency.has_value());
  EXPECT_FALSE(result.found.has_value());
  EXPECT_EQ(result.write_amplification, "1.50") << "the engine's figure, as the engine gave it";
}

TEST(BenchWorkloadsTest, AFailedPutEndsTheRunWithItsStatus) {
  FakeEngine engine(Flaw::kPutFails);
  Populated populated;
  RunResult result;
  const Status s = RunWriteBenchmark(&engine, Options(), Benchmark::kFillSeq, &populated, &result);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
}

// ---- The read benchmarks. -----------------------------------------------------------------------

// A fake engine holding what `written_by` wrote, and the record of it.
struct Written {
  FakeEngine engine;
  Populated populated;

  explicit Written(Flaw flaw, Benchmark written_by = Benchmark::kFillRandom,
                   const WorkloadOptions& options = Options(300))
      : engine(flaw) {
    RunResult result;
    // The flaw is on the reads, so a write on a fake that fails puts would fail the setup.
    EXPECT_OK(RunWriteBenchmark(&engine, options, written_by, &populated, &result));
  }
};

Status Read(Written* written, Benchmark benchmark, RunResult* result,
            const WorkloadOptions& options = Options(300)) {
  return RunReadBenchmark(&written->engine, options, benchmark, written->populated, result);
}

TEST(BenchWorkloadsTest, ReadRandomFindsExactlyTheKeysThatWereWritten) {
  Written written(Flaw::kNone);
  RunResult result;
  ASSERT_OK(Read(&written, Benchmark::kReadRandom, &result));

  EXPECT_EQ(result.operations, 300u);
  ASSERT_EQ(written.engine.gets.size(), 300u);
  uint64_t hits = 0;
  for (const std::string& key : written.engine.gets) {
    ASSERT_TRUE(IsKeyOf16Digits(key)) << key;
    EXPECT_LT(IndexOf(key), 300u);
    hits += written.engine.data().count(key);
  }
  EXPECT_EQ(result.found, hits);
  EXPECT_GT(hits, 0u);
  EXPECT_LT(hits, 300u) << "after fillrandom, some of the keys are not there";
  EXPECT_EQ(result.bytes, hits * (16 + 24)) << "a miss moves no entry";
  ASSERT_TRUE(result.latency.has_value());
  EXPECT_FALSE(result.write_amplification.has_value());
}

TEST(BenchWorkloadsTest, ReadMissingAsksOnlyForKeysThatAreNotThere) {
  Written written(Flaw::kNone);
  RunResult result;
  ASSERT_OK(Read(&written, Benchmark::kReadMissing, &result));

  ASSERT_EQ(written.engine.gets.size(), 300u);
  for (const std::string& key : written.engine.gets) {
    ASSERT_EQ(key.size(), 17u) << key;
    EXPECT_EQ(key.back(), '.');
    EXPECT_TRUE(IsKeyOf16Digits(key.substr(0, 16)));
    EXPECT_EQ(written.engine.data().count(key), 0u);
  }
  EXPECT_EQ(result.found, 0u);
  EXPECT_EQ(result.bytes, 0u);
}

TEST(BenchWorkloadsTest, ReadSeqScansTheWholeDatabaseOnceAndHasNoLatency) {
  Written written(Flaw::kNone);
  RunResult result;
  ASSERT_OK(Read(&written, Benchmark::kReadSeq, &result));

  ASSERT_EQ(written.engine.scans.size(), 1u);
  EXPECT_EQ(written.engine.scans[0].from, "");
  EXPECT_EQ(written.engine.scans[0].limit, 0u) << "to the end";
  EXPECT_EQ(result.operations, written.populated.entries);
  EXPECT_EQ(result.found, written.populated.entries);
  EXPECT_EQ(result.bytes, written.populated.entries * (16 + 24));
  EXPECT_FALSE(result.latency.has_value()) << "a scan is one call, so it has no latencies";
}

TEST(BenchWorkloadsTest, SeekRandomSeeksToAKeyAndReadsOneEntry) {
  Written written(Flaw::kNone);
  RunResult result;
  ASSERT_OK(Read(&written, Benchmark::kSeekRandom, &result));

  EXPECT_EQ(result.operations, 300u);
  ASSERT_EQ(written.engine.scans.size(), 300u);
  uint64_t landed = 0;
  for (const auto& scan : written.engine.scans) {
    EXPECT_EQ(scan.limit, 1u);
    ASSERT_TRUE(IsKeyOf16Digits(scan.from)) << scan.from;
    if (static_cast<int64_t>(IndexOf(scan.from)) <= written.populated.last_present) {
      landed++;
    }
  }
  EXPECT_EQ(result.found, landed);
  EXPECT_EQ(result.bytes, landed * (16 + 24));
  EXPECT_TRUE(result.latency.has_value());
}

TEST(BenchWorkloadsTest, ReadsRepeatTheSameKeysEveryRun) {
  Written written(Flaw::kNone);
  RunResult result;
  ASSERT_OK(Read(&written, Benchmark::kReadRandom, &result));
  const std::vector<std::string> first = written.engine.gets;
  written.engine.gets.clear();
  ASSERT_OK(Read(&written, Benchmark::kReadRandom, &result));
  EXPECT_EQ(written.engine.gets, first);
}

TEST(BenchWorkloadsTest, ReadsOfASparseDatabaseFollowWhatItHolds) {
  // fillsync leaves a few of the 300 keys, so most gets miss, and every answer is still right.
  Written written(Flaw::kNone, Benchmark::kFillSync);
  RunResult result;
  ASSERT_OK(Read(&written, Benchmark::kReadRandom, &result));
  EXPECT_LT(*result.found, 50u);
  ASSERT_OK(Read(&written, Benchmark::kSeekRandom, &result));
  ASSERT_OK(Read(&written, Benchmark::kReadSeq, &result));
  EXPECT_EQ(result.operations, written.populated.entries);
}

// ---- A wrong answer is a failure that names what was wrong. -------------------------------------

bool Mentions(const Status& s, const std::string& text) {
  return s.ToString().find(text) != std::string::npos;
}

TEST(BenchWorkloadsTest, AKeyThatWasWrittenAndIsNotFoundIsCaught) {
  Written written(Flaw::kLosesKeys, Benchmark::kFillSeq);
  RunResult result;
  const Status s = Read(&written, Benchmark::kReadRandom, &result);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Mentions(s, "readrandom")) << s.ToString();
  EXPECT_TRUE(Mentions(s, "was written and is not found")) << s.ToString();
}

TEST(BenchWorkloadsTest, AKeyThatWasNeverWrittenAndIsFoundIsCaughtByReadRandom) {
  Written written(Flaw::kInventsKeys, Benchmark::kFillRandom);
  RunResult result;
  const Status s = Read(&written, Benchmark::kReadRandom, &result);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Mentions(s, "was found and was never written")) << s.ToString();
}

TEST(BenchWorkloadsTest, AKeyThatWasNeverWrittenAndIsFoundIsCaughtByReadMissing) {
  Written written(Flaw::kInventsKeys, Benchmark::kFillSeq);
  RunResult result;
  const Status s = Read(&written, Benchmark::kReadMissing, &result);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Mentions(s, "readmissing")) << s.ToString();
  // The run stops at the first answer that is wrong, so the key it asked for last is the one named.
  ASSERT_FALSE(written.engine.gets.empty());
  EXPECT_EQ(written.engine.gets.back().size(), 17u);
  EXPECT_TRUE(Mentions(s, "key " + written.engine.gets.back() + " was found and was never written"))
      << s.ToString();
}

TEST(BenchWorkloadsTest, AValueOfTheWrongSizeIsCaught) {
  Written written(Flaw::kShortValues, Benchmark::kFillSeq);
  RunResult result;
  const Status s = Read(&written, Benchmark::kReadRandom, &result);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Mentions(s, "23 bytes, not 24")) << s.ToString();
}

TEST(BenchWorkloadsTest, AScanThatVisitsTheWrongNumberOfEntriesIsCaught) {
  Written written(Flaw::kScanCountsExtra, Benchmark::kFillSeq);
  RunResult result;
  const Status s = Read(&written, Benchmark::kReadSeq, &result);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Mentions(s, "readseq")) << s.ToString();
  EXPECT_TRUE(Mentions(s, "visited 301 entries")) << s.ToString();
}

TEST(BenchWorkloadsTest, ASeekThatLandsWhereItShouldNotIsCaught) {
  Written extra(Flaw::kScanCountsExtra, Benchmark::kFillRandom);
  RunResult result;
  Status s = Read(&extra, Benchmark::kSeekRandom, &result);
  EXPECT_TRUE(s.IsCorruption()) << "a seek that reports two entries for a limit of one: "
                                << s.ToString();

  Written nothing(Flaw::kSeekFindsNothing, Benchmark::kFillSeq);
  s = Read(&nothing, Benchmark::kSeekRandom, &result);
  EXPECT_TRUE(s.IsCorruption()) << "a seek that finds nothing where a key follows: "
                                << s.ToString();
  EXPECT_TRUE(Mentions(s, "seekrandom")) << s.ToString();
}

TEST(BenchWorkloadsTest, AnErrorOfTheEngineIsReturnedAsItIsAndNotTakenForAnAnswer) {
  Written written(Flaw::kGetFails, Benchmark::kFillSeq);
  RunResult result;
  const Status s = Read(&written, Benchmark::kReadRandom, &result);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
}

// ---- What a run says about itself. --------------------------------------------------------------

TEST(BenchWorkloadsTest, TheRatesAreTheOperationsAndTheBytesOverTheTimeWithAMegabyteOf2To20) {
  RunResult result;
  result.operations = 3000;
  result.bytes = 6 * 1048576;
  result.seconds = 2.0;
  EXPECT_DOUBLE_EQ(result.OpsPerSecond(), 1500.0);
  EXPECT_DOUBLE_EQ(result.MbPerSecond(), 3.0);
}

TEST(BenchWorkloadsTest, TheLatenciesAreTheMedianTheNinetyNinthAndTheNinetyNinePointNinth) {
  // A thousand samples, so that the 99th percentile (the 990th), the 99.9th (the 999th) and the
  // percentiles near them are all different samples.
  Histogram histogram;
  for (int micros = 1; micros <= 1000; micros++) {
    histogram.Add(micros);
  }
  const Latency latency = SummarizeLatencies(histogram);
  EXPECT_EQ(latency.p50, 500);
  EXPECT_EQ(latency.p99, 990);
  EXPECT_EQ(latency.p999, 999);
  EXPECT_DOUBLE_EQ(latency.average, 500.5);
}

TEST(BenchWorkloadsTest, TheTimeInsideTheEngineIsTheTimeOfItsCallsAndNoMore) {
  constexpr uint64_t kDelayMicros = 200;
  FakeEngine engine;
  engine.call_delay_micros = kDelayMicros;
  Populated populated;
  RunResult result;
  ASSERT_OK(RunWriteBenchmark(&engine, Options(), Benchmark::kFillSeq, &populated, &result));
  // Sixty puts of 200 us each. A call takes at least its spin, so 200 us a call is the least the
  // sum should be, and the tenth of slack is only for rounding. Once, in a GCC build, this test
  // came out 0.3% short of it (0.05984 s against 0.06 s) in 28 of 40 repeats, and has not in 160
  // more: that failure was seen once and is unexplained. The bound is still worth having: it goes
  // red when engine_seconds is zero, as the identity below goes red when it is the whole loop.
  EXPECT_GE(result.engine_seconds, 0.9 * 60 * kDelayMicros * 1e-6);
  EXPECT_LE(result.engine_seconds, result.seconds * (1 + 1e-9));
  // It is the sum of the latencies, which the loop's own work is not part of.
  ASSERT_TRUE(result.latency.has_value());
  EXPECT_NEAR(result.engine_seconds, result.latency->average * 60 * 1e-6, 1e-9);

  for (const Benchmark benchmark :
       {Benchmark::kReadRandom, Benchmark::kReadMissing, Benchmark::kSeekRandom}) {
    Written written(Flaw::kNone);
    written.engine.call_delay_micros = kDelayMicros;
    ASSERT_OK(Read(&written, benchmark, &result));
    EXPECT_GE(result.engine_seconds, 0.9 * 300 * kDelayMicros * 1e-6) << BenchmarkName(benchmark);
    EXPECT_LE(result.engine_seconds, result.seconds * (1 + 1e-9)) << BenchmarkName(benchmark);
    ASSERT_TRUE(result.latency.has_value());
    EXPECT_NEAR(result.engine_seconds, result.latency->average * 300 * 1e-6, 1e-9)
        << BenchmarkName(benchmark);
  }

  Written written(Flaw::kNone);
  ASSERT_OK(Read(&written, Benchmark::kReadSeq, &result));
  EXPECT_EQ(result.engine_seconds, result.seconds) << "a scan is one call, and the whole loop";
}

// ---- Every put says which put it is. ------------------------------------------------------------

uint64_t StampOf(const PutCall& put) { return DecodeFixed64(put.value.data()); }

TEST(BenchWorkloadsTest, EveryPutCarriesItsNumberInItsFirstEightBytes) {
  Populated populated;
  RunResult result;
  const FakeEngine engine = WriteOn(Benchmark::kOverwrite, Options(), &populated, &result);
  ASSERT_EQ(engine.puts.size(), 120u);
  for (size_t i = 0; i < engine.puts.size(); i++) {
    // The load counts, so that an overwrite is told from the first value of its key.
    EXPECT_EQ(StampOf(engine.puts[i]), i + 1) << "put " << i;
  }
}

TEST(BenchWorkloadsTest, AValueShorterThanTheNumberCarriesTheLowBytesOfIt) {
  WorkloadOptions options = Options(200);
  options.value_size = 3;
  Populated populated;
  RunResult result;
  FakeEngine engine;
  ASSERT_OK(RunWriteBenchmark(&engine, options, Benchmark::kOverwrite, &populated, &result));
  ASSERT_EQ(engine.puts.size(), 400u);
  EXPECT_EQ(engine.puts[0].value, std::string("\x01\x00\x00", 3));
  EXPECT_EQ(engine.puts[299].value, std::string("\x2c\x01\x00", 3)) << "put 300 is 0x012c";

  // What is checked is the bytes there are, and an engine that tells the truth passes.
  for (const Benchmark benchmark : {Benchmark::kReadRandom, Benchmark::kReadSeq}) {
    ASSERT_OK(RunReadBenchmark(&engine, options, benchmark, populated, &result));
  }
}

TEST(BenchWorkloadsTest, AValueThatIsNotTheOneLastPutToItsKeyIsCaught) {
  // An engine that keeps the first value of a key serves, after an overwrite, a value of the right
  // size that is a stale one.
  Written written(Flaw::kKeepsTheFirstVersion, Benchmark::kOverwrite);
  RunResult result;
  const Status s = Read(&written, Benchmark::kReadRandom, &result);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(Mentions(s, "readrandom")) << s.ToString();
  EXPECT_TRUE(Mentions(s, "is not the value last put to it")) << s.ToString();
}

}  // namespace
}  // namespace shale
