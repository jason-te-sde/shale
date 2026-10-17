// The deterministic simulator, run over many seeds, and the tests that show it can fail.
//
// A simulator that passes on a correct database proves little until it is also seen to fail on a
// wrong one: a driver that never checks, or never provokes, passes every seed. So beside the runs
// there are the doubles, real databases behind a wrapper that is told to misbehave in one named
// way, and each must make the run fail with the invariant that is meant to catch it.
//
// Environment:
//   SHALE_SIM_SEEDS  how many seeds the main test runs, from 0 (default 50)
//   SHALE_SIM_STEPS  how many steps each of them takes (default 2000)
//   SHALE_SIM_KEYS   how many different keys each of them uses (default 200)
//   SHALE_SIM_SEED   run that one seed and nothing else, for replaying a failure
//   SHALE_SIM_VERBOSE  print each seed before it runs, which names the seed of a run that aborts,
//                    and its trace digest after, which is the same on every run of a seed on one
//                    toolchain

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "shale/db.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/simulator.h"

namespace shale {
namespace {

constexpr uint64_t kDefaultSeeds = 50;
constexpr int kDefaultSteps = 2000;
constexpr int kDefaultKeys = 200;

// The value of the environment variable `name` as a number, or `fallback` if it is not set or is
// not a number.
uint64_t EnvNumber(const char* name, uint64_t fallback) {
  const char* text = std::getenv(name);
  if (text == nullptr || *text == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  return *end == '\0' ? static_cast<uint64_t>(value) : fallback;
}

bool Verbose() { return std::getenv("SHALE_SIM_VERBOSE") != nullptr; }

SimResult RunSeed(const SimOptions& options) {
  if (Verbose()) {
    std::printf("[sim] seed %llu\n", static_cast<unsigned long long>(options.seed));
    std::fflush(stdout);
  }
  SimResult result = RunSimulation(options);
  if (Verbose()) {
    std::printf("[sim] seed %llu: %s, trace digest %016llx\n",
                static_cast<unsigned long long>(options.seed), result.ok ? "ok" : "FAILED",
                static_cast<unsigned long long>(result.trace_digest));
  }
  return result;
}

// Adds every counter of `part` to `*total`.
void Add(SimStats* total, const SimStats& part) {
  total->steps += part.steps;
  total->writes += part.writes;
  total->sync_writes += part.sync_writes;
  total->power_losses += part.power_losses;
  total->process_crashes += part.process_crashes;
  total->flushes += part.flushes;
  total->compactions += part.compactions;
  total->trivial_moves += part.trivial_moves;
  total->tombstones_dropped += part.tombstones_dropped;
  total->snapshots_across_compaction += part.snapshots_across_compaction;
  total->iterators_across_compaction += part.iterators_across_compaction;
  total->injected_errors += part.injected_errors;
  total->injected_corruptions += part.injected_corruptions;
  total->reopens += part.reopens;
  total->write_failures += part.write_failures;
  total->read_failures += part.read_failures;
  total->open_failures += part.open_failures;
  total->orphan_checks += part.orphan_checks;
  total->deepest_level = std::max(total->deepest_level, part.deepest_level);
}

// The least that the totals of the 50 seeds of the default run (2000 steps each, 200 keys) may be,
// each as half of what the simulator observed over those seeds when it was finished, in a debug
// build with AppleClang (a release build with GCC 16 observed within a percent of the same):
//
//                                         observed    floor
//   power losses                              1155      577
//   process crashes                           1248      624
//   flushes                                   5421     2710
//   compactions                               3625     1812
//   tombstones dropped                       14580     7290
//   snapshots held across a compaction         695      347
//   iterators held across a compaction         888      444
//   injected errors                            728      364
//   injected corruptions                      1100      550
//
// A simulator that stops provoking something, because an operation was reweighted or a
// precondition began to skip it, falls under its floor and fails here, where a run that merely
// passes would not say so.
struct Floors {
  uint64_t power_losses;
  uint64_t process_crashes;
  uint64_t flushes;
  uint64_t compactions;
  uint64_t tombstones_dropped;
  uint64_t snapshots_across_compaction;
  uint64_t iterators_across_compaction;
  uint64_t injected_errors;
  uint64_t injected_corruptions;
};
constexpr Floors kFloors = {577, 624, 2710, 1812, 7290, 347, 444, 364, 550};

// The floors are those of kDefaultSeeds seeds of kDefaultSteps steps, and scale with the number of
// seeds, which is what a longer run is for. A run with other steps, or with too few seeds for the
// totals to be steady, is not held to them.
void ExpectFierce(const SimStats& total, uint64_t seeds, int steps, int keys) {
  std::printf(
      "[sim] totals over %llu seeds of %d steps: steps=%llu writes=%llu sync_writes=%llu "
      "power_losses=%llu process_crashes=%llu reopens=%llu flushes=%llu compactions=%llu "
      "trivial_moves=%llu tombstones_dropped=%llu snapshots_across_compaction=%llu "
      "iterators_across_compaction=%llu injected_errors=%llu injected_corruptions=%llu "
      "write_failures=%llu read_failures=%llu open_failures=%llu orphan_checks=%llu "
      "deepest_level=%llu\n",
      static_cast<unsigned long long>(seeds), steps, static_cast<unsigned long long>(total.steps),
      static_cast<unsigned long long>(total.writes),
      static_cast<unsigned long long>(total.sync_writes),
      static_cast<unsigned long long>(total.power_losses),
      static_cast<unsigned long long>(total.process_crashes),
      static_cast<unsigned long long>(total.reopens),
      static_cast<unsigned long long>(total.flushes),
      static_cast<unsigned long long>(total.compactions),
      static_cast<unsigned long long>(total.trivial_moves),
      static_cast<unsigned long long>(total.tombstones_dropped),
      static_cast<unsigned long long>(total.snapshots_across_compaction),
      static_cast<unsigned long long>(total.iterators_across_compaction),
      static_cast<unsigned long long>(total.injected_errors),
      static_cast<unsigned long long>(total.injected_corruptions),
      static_cast<unsigned long long>(total.write_failures),
      static_cast<unsigned long long>(total.read_failures),
      static_cast<unsigned long long>(total.open_failures),
      static_cast<unsigned long long>(total.orphan_checks),
      static_cast<unsigned long long>(total.deepest_level));
  if (steps != kDefaultSteps || keys != kDefaultKeys || seeds < 20) {
    return;
  }
  // The simulator also has to have done what no floor names: stopped writes on a failure, met
  // faults in reads and in opens, checked for orphans, reopened, and gone below the first levels.
  EXPECT_GT(total.reopens, 0u);
  EXPECT_GT(total.write_failures, 0u) << "no failure ever stopped the database's writes";
  EXPECT_GT(total.read_failures, 0u) << "no read ever failed for a fault";
  EXPECT_GT(total.open_failures, 0u) << "no open ever failed for a fault";
  EXPECT_GT(total.orphan_checks, 0u) << "the directory was never checked for orphan tables";
  EXPECT_GE(total.deepest_level, 2u) << "no table ever reached level 2";
  const auto floor_of = [seeds](uint64_t per_default_run) {
    return per_default_run * seeds / kDefaultSeeds;
  };
  EXPECT_GE(total.power_losses, floor_of(kFloors.power_losses)) << "power losses";
  EXPECT_GE(total.process_crashes, floor_of(kFloors.process_crashes)) << "process crashes";
  EXPECT_GE(total.flushes, floor_of(kFloors.flushes)) << "flushes";
  EXPECT_GE(total.compactions, floor_of(kFloors.compactions)) << "compactions";
  EXPECT_GE(total.tombstones_dropped, floor_of(kFloors.tombstones_dropped)) << "tombstones dropped";
  EXPECT_GE(total.snapshots_across_compaction, floor_of(kFloors.snapshots_across_compaction))
      << "snapshots held across a compaction";
  EXPECT_GE(total.iterators_across_compaction, floor_of(kFloors.iterators_across_compaction))
      << "iterators held across a compaction";
  EXPECT_GE(total.injected_errors, floor_of(kFloors.injected_errors)) << "injected errors";
  EXPECT_GE(total.injected_corruptions, floor_of(kFloors.injected_corruptions))
      << "injected corruptions";
}

TEST(SimulationTest, RandomSeedsHoldAllInvariants) {
  const int steps = static_cast<int>(EnvNumber("SHALE_SIM_STEPS", kDefaultSteps));
  const int keys = static_cast<int>(EnvNumber("SHALE_SIM_KEYS", kDefaultKeys));
  if (std::getenv("SHALE_SIM_SEED") != nullptr) {
    SimOptions options;
    options.seed = EnvNumber("SHALE_SIM_SEED", 0);
    options.steps = steps;
    options.key_space = keys;
    const SimResult result = RunSeed(options);
    ASSERT_TRUE(result.ok) << result.failure;
    return;
  }
  const uint64_t seeds = EnvNumber("SHALE_SIM_SEEDS", kDefaultSeeds);
  SimStats total;
  for (uint64_t seed = 0; seed < seeds; seed++) {
    SimOptions options;
    options.seed = seed;
    options.steps = steps;
    options.key_space = keys;
    const SimResult result = RunSeed(options);
    ASSERT_TRUE(result.ok) << result.failure;
    Add(&total, result.stats);
  }
  ExpectFierce(total, seeds, steps, keys);
}

// The seeds that once failed, one per line after the first line, which is a comment. Whatever
// follows the number on a line is a comment too.
std::vector<uint64_t> RegressionSeeds() {
  std::ifstream in(SHALE_REGRESSION_SEEDS_PATH);
  EXPECT_TRUE(in.is_open()) << "cannot open " << SHALE_REGRESSION_SEEDS_PATH;
  std::vector<uint64_t> seeds;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    seeds.push_back(std::strtoull(line.c_str(), nullptr, 10));
  }
  return seeds;
}

TEST(SimulationTest, RegressionSeeds) {
  for (const uint64_t seed : RegressionSeeds()) {
    SimOptions options;
    options.seed = seed;
    const SimResult result = RunSeed(options);
    EXPECT_TRUE(result.ok) << "regression seed " << seed << ": " << result.failure;
  }
}

TEST(SimulationTest, CrashFreeRunsToo) {
  for (uint64_t seed = 0; seed < 10; seed++) {
    SimOptions options;
    options.seed = seed;
    options.crashes = false;
    options.io_errors = false;
    options.read_corruption = false;
    const SimResult result = RunSeed(options);
    ASSERT_TRUE(result.ok) << result.failure;
    EXPECT_EQ(result.stats.steps, static_cast<uint64_t>(options.steps));
    EXPECT_EQ(result.stats.power_losses, 0u);
    EXPECT_EQ(result.stats.process_crashes, 0u);
    EXPECT_EQ(result.stats.injected_errors, 0u);
    EXPECT_EQ(result.stats.injected_corruptions, 0u);
    EXPECT_EQ(result.stats.reopens, 0u) << "nothing reopens a database that nothing disturbs";
    EXPECT_GT(result.stats.flushes, 0u) << "a run with no faults must still flush";
    EXPECT_GT(result.stats.compactions, 0u) << "and compact";
  }
}

TEST(SimulationTest, ALargerKeySpaceGoesDeeper) {
  // With the sizes the simulator gives the engine, 200 keys fit in the first three levels and the
  // deeper ones are never reached. A larger key space does reach them, with crashes and faults on
  // and, with none, over a database that nothing interrupts.
  for (const bool disturbed : {true, false}) {
    SimOptions options;
    options.seed = 7;
    options.key_space = 1500;
    options.steps = disturbed ? 3000 : 2500;
    options.crashes = disturbed;
    options.io_errors = disturbed;
    options.read_corruption = disturbed;
    const SimResult result = RunSeed(options);
    ASSERT_TRUE(result.ok) << result.failure;
    EXPECT_GE(result.stats.deepest_level, 3u) << (disturbed ? "disturbed" : "undisturbed");
  }
}

// ---------------------------------------------------------------------------------------------
// The doubles.

// What a double does wrong. It is shared by every incarnation of the database in one run, since the
// simulator reopens, and a count of calls has to go on across it.
struct Misbehavior {
  // Every Get from the n-th on fails with an error that no fault explains.
  std::optional<uint64_t> fail_gets_from;
  // Every n-th Write from the n-th on is acknowledged and not applied. A single lost write may be
  // one nothing can see, a delete of a key that is not there or a put that a later write covers,
  // so a double that is to be caught drops them steadily.
  std::optional<uint64_t> drop_every_writes;
  // Every write is made without a sync, whatever it was asked.
  bool ignore_sync = false;
  // A Write is acknowledged and not applied once the database has stopped writes: its error is
  // not sticky.
  bool accept_write_after_error = false;

