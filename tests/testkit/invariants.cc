#include "testkit/invariants.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_state.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "testkit/model_db.h"
#include "util/dbformat.h"
#include "util/filename.h"

namespace shale {

namespace {

// The longest run of a key or a value that a message shows; the rest is counted, not printed.
constexpr size_t kShownBytes = 48;

// Returns `bytes` quoted, with every byte that is not printable ASCII, and the quote and the
// backslash, as \xNN, cut to kShownBytes with its full length said. Keys and values are binary in
// the simulator, and a message that holds them raw is one that a terminal can garble.
std::string Show(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string shown = "'";
  const size_t n = std::min(bytes.size(), kShownBytes);
  for (size_t i = 0; i < n; i++) {
    const auto c = static_cast<unsigned char>(bytes[i]);
    if (c >= 0x20 && c <= 0x7e && c != '\'' && c != '\\') {
      shown.push_back(static_cast<char>(c));
    } else {
      shown += "\\x";
      shown.push_back(kHex[c >> 4]);
      shown.push_back(kHex[c & 0xf]);
    }
  }
  shown.push_back('\'');
  if (bytes.size() > n) {
    shown += "... (" + std::to_string(bytes.size()) + " bytes)";
  }
  return shown;
}

// The alarm of invariant `id`.
Status Alarm(std::string_view id, const std::string& message) {
  return Status::Corruption(std::string(id) + ": " + message);
}

// The text of `s` without its leading code name, "Corruption: ", for a message that carries
// another check's finding inside its own.
std::string MessageOf(const Status& s) {
  const std::string text = s.ToString();
  const size_t colon = text.find(": ");
  return colon == std::string::npos ? text : text.substr(colon + 2);
}

// What differs between `expected` and `actual`, for the first key where they do; empty if they are
// equal.
std::string FirstDifference(const ModelDB::State& expected, const ModelDB::State& actual) {
  auto want = expected.begin();
  auto got = actual.begin();
  while (want != expected.end() || got != actual.end()) {
    if (got == actual.end() || (want != expected.end() && want->first < got->first)) {
      return "key " + Show(want->first) + " is missing";
    }
    if (want == expected.end() || got->first < want->first) {
      return "key " + Show(got->first) + " is there and should not be";
    }
    if (want->second != got->second) {
      return "key " + Show(want->first) + " has value " + Show(got->second) + ", expected " +
             Show(want->second);
    }
    ++want;
    ++got;
  }
  return "";
}

// Reads every entry of `db` into `*out`. A key that is not above the one before it is an alarm of
// `id`, because the map that holds the result would take it in silently, a duplicate vanishing and
// a disorder being put right.
Status ReadAll(DB* db, std::string_view id, ModelDB::State* out) {
  out->clear();
  const std::unique_ptr<Iterator> it = db->NewIterator(ReadOptions());
  const std::string* previous = nullptr;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const auto [position, inserted] =
        out->emplace(std::string(it->key()), std::string(it->value()));
    if (!inserted || (previous != nullptr && !(*previous < position->first))) {
      return Alarm(id, "a scan serves key " + Show(position->first) + " after " +
                           Show(previous != nullptr ? *previous : std::string()) +
                           ", which is not ascending order");
    }
    previous = &position->first;
  }
  return it->status();
}

// The state that `durable` becomes with the first `k` batches of `uncertain` applied.
ModelDB::State AfterPrefix(const ModelDB::State& durable, const std::vector<WriteBatch>& uncertain,
                           size_t k) {
  ModelDB model;
  model.Reset(durable);
  for (size_t i = 0; i < k; i++) {
    model.Apply(uncertain[i]);
  }
  return model.state();
}

}  // namespace

bool IsInvariantViolation(const Status& s) {
  if (!s.IsCorruption()) {
    return false;
  }
  // "Corruption: I", digits, ": ".
  const std::string text = MessageOf(s);
  if (text.size() < 4 || text[0] != 'I') {
    return false;
  }
  size_t i = 1;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    i++;
  }
  return i > 1 && text.compare(i, 2, ": ") == 0;
}

Status CheckReadsMatch(DB* db, const ModelDB::State& model, const Snapshot* snapshot) {
  ReadOptions options;
  options.snapshot = snapshot;
  const std::string where = snapshot != nullptr ? " at a snapshot" : "";

  for (const auto& [key, want] : model) {
    std::string got;
    const Status s = db->Get(options, key, &got);
    if (s.IsNotFound()) {
      return Alarm(
          "I1", "Get of key " + Show(key) + where + " found nothing, expected value " + Show(want));
    }
    if (!s.ok()) {
      return s;
    }
    if (got != want) {
      return Alarm("I1", "Get of key " + Show(key) + where + " returned value " + Show(got) +
                             ", expected " + Show(want));
    }
  }

  // The scan is compared with the model entry by entry, as the two come, and not after the scan has
  // been collected into a map, which would put a disorder right and swallow a duplicate. The model
  // is in ascending order, so a key that is out of place, or twice, is one that is not the next.
  const std::unique_ptr<Iterator> it = db->NewIterator(options);
  auto expected = model.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const std::string key(it->key());
    if (expected == model.end() || key < expected->first) {
      return Alarm("I1",
                   "scan" + where + " has key " + Show(key) + " that the model does not hold");
    }
    if (expected->first < key) {
      return Alarm("I1", "scan" + where + " is missing key " + Show(expected->first) +
                             " (it went on to " + Show(key) + ")");
    }
    if (it->value() != expected->second) {
      return Alarm("I1", "scan" + where + " has value " + Show(it->value()) + " for key " +
                             Show(key) + ", expected " + Show(expected->second));
    }
    ++expected;
  }
  // An iterator that stopped because it failed has not shown that the data ran out.
  if (!it->status().ok()) {
    return it->status();
  }
  if (expected != model.end()) {
    return Alarm("I1", "scan" + where + " ended before key " + Show(expected->first));
  }
  return Status::OK();
}

