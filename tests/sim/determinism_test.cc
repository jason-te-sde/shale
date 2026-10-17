// A simulation is a function of its seed: the same seed gives the same operations, the same faults
// and the same answers, and so the same trace. A failure that a seed reports can be replayed only
// if that holds, so it is tested apart from the invariants themselves.

#include <gtest/gtest.h>

#include <cstdint>

#include "testkit/simulator.h"

namespace shale {
namespace {

SimResult RunSeed(uint64_t seed) {
  SimOptions options;
  options.seed = seed;
  return RunSimulation(options);
}

TEST(DeterminismTest, SameSeedSameTrace) {
  for (const uint64_t seed : {uint64_t{1}, uint64_t{42}, uint64_t{8123}}) {
    const SimResult first = RunSeed(seed);
    const SimResult second = RunSeed(seed);
    ASSERT_TRUE(first.ok) << first.failure;
    ASSERT_TRUE(second.ok) << second.failure;
    EXPECT_EQ(first.trace_digest, second.trace_digest) << "seed " << seed;
    EXPECT_TRUE(first.stats == second.stats) << "seed " << seed << ": the counters differ";
    EXPECT_NE(first.trace_digest, 0u) << "seed " << seed << ": a digest that nothing went into";
  }
}

TEST(DeterminismTest, DifferentSeedsDiffer) {
  const SimResult one = RunSeed(1);
  const SimResult two = RunSeed(2);
  ASSERT_TRUE(one.ok) << one.failure;
  ASSERT_TRUE(two.ok) << two.failure;
  EXPECT_NE(one.trace_digest, two.trace_digest);
}

}  // namespace
}  // namespace shale