  uint64_t gets = 0;
  uint64_t writes = 0;
};

// A real database that does what `misbehavior` says it does wrong, and nothing else.
class DoubleDB final : public DB {
 public:
  DoubleDB(std::unique_ptr<DB> real, std::shared_ptr<Misbehavior> misbehavior)
      : real_(std::move(real)), m_(std::move(misbehavior)) {}

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value) override {
    WriteBatch batch;
    batch.Put(key, value);
    return Write(options, &batch);
  }

  Status Delete(const WriteOptions& options, std::string_view key) override {
    WriteBatch batch;
    batch.Delete(key);
    return Write(options, &batch);
  }

  Status Write(const WriteOptions& options, WriteBatch* updates) override {
    m_->writes++;
    if (m_->accept_write_after_error) {
      std::string error;
      if (real_->GetProperty("shale.background-error", &error) && error != "OK") {
        return Status::OK();
      }
    }
    if (m_->drop_every_writes.has_value() && m_->writes % *m_->drop_every_writes == 0) {
      return Status::OK();
    }
    WriteOptions actual = options;
    if (m_->ignore_sync) {
      actual.sync = false;
    }
    return real_->Write(actual, updates);
  }

  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override {
    m_->gets++;
    if (m_->fail_gets_from.has_value() && m_->gets >= *m_->fail_gets_from) {
      return Status::IOError("double: a read that fails for no reason");
    }
    return real_->Get(options, key, value);
  }

  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override {
    return real_->NewIterator(options);
  }
  const Snapshot* GetSnapshot() override { return real_->GetSnapshot(); }
  void ReleaseSnapshot(const Snapshot* snapshot) override { real_->ReleaseSnapshot(snapshot); }
  Status Flush() override { return real_->Flush(); }
  Status CompactRange(std::optional<std::string_view> begin,
                      std::optional<std::string_view> end) override {
    return real_->CompactRange(begin, end);
  }
  bool GetProperty(std::string_view property, std::string* value) override {
    return real_->GetProperty(property, value);
  }

