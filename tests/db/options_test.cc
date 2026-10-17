// ValidateOptions() stands between what a user can write in an Options and the parts of the engine
// that abort on a value they cannot work with: the picker on a trigger below one, the table
// builder on a block size of nothing. Every bound is checked on both sides, and the error has to
// name the field.

#include "shale/options.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "shale/status.h"

namespace shale {
namespace {

TEST(OptionsTest, DefaultsAreValid) { EXPECT_TRUE(ValidateOptions(Options()).ok()); }

TEST(OptionsTest, DefaultsAreTheDocumentedOnes) {
  const Options o;
  EXPECT_EQ(o.env, nullptr);
  EXPECT_EQ(o.scheduler, nullptr);
  EXPECT_FALSE(o.create_if_missing);
  EXPECT_FALSE(o.error_if_exists);
  EXPECT_TRUE(o.paranoid_checks);
  EXPECT_EQ(o.write_buffer_size, size_t{4} << 20);
  EXPECT_EQ(o.block_size, size_t{4096});
  EXPECT_EQ(o.block_restart_interval, 16);
  EXPECT_EQ(o.bloom_bits_per_key, 10);
  EXPECT_EQ(o.block_cache_capacity, size_t{8} << 20);
  EXPECT_EQ(o.max_open_files, 1000);
  EXPECT_EQ(o.l0_compaction_trigger, 4);
  EXPECT_EQ(o.l0_slowdown_trigger, 8);
  EXPECT_EQ(o.l0_stop_trigger, 12);
  EXPECT_EQ(o.max_bytes_for_level_base, uint64_t{10} << 20);
  EXPECT_EQ(o.level_size_multiplier, 10);
  EXPECT_EQ(o.target_file_size, uint64_t{2} << 20);
  EXPECT_TRUE(ReadOptions().verify_checksums);
  EXPECT_TRUE(ReadOptions().fill_cache);
  EXPECT_EQ(ReadOptions().snapshot, nullptr);
  EXPECT_TRUE(WriteOptions().sync);
}

TEST(OptionsTest, TheLimitsOnKeysAndValues) {
  EXPECT_EQ(kMaxKeySize, size_t{65536});
  EXPECT_EQ(kMaxValueSize, size_t{67108864});
}

// One bound: how to set the field to a value, the field's name as the error must give it, and the
// values on each side of the bound.
struct Bound {
  std::string field;
  std::function<void(Options*, int64_t)> set;
  int64_t last_invalid;
  int64_t first_valid;
};

std::vector<Bound> Bounds() {
  return {
      {"write_buffer_size", [](Options* o, int64_t v) { o->write_buffer_size = size_t(v); }, 16383,
       16384},
      {"block_size", [](Options* o, int64_t v) { o->block_size = size_t(v); }, 255, 256},
      {"block_size", [](Options* o, int64_t v) { o->block_size = size_t(v); }, 4194305, 4194304},
      {"block_restart_interval", [](Options* o, int64_t v) { o->block_restart_interval = int(v); },
       0, 1},
      {"block_restart_interval", [](Options* o, int64_t v) { o->block_restart_interval = int(v); },
       1025, 1024},
      {"bloom_bits_per_key", [](Options* o, int64_t v) { o->bloom_bits_per_key = int(v); }, -1, 0},
      {"bloom_bits_per_key", [](Options* o, int64_t v) { o->bloom_bits_per_key = int(v); }, 31, 30},
      {"max_open_files", [](Options* o, int64_t v) { o->max_open_files = int(v); }, 19, 20},
      {"l0_compaction_trigger", [](Options* o, int64_t v) { o->l0_compaction_trigger = int(v); }, 0,
       1},
      {"l0_slowdown_trigger", [](Options* o, int64_t v) { o->l0_slowdown_trigger = int(v); }, 0, 1},
      {"l0_stop_trigger", [](Options* o, int64_t v) { o->l0_stop_trigger = int(v); }, 0, 8},
      {"max_bytes_for_level_base",
       [](Options* o, int64_t v) { o->max_bytes_for_level_base = uint64_t(v); }, 65535, 65536},
      {"level_size_multiplier", [](Options* o, int64_t v) { o->level_size_multiplier = int(v); }, 1,
       2},
      {"level_size_multiplier", [](Options* o, int64_t v) { o->level_size_multiplier = int(v); },
       101, 100},
      {"target_file_size", [](Options* o, int64_t v) { o->target_file_size = uint64_t(v); }, 16383,
       16384},
  };
}

TEST(OptionsTest, EveryBoundIsRefusedBelowAndAcceptedAt) {
  for (const Bound& bound : Bounds()) {
    Options invalid;
    bound.set(&invalid, bound.last_invalid);
    const Status refused = ValidateOptions(invalid);
    EXPECT_TRUE(refused.IsInvalidArgument())
        << bound.field << " = " << bound.last_invalid << ": " << refused.ToString();
    EXPECT_NE(refused.ToString().find(bound.field), std::string::npos)
        << "the error does not name the field: " << refused.ToString();

    Options valid;
    bound.set(&valid, bound.first_valid);
    EXPECT_TRUE(ValidateOptions(valid).ok()) << bound.field << " = " << bound.first_valid;
  }
}

TEST(OptionsTest, StopTriggerMayNotBeBelowSlowdownTrigger) {
  Options o;
  o.l0_slowdown_trigger = 13;
  o.l0_stop_trigger = 12;
  const Status s = ValidateOptions(o);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_NE(s.ToString().find("l0_stop_trigger"), std::string::npos) << s.ToString();

  o.l0_stop_trigger = 13;
  EXPECT_TRUE(ValidateOptions(o).ok());
}

TEST(OptionsTest, CompactionTriggerMayExceedTheOtherTriggers) {
  // Nothing relates the compaction trigger to the stall triggers; tests that want compaction never
  // to start between two flushes rely on that.
  Options o;
  o.l0_compaction_trigger = 10;
  o.l0_slowdown_trigger = 2;
  o.l0_stop_trigger = 10;
  EXPECT_TRUE(ValidateOptions(o).ok());
}

TEST(OptionsTest, EnvSchedulerAndCacheCapacityAreNotConstrained) {
  Options o;
  o.block_cache_capacity = 0;
  EXPECT_TRUE(ValidateOptions(o).ok());
}

}  // namespace
}  // namespace shale
