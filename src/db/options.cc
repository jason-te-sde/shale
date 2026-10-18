#include "shale/options.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "shale/status.h"

namespace shale {

namespace {

// The error for `field` being out of range: the field, what it may be, and what it was.
Status OutOfRange(const char* field, const std::string& rule, const std::string& value) {
  return Status::InvalidArgument(std::string(field) + " must be " + rule, "got " + value);
}

}  // namespace

Status ValidateOptions(const Options& options) {
  // What breaks the engine, and not only what is unwise: the picker aborts on a trigger below one
  // or a level size of nothing, and a table builder cannot work in blocks of no size. The bounds
  // are checked before a database is opened, since a database that is open has already used them.
  if (options.write_buffer_size < 16384) {
    return OutOfRange("write_buffer_size", "at least 16384",
                      std::to_string(options.write_buffer_size));
  }
  if (options.block_size < 256 || options.block_size > 4194304) {
    return OutOfRange("block_size", "from 256 to 4194304", std::to_string(options.block_size));
  }
  if (options.block_restart_interval < 1 || options.block_restart_interval > 1024) {
    return OutOfRange("block_restart_interval", "from 1 to 1024",
                      std::to_string(options.block_restart_interval));
  }
  if (options.bloom_bits_per_key < 0 || options.bloom_bits_per_key > 30) {
    return OutOfRange("bloom_bits_per_key", "from 0 to 30",
                      std::to_string(options.bloom_bits_per_key));
  }
  if (options.max_open_files < 20) {
    return OutOfRange("max_open_files", "at least 20", std::to_string(options.max_open_files));
  }
  if (options.l0_compaction_trigger < 1) {
    return OutOfRange("l0_compaction_trigger", "at least 1",
                      std::to_string(options.l0_compaction_trigger));
  }
  if (options.l0_slowdown_trigger < 1) {
    return OutOfRange("l0_slowdown_trigger", "at least 1",
                      std::to_string(options.l0_slowdown_trigger));
  }
  if (options.l0_stop_trigger < 1) {
    return OutOfRange("l0_stop_trigger", "at least 1", std::to_string(options.l0_stop_trigger));
  }
  if (options.l0_stop_trigger < options.l0_slowdown_trigger) {
    return OutOfRange("l0_stop_trigger", "at least l0_slowdown_trigger",
                      std::to_string(options.l0_stop_trigger) + " with l0_slowdown_trigger " +
                          std::to_string(options.l0_slowdown_trigger));
  }
  if (options.max_bytes_for_level_base < 65536) {
    return OutOfRange("max_bytes_for_level_base", "at least 65536",
                      std::to_string(options.max_bytes_for_level_base));
  }
  if (options.level_size_multiplier < 2 || options.level_size_multiplier > 100) {
    return OutOfRange("level_size_multiplier", "from 2 to 100",
                      std::to_string(options.level_size_multiplier));
  }
  if (options.target_file_size < 16384) {
    return OutOfRange("target_file_size", "at least 16384",
                      std::to_string(options.target_file_size));
  }
  return Status::OK();
}

}  // namespace shale