Status CheckVersion(Env* env, const std::string& dbname, const VersionState& state,
                    const std::set<uint64_t>& live_files, SequenceNumber last_sequence,
                    SequenceNumber previous_last_sequence) {
  const Status valid = CheckInvariants(state);
  if (!valid.ok()) {
    return Alarm("I2", "the version breaks its own rules: " + MessageOf(valid));
  }
  if (last_sequence < previous_last_sequence) {
    return Alarm("I2", "last_sequence went back from " + std::to_string(previous_last_sequence) +
                           " to " + std::to_string(last_sequence));
  }
  if (state.last_sequence > last_sequence) {
    return Alarm("I2", "the tables hold last_sequence " + std::to_string(state.last_sequence) +
                           ", above the database's " + std::to_string(last_sequence));
  }

  std::map<uint64_t, uint64_t> recorded_sizes;
  for (const std::vector<FileMetaData>& level : state.files) {
    for (const FileMetaData& file : level) {
      if (live_files.count(file.number) == 0) {
        return Alarm("I2", "table " + TableFileName(dbname, file.number) +
                               " is in the version and not in the live set");
      }
      recorded_sizes[file.number] = file.file_size;
    }
  }

  for (const uint64_t number : live_files) {
    const std::string path = TableFileName(dbname, number);
    if (!env->FileExists(path)) {
      return Alarm("I2", "live table " + path + " does not exist");
    }
    const auto recorded = recorded_sizes.find(number);
    if (recorded == recorded_sizes.end()) {
      continue;
    }
    uint64_t size = 0;
    const Status s = env->GetFileSize(path, &size);
    if (!s.ok()) {
      return s;
    }
    if (size != recorded->second) {
      return Alarm("I2", "table " + path + " is " + std::to_string(size) +
                             " bytes, and the version records its size as " +
                             std::to_string(recorded->second));
    }
  }
  return Status::OK();
}

Status CheckNoOrphanTables(Env* env, const std::string& dbname,
                           const std::set<uint64_t>& live_files) {
  std::vector<std::string> names;
  const Status listed = env->GetChildren(dbname, &names);
  if (!listed.ok()) {
    return listed;
  }
  std::sort(names.begin(), names.end());
  std::vector<std::string> orphans;
  for (const std::string& name : names) {
    uint64_t number = 0;
    FileType type = FileType::kCurrentFile;
    if (ParseFileName(name, &number, &type) && type == FileType::kTableFile &&
        live_files.count(number) == 0) {
      orphans.push_back(name);
    }
  }
  if (orphans.empty()) {
    return Status::OK();
  }
  return Alarm("I3", "table " + orphans.front() + " is in " + dbname + " and in no live version (" +
                         std::to_string(orphans.size()) + " such table(s), " +
                         std::to_string(live_files.size()) + " live)");
}

Status CheckRecoveredPrefix(DB* db, const ModelDB::State& durable,
                            const std::vector<WriteBatch>& uncertain, size_t min_prefix,
                            ModelDB::State* matched) {
  if (min_prefix > uncertain.size()) {
    return Status::InvalidArgument("CheckRecoveredPrefix: min_prefix " +
                                   std::to_string(min_prefix) + " is above the " +
                                   std::to_string(uncertain.size()) + " uncertain write(s)");
  }
  ModelDB::State actual;
  const Status read = ReadAll(db, "I4", &actual);
  if (!read.ok()) {
    return read;
  }

  ModelDB model;
  model.Reset(durable);
  for (size_t k = 0;; k++) {
    if (k >= min_prefix && model.state() == actual) {
      if (matched != nullptr) {
        *matched = actual;
      }
      return Status::OK();
    }
    if (k == uncertain.size()) {
      break;
    }
    model.Apply(uncertain[k]);
  }

  // No prefix fits. Of the ones that were allowed, the shortest and the longest are the two ends of
  // what the database could have kept, so what differs from each says what is wrong.
  const std::string against_shortest =
      FirstDifference(AfterPrefix(durable, uncertain, min_prefix), actual);
  const std::string against_longest = FirstDifference(model.state(), actual);
  return Alarm("I4", "the recovered state (" + std::to_string(actual.size()) +
                         " key(s)) is the durable state (" + std::to_string(durable.size()) +
                         " key(s)) with no prefix of the " + std::to_string(uncertain.size()) +
                         " uncertain write(s), from " + std::to_string(min_prefix) +
                         " on, applied; "
                         "against prefix " +
                         std::to_string(min_prefix) + ": " + against_shortest +
                         "; against prefix " + std::to_string(uncertain.size()) + ": " +
                         against_longest);
}

}  // namespace shale
