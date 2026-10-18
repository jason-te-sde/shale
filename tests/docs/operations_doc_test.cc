// Keeps the numbers of docs/operations.md true.
//
// The operations guide is the page that is read when something is wrong, and a default or a limit
// that it states wrongly is worse than one it does not state. So the tables that carry them, the
// options with their defaults and ranges, the yes-or-no options, the properties and the counters,
// are read from the document and held to the code:
//
//   - every default is what a default-constructed Options (or ReadOptions, or WriteOptions) holds;
//   - every minimum and maximum is where ValidateOptions() draws the line: the bound itself is
//     accepted, one past it is refused with an Invalid argument that names the option, and a field
//     that the document says has no bound accepts a value far past anything a bound would allow;
//   - every property that the document lists is one that GetProperty() knows, and the counters are
//     the ones `shale.counters` prints, in the order it prints them.
//
// The tables are found by the HTML comments around them; see docs/doc_tables.h.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "docs/doc_tables.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/options.h"
#include "shale/status.h"
#include "testkit/test_util.h"

namespace shale {
namespace {

using doc_tables::ParseNumber;
using doc_tables::TableRows;

std::vector<std::string> LoadLines() { return doc_tables::LoadLines(SHALE_OPERATIONS_DOC_PATH); }

// One numeric field of Options: how to read it, how to set it, and a value so far above anything a
// limit could be that a field with no maximum must accept it.
struct NumericOption {
  std::string name;
  std::function<int64_t(const Options&)> get;
  std::function<void(Options*, int64_t)> set;
  int64_t large;
};

constexpr int64_t kLargeSize = int64_t{1} << 40;
constexpr int64_t kLargeCount = int64_t{1} << 30;

// The field `member` of Options, as a NumericOption.
template <typename Member>
NumericOption MakeOption(const char* name, Member Options::* member, int64_t large) {
  return NumericOption{name, [member](const Options& o) { return static_cast<int64_t>(o.*member); },
                       [member](Options* o, int64_t v) { o->*member = static_cast<Member>(v); },
                       large};
}

std::vector<NumericOption> NumericOptions() {
  return {
      MakeOption("write_buffer_size", &Options::write_buffer_size, kLargeSize),
      MakeOption("block_size", &Options::block_size, kLargeSize),
      MakeOption("block_restart_interval", &Options::block_restart_interval, kLargeCount),
      MakeOption("bloom_bits_per_key", &Options::bloom_bits_per_key, kLargeCount),
      MakeOption("block_cache_capacity", &Options::block_cache_capacity, kLargeSize),
      MakeOption("max_open_files", &Options::max_open_files, kLargeCount),
      MakeOption("l0_compaction_trigger", &Options::l0_compaction_trigger, kLargeCount),
      MakeOption("l0_slowdown_trigger", &Options::l0_slowdown_trigger, kLargeCount),
      MakeOption("l0_stop_trigger", &Options::l0_stop_trigger, kLargeCount),
      MakeOption("max_bytes_for_level_base", &Options::max_bytes_for_level_base, kLargeSize),
      MakeOption("level_size_multiplier", &Options::level_size_multiplier, kLargeCount),
      MakeOption("target_file_size", &Options::target_file_size, kLargeSize),
  };
}

bool Accepts(const Options& options) { return ValidateOptions(options).ok(); }

// Whether the options are refused, with an Invalid argument that names `field`.
::testing::AssertionResult RefusedFor(const Options& options, const std::string& field) {
  const Status s = ValidateOptions(options);
  if (s.ok()) {
    return ::testing::AssertionFailure() << field << " was accepted";
  }
  if (!s.IsInvalidArgument()) {
    return ::testing::AssertionFailure()
           << "refused, but not as an invalid argument: " << s.ToString();
  }
  if (s.ToString().find(field) == std::string::npos) {
    return ::testing::AssertionFailure()
           << "refused, but not naming " << field << ": " << s.ToString();
  }
  return ::testing::AssertionSuccess();
}

TEST(OperationsDocTest, TheOptionsTableMatchesTheCode) {
  const std::vector<std::string> lines = LoadLines();
  const auto rows = TableRows(lines, "options", {"Option", "Default", "Minimum", "Maximum"});
  ASSERT_FALSE(rows.empty());

  std::map<std::string, std::vector<std::string>> by_name;
  for (const auto& row : rows) {
    EXPECT_TRUE(by_name.emplace(row[0], row).second) << row[0] << " appears twice";
  }
  const std::vector<NumericOption> fields = NumericOptions();
  std::map<std::string, const NumericOption*> field_by_name;
  for (const NumericOption& field : fields) {
    field_by_name[field.name] = &field;
    EXPECT_TRUE(by_name.count(field.name) != 0)
        << "docs/operations.md does not list the option " << field.name;
  }

  for (const auto& entry : by_name) {
    // Not a structured binding: a lambda below captures these, which older compilers refuse.
    const std::string& name = entry.first;
    const std::vector<std::string>& row = entry.second;
    const auto it = field_by_name.find(name);
    if (it == field_by_name.end()) {
      ADD_FAILURE() << "docs/operations.md lists '" << name << "', which this test does not know";
      continue;
    }
    const NumericOption& field = *it->second;
    SCOPED_TRACE(name);

    uint64_t documented_default = 0;
    ASSERT_TRUE(ParseNumber(row[1], &documented_default)) << "the default of " << name;
    EXPECT_EQ(static_cast<int64_t>(documented_default), field.get(Options{}))
        << "the default of " << name;

    // A bound is a number, "-" for none, or the name of another option that it follows.
    const auto check_bound = [&](const std::string& cell, bool is_minimum) {
      const std::string side = is_minimum ? "minimum" : "maximum";
      uint64_t number = 0;
      if (cell == "-") {
        // No bound is documented, so a value at the far end must be accepted: 0 for a minimum, and
        // a value far above any limit for a maximum.
        Options options;
        field.set(&options, is_minimum ? int64_t{0} : field.large);
        EXPECT_TRUE(Accepts(options)) << name << " is documented as having no " << side;
        return;
      }
      if (ParseNumber(cell, &number)) {
        const auto bound = static_cast<int64_t>(number);
        Options at_bound;
        field.set(&at_bound, bound);
        EXPECT_TRUE(Accepts(at_bound)) << name << " = " << bound << ", its documented " << side;
        Options past_bound;
        field.set(&past_bound, is_minimum ? bound - 1 : bound + 1);
        EXPECT_TRUE(RefusedFor(past_bound, name))
            << name << " one past its documented " << side << " of " << bound;
        return;
      }
      // The name of another option: this one is held at or above (minimum) or at or below
      // (maximum) the other's default.
      const auto other = field_by_name.find(cell);
      if (other == field_by_name.end()) {
        ADD_FAILURE() << "the " << side << " of " << name << " is '" << cell
                      << "', which is not a number, '-' or an option";
        return;
      }
      const int64_t limit = other->second->get(Options{});
      Options at_bound;
      field.set(&at_bound, limit);
      EXPECT_TRUE(Accepts(at_bound)) << name << " = " << cell << " (" << limit << ")";
      Options past_bound;
      field.set(&past_bound, is_minimum ? limit - 1 : limit + 1);
      EXPECT_TRUE(RefusedFor(past_bound, name))
          << name << " one past " << cell << " (" << limit << ")";
    };
    check_bound(row[2], /*is_minimum=*/true);
    check_bound(row[3], /*is_minimum=*/false);
  }
}

TEST(OperationsDocTest, TheYesOrNoDefaultsMatchTheCode) {
  const std::vector<std::string> lines = LoadLines();
  const auto rows = TableRows(lines, "flags", {"Option", "Default"});
  ASSERT_FALSE(rows.empty());

  const Options options;
  const ReadOptions read_options;
  const WriteOptions write_options;
  const std::map<std::string, bool> code = {
      {"Options::create_if_missing", options.create_if_missing},
      {"Options::error_if_exists", options.error_if_exists},
      {"Options::paranoid_checks", options.paranoid_checks},
      {"ReadOptions::verify_checksums", read_options.verify_checksums},
      {"ReadOptions::fill_cache", read_options.fill_cache},
      {"WriteOptions::sync", write_options.sync},
  };
  std::set<std::string> documented;
  for (const auto& row : rows) {
    documented.insert(row[0]);
    const auto it = code.find(row[0]);
    if (it == code.end()) {
      ADD_FAILURE() << "docs/operations.md lists '" << row[0] << "', which this test does not know";
      continue;
    }
    EXPECT_TRUE(row[1] == "true" || row[1] == "false") << row[0] << " is '" << row[1] << "'";
    EXPECT_EQ(row[1] == "true", it->second) << "the default of " << row[0];
  }
  for (const auto& [name, value] : code) {
    EXPECT_TRUE(documented.count(name) != 0) << "docs/operations.md does not list " << name;
  }
}

// An open, empty database in memory, for the tests of the properties.
struct OpenDatabase {
  std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<DB> db;

