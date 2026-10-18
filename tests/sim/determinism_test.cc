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

// The digests of three seeds as they were when the simulator was finished, taken with AppleClang
// and again with GCC 16, which agree. A run depends on the engine only through what it does, and
// not through how a compiler or a standard library lays things out, so these are the digests on
// every toolchain, and CI proves it on GCC 13 and 14, Clang 18 and AppleClang. A digest that
// changes is a change of what a run does: of the simulator, of the engine's behaviour, or of a
// toolchain's, and the first two are meant when they come with a commit that says so, and are
// accepted by pinning the new values. The third is a bug of the replay that this test is for.
struct Golden {
  uint64_t seed;
  uint64_t digest;
};
constexpr Golden kGolden[] = {
    {1, 0xa98a7939b26de369ULL},
    {42, 0x9935578d274bff32ULL},
    {8123, 0x9dc8bcdeca19c9eeULL},
};

TEST(DeterminismTest, TracesAreTheSameOnEveryToolchain) {
  for (const Golden& golden : kGolden) {
    const SimResult result = RunSeed(golden.seed);
    ASSERT_TRUE(result.ok) << result.failure;
    EXPECT_EQ(result.trace_digest, golden.digest) << "seed " << golden.seed;
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