 private:
  std::unique_ptr<DB> real_;
  std::shared_ptr<Misbehavior> m_;
};

SimOptions OptionsWithDouble(uint64_t seed, const std::shared_ptr<Misbehavior>& misbehavior) {
  SimOptions options;
  options.seed = seed;
  options.wrap_db = [misbehavior](std::unique_ptr<DB> real) -> std::unique_ptr<DB> {
    return std::make_unique<DoubleDB>(std::move(real), misbehavior);
  };
  return options;
}

testing::AssertionResult Contains(const std::string& text, const std::string& part) {
  if (text.find(part) != std::string::npos) {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure() << "\"" << text << "\" does not contain \"" << part << "\"";
}

TEST(SimulationDoublesTest, ADoubleThatMisbehavesNeverIsHarmless) {
  // The control: if the wrapper alone made runs fail, every test below would pass for no reason.
  for (uint64_t seed = 0; seed < 3; seed++) {
    const SimResult result = RunSeed(OptionsWithDouble(seed, std::make_shared<Misbehavior>()));
    EXPECT_TRUE(result.ok) << result.failure;
  }
}

TEST(SimulationDoublesTest, AReadErrorWithNoFaultArmedFailsTheRun) {
  // Faults are on in these runs, and still the error, which no fault caused, is not allowed: the
  // allowance is for a read that a fault that fired in that very call explains.
  for (uint64_t seed = 0; seed < 3; seed++) {
    auto misbehavior = std::make_shared<Misbehavior>();
    misbehavior->fail_gets_from = 200;
    const SimResult result = RunSeed(OptionsWithDouble(seed, misbehavior));
    ASSERT_FALSE(result.ok) << "seed " << seed << " did not notice a database that fails reads";
    EXPECT_TRUE(Contains(result.failure, "I5: unexpected read error")) << result.failure;
    EXPECT_TRUE(Contains(result.failure, "seed=" + std::to_string(seed))) << result.failure;
    EXPECT_TRUE(Contains(result.failure, "last 50 operations:")) << result.failure;
  }
}

TEST(SimulationDoublesTest, AFailureReplaysFromItsSeed) {
  // What a report says is only worth having if the seed in it brings the same failure back.
  const auto run = [] {
    auto misbehavior = std::make_shared<Misbehavior>();
    misbehavior->fail_gets_from = 200;
    return RunSeed(OptionsWithDouble(5, misbehavior));
  };
  const SimResult first = run();
  const SimResult second = run();
  ASSERT_FALSE(first.ok);
  EXPECT_EQ(first.failure, second.failure);
  EXPECT_EQ(first.trace_digest, second.trace_digest);
}

TEST(SimulationDoublesTest, ADroppedWriteFailsTheRun) {
  for (uint64_t seed = 0; seed < 5; seed++) {
    auto misbehavior = std::make_shared<Misbehavior>();
    misbehavior->drop_every_writes = 7;
    const SimResult result = RunSeed(OptionsWithDouble(seed, misbehavior));
    ASSERT_FALSE(result.ok) << "seed " << seed << " did not notice a write that was lost";
    const bool by_reads = result.failure.find("I1: ") != std::string::npos;
    const bool by_recovery = result.failure.find("I4: ") != std::string::npos;
    EXPECT_TRUE(by_reads || by_recovery) << result.failure;
  }
}

TEST(SimulationDoublesTest, ASyncThatDoesNotSyncFailsTheRunAtAPowerLoss) {
  // The writes are acknowledged as durable and are not, which only a power loss shows. It is a
  // matter of the seed whether the loss takes the bytes, so some seed must, not each.
  bool noticed = false;
  for (uint64_t seed = 0; seed < 20 && !noticed; seed++) {
    auto misbehavior = std::make_shared<Misbehavior>();
    misbehavior->ignore_sync = true;
    const SimResult result = RunSeed(OptionsWithDouble(seed, misbehavior));
    if (!result.ok) {
      noticed = true;
      // The database comes back with a sequence number below that of a write it acknowledged as
      // durable, which I2 sees first, or without the write, which I4 sees.
      const bool by_sequence = result.failure.find("I2: last_sequence went back") == 0;
      const bool by_recovery = result.failure.find("I4: ") == 0;
      EXPECT_TRUE(by_sequence || by_recovery) << result.failure;
    }
  }
  EXPECT_TRUE(noticed) << "no seed of twenty noticed that acknowledged synced writes were lost";
}

TEST(SimulationDoublesTest, AWriteAcceptedAfterAnErrorFailsTheRun) {
  bool noticed = false;
  for (uint64_t seed = 0; seed < 50 && !noticed; seed++) {
    auto misbehavior = std::make_shared<Misbehavior>();
    misbehavior->accept_write_after_error = true;
    const SimResult result = RunSeed(OptionsWithDouble(seed, misbehavior));
    if (!result.ok) {
      noticed = true;
      EXPECT_TRUE(Contains(result.failure, "I6: ")) << result.failure;
    }
  }
  EXPECT_TRUE(noticed) << "no seed of fifty had a write fail, or none noticed the next one succeed";
}

}  // namespace
}  // namespace shale