  void Open() {
    Options options;
    options.env = env.get();
    options.create_if_missing = true;
    ASSERT_OK(DB::Open(options, "/db", &db));
  }
};

TEST(OperationsDocTest, TheDocumentedPropertiesAreTheOnesTheDatabaseHas) {
  const std::vector<std::string> lines = LoadLines();
  const auto rows = TableRows(lines, "properties", {"Property", "Value"});
  ASSERT_FALSE(rows.empty());

  OpenDatabase open;
  open.Open();
  ASSERT_TRUE(open.db != nullptr);

  std::set<std::string> documented;
  for (const auto& row : rows) {
    documented.insert(row[0]);
    std::string value;
    EXPECT_TRUE(open.db->GetProperty(row[0], &value)) << "GetProperty does not know " << row[0];
  }
  EXPECT_EQ(documented,
            (std::set<std::string>{"shale.num-files-at-level0", "shale.total-sst-bytes",
                                   "shale.counters", "shale.write-amplification", "shale.stats",
                                   "shale.approximate-memory-usage", "shale.background-error"}))
      << "the table and this test disagree about which properties there are";

  // The family: levels 0 to 6 are properties, and 7 is not.
  std::string value;
  for (int level = 0; level < 7; level++) {
    EXPECT_TRUE(open.db->GetProperty("shale.num-files-at-level" + std::to_string(level), &value))
        << "level " << level;
    EXPECT_EQ(value, "0");
  }
  EXPECT_FALSE(open.db->GetProperty("shale.num-files-at-level7", &value));

  // What the table says of the values that can be seen on an empty database.
  EXPECT_TRUE(open.db->GetProperty("shale.write-amplification", &value));
  EXPECT_EQ(value, "0.00") << "documented as 0.00 before any write";
  EXPECT_TRUE(open.db->GetProperty("shale.background-error", &value));
  EXPECT_EQ(value, "OK");
  EXPECT_TRUE(open.db->GetProperty("shale.stats", &value));
  EXPECT_NE(value.find("Level 6: "), std::string::npos) << "a line for each of the 7 levels";
  EXPECT_EQ(value.find("Level 7: "), std::string::npos);
  EXPECT_NE(value.find("Counters: "), std::string::npos);
}

TEST(OperationsDocTest, TheDocumentedCountersAreTheOnesInTheOrderThePropertyPrintsThem) {
  const std::vector<std::string> lines = LoadLines();
  const auto rows = TableRows(lines, "counters", {"Counter", "Counts"});
  ASSERT_FALSE(rows.empty());
  std::vector<std::string> documented;
  for (const auto& row : rows) {
    documented.push_back(row[0]);
  }

  OpenDatabase open;
  open.Open();
  ASSERT_TRUE(open.db != nullptr);
  std::string text;
  ASSERT_TRUE(open.db->GetProperty("shale.counters", &text));

  // "name=value name=value ...": the names, in order.
  std::vector<std::string> printed;
  std::istringstream in(text);
  std::string pair;
  while (in >> pair) {
    const size_t equals = pair.find('=');
    ASSERT_NE(equals, std::string::npos) << "'" << pair << "' is not name=value";
    printed.push_back(pair.substr(0, equals));
  }
  EXPECT_EQ(documented, printed);
}

}  // namespace
}  // namespace shale
