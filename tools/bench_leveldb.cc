#include "bench_leveldb.h"

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/env.h>
#include <leveldb/filter_policy.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "bench_engine.h"
#include "leveldb_task_env.h"
#include "shale/status.h"

namespace shale {

double LevelDBHighestScore(const std::vector<uint64_t>& files_per_level,
                           const std::vector<uint64_t>& bytes_per_level) {
  // config::kL0_CompactionTrigger, and MaxBytesForLevel() of version_set.cc, in LevelDB 1.23.
  constexpr double kLevel0Trigger = 4;
  double limit = 10.0 * 1048576.0;
  double highest = 0;
  // The last level is left out: its files have nowhere to go.
  const size_t levels = std::min(files_per_level.size(), bytes_per_level.size());
  for (size_t level = 0; level + 1 < levels; level++) {
    double score = 0;
    if (level == 0) {
      score = static_cast<double>(files_per_level[0]) / kLevel0Trigger;
    } else {
      score = static_cast<double>(bytes_per_level[level]) / limit;
      limit *= 10;
    }
    highest = std::max(highest, score);
  }
  return highest;
}

namespace {

// The settings that put LevelDB level with shale's defaults. Where the two engines have the same
// knob, it has shale's default value here; what LevelDB has no knob for is not set. The settings
// that stay LevelDB's own, and that differ from shale's, are in docs/benchmarks.md.
constexpr size_t kWriteBufferSize = size_t{4} * 1024 * 1024;
constexpr size_t kBlockSize = 4096;
constexpr int kBloomBitsPerKey = 10;
constexpr size_t kBlockCacheCapacity = size_t{8} * 1024 * 1024;
constexpr int kMaxOpenFiles = 1000;

leveldb::Slice ToSlice(std::string_view text) { return leveldb::Slice(text.data(), text.size()); }

// LevelDB's status as one of ours, keeping its text. A status that is none of the kinds a caller
// tells apart is an IOError, which is what a failure of the engine is.
Status ToStatus(const leveldb::Status& status) {
  if (status.ok()) {
    return Status::OK();
  }
  const std::string text = status.ToString();
  if (status.IsNotFound()) {
    return Status::NotFound("leveldb", text);
  }
  if (status.IsCorruption()) {
    return Status::Corruption("leveldb", text);
  }
  if (status.IsInvalidArgument()) {
    return Status::InvalidArgument("leveldb", text);
  }
  if (status.IsNotSupportedError()) {
    return Status::NotSupported("leveldb", text);
  }
  return Status::IOError("leveldb", text);
}

// Reads a whole decimal number. Returns whether `text` is one and nothing else.
bool ParseWhole(std::string_view text, uint64_t* out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *out);
  return !text.empty() && error == std::errc() && end == text.data() + text.size();
}

// The lines of `text`, without their newlines.
std::vector<std::string_view> Lines(std::string_view text) {
  std::vector<std::string_view> lines;
  while (!text.empty()) {
    const size_t newline = text.find('\n');
    lines.push_back(text.substr(0, newline));
    text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
  }
  return lines;
}

// The words of `line`, separated by spaces.
std::vector<std::string_view> Words(std::string_view line) {
  std::vector<std::string_view> words;
  while (true) {
    const size_t start = line.find_first_not_of(' ');
    if (start == std::string_view::npos) {
      return words;
    }
    line.remove_prefix(start);
    const size_t end = line.find(' ');
    words.push_back(line.substr(0, end));
    line.remove_prefix(end == std::string_view::npos ? line.size() : end);
  }
}

Status Unreadable(const char* property, std::string_view line) {
  return Status::NotSupported(std::string(property) + " has a line that shale_bench cannot read",
                              std::string(line));
}

// Reads the text of "leveldb.sstables": for each level a line "--- level <n> ---", and under it a
// line " <number>:<bytes>[<smallest> .. <largest>]" for each of its tables.
Status ParseTables(std::string_view text, EngineState* state) {
  state->files_per_level.clear();
  state->bytes_per_level.clear();
  for (const std::string_view line : Lines(text)) {
    const std::string level_line =
        "--- level " + std::to_string(state->files_per_level.size()) + " ---";
    if (line == level_line) {
      state->files_per_level.push_back(0);
      state->bytes_per_level.push_back(0);
      continue;
    }
    const size_t colon = line.find(':');
    const size_t bracket = line.find('[');
    uint64_t number = 0;
    uint64_t bytes = 0;
    if (state->files_per_level.empty() || !line.starts_with(' ') ||
        colon == std::string_view::npos || bracket == std::string_view::npos || bracket < colon ||
        !ParseWhole(line.substr(1, colon - 1), &number) ||
        !ParseWhole(line.substr(colon + 1, bracket - colon - 1), &bytes)) {
      return Unreadable("leveldb.sstables", line);
    }
    state->files_per_level.back()++;
    state->bytes_per_level.back() += bytes;
  }
  if (state->files_per_level.empty()) {
    return Status::NotSupported("leveldb.sstables has no levels");
  }
  return Status::OK();
}

// Reads the text of "leveldb.stats": a header, a line of dashes, and a line for each level that
// has files or has been compacted into, with the level, its files, its size in MB, and the
// Time(sec), Read(MB) and Write(MB) of the compactions into it, each a whole number. The counters
// are the sums of the last three.
Status ParseStatistics(std::string_view text, EngineState* state) {
  uint64_t seconds = 0;
  uint64_t read_mb = 0;
  uint64_t write_mb = 0;
  bool table = false;
  for (const std::string_view line : Lines(text)) {
    if (!table) {
      table = line.starts_with("---");
      continue;
    }
    const std::vector<std::string_view> words = Words(line);
    if (words.empty()) {
      continue;
    }
    std::vector<uint64_t> figures(words.size());
    bool whole = words.size() == 6;
    for (size_t i = 0; whole && i < words.size(); i++) {
      whole = ParseWhole(words[i], &figures[i]);
    }
    if (!whole) {
      return Unreadable("leveldb.stats", line);
    }
    seconds += figures[3];
    read_mb += figures[4];
    write_mb += figures[5];
  }
  if (!table) {
    return Status::NotSupported("leveldb.stats has no table of levels");
  }
  state->counters = {{"time_sec", seconds}, {"read_mb", read_mb}, {"write_mb", write_mb}};
  return Status::OK();
}

class LevelDBBenchEngine final : public BenchEngine {
 public:
  explicit LevelDBBenchEngine(leveldb::Env* env)
      : task_env_(NewTaskCountingEnv(env != nullptr ? env : leveldb::Env::Default(), in_flight_)),
        filter_(leveldb::NewBloomFilterPolicy(kBloomBitsPerKey)) {
    options_.create_if_missing = true;
    options_.write_buffer_size = kWriteBufferSize;
    options_.block_size = kBlockSize;
    options_.filter_policy = filter_.get();
    options_.max_open_files = kMaxOpenFiles;
    options_.compression = leveldb::kNoCompression;
    options_.env = task_env_.get();
  }

  std::string Name() const override { return "leveldb"; }

  Status Open(const std::string& path, bool fresh) override {
    Close();
    if (fresh) {
      if (const leveldb::Status destroyed = leveldb::DestroyDB(path, options_); !destroyed.ok()) {
        return ToStatus(destroyed);
      }
    }
    return OpenDatabase(path, /*create_if_missing=*/true);
  }

  // LevelDB makes the directory, its lock and its info log before it finds that there is no
  // database at the path, so the engine looks for the database's CURRENT file first.
  Status OpenExisting(const std::string& path) override {
    Close();
    if (!task_env_->FileExists(path + "/CURRENT")) {
      return Status::InvalidArgument("leveldb", "there is no database at " + path);
    }
    return OpenDatabase(path, /*create_if_missing=*/false);
  }

  Status Put(std::string_view key, std::string_view value, bool sync) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    leveldb::WriteOptions write_options;
    write_options.sync = sync;
    return ToStatus(db_->Put(write_options, ToSlice(key), ToSlice(value)));
  }

  Status Get(std::string_view key, std::string* value) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    return ToStatus(db_->Get(VerifiedReadOptions(), ToSlice(key), value));
  }

  Status Scan(std::string_view from, size_t limit, size_t* seen) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    const std::unique_ptr<leveldb::Iterator> iterator(db_->NewIterator(VerifiedReadOptions()));
    if (from.empty()) {
      iterator->SeekToFirst();
    } else {
      iterator->Seek(ToSlice(from));
    }
    size_t visited = 0;
    for (; iterator->Valid() && (limit == 0 || visited < limit); iterator->Next()) {
      // As for shale: the key and the value are produced, and their bytes are not read.
      static_cast<void>(iterator->key());
      static_cast<void>(iterator->value());
      visited++;
    }
    if (const Status status = ToStatus(iterator->status()); !status.ok()) {
      return status;
    }
    *seen = visited;
    return Status::OK();
  }

  Status CompactAll() override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    db_->CompactRange(nullptr, nullptr);
    return Status::OK();
  }

  // LevelDB's statistics give the bytes that each level read and wrote, not a ratio of what reached
  // the disk to what the callers gave it that the adapter could report without working it out
  // itself, and a figure of the adapter's own making would not be LevelDB's.
  std::string WriteAmplification() override { return "n/a"; }

  Status GetState(EngineState* state) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    // Whether the background is busy first, and the tables after it, as for shale.
    state->background_busy = in_flight_->load(std::memory_order_acquire) > 0;
    std::string tables;
    std::string statistics;
    if (!db_->GetProperty("leveldb.sstables", &tables) ||
        !db_->GetProperty("leveldb.stats", &statistics)) {
      return Status::NotSupported("LevelDB has no property leveldb.sstables or leveldb.stats");
    }
    if (const Status s = ParseTables(tables, state); !s.ok()) {
      return s;
    }
    if (const Status s = ParseStatistics(statistics, state); !s.ok()) {
      return s;
    }
    state->highest_score = LevelDBHighestScore(state->files_per_level, state->bytes_per_level);
    state->write_amplification = WriteAmplification();
    return Status::OK();
  }

  void Close() override { db_.reset(); }

 private:
  static Status NotOpen() { return Status::InvalidArgument("the benchmark database is not open"); }

  Status OpenDatabase(const std::string& path, bool create_if_missing) {
    // The cache is new at every open, as the cache that is part of a shale database is: a database
    // that was opened again starts with nothing cached, and not with what the one before it read.
    cache_.reset(leveldb::NewLRUCache(kBlockCacheCapacity));
    options_.block_cache = cache_.get();
    options_.create_if_missing = create_if_missing;
    leveldb::DB* db = nullptr;
    const leveldb::Status opened = leveldb::DB::Open(options_, path, &db);
    db_.reset(db);
    return ToStatus(opened);
  }

  // What shale reads with, which is checksummed: LevelDB's default is not to verify, so it is
  // asked to. A read fills the cache, as it does by default in both.
  static leveldb::ReadOptions VerifiedReadOptions() {
    leveldb::ReadOptions read_options;
    read_options.verify_checksums = true;
    return read_options;
  }

  // The background tasks that LevelDB has handed to the Env and that have not returned, and the
  // Env that counts them, which every database of the engine is opened with.
  const std::shared_ptr<std::atomic<uint64_t>> in_flight_ =
      std::make_shared<std::atomic<uint64_t>>(0);
  const std::unique_ptr<leveldb::Env> task_env_;
  const std::unique_ptr<const leveldb::FilterPolicy> filter_;
  leveldb::Options options_;
  // Declared after the cache and the filter policy that the database uses, so that it is destroyed
  // before them.
  std::unique_ptr<leveldb::Cache> cache_;
  std::unique_ptr<leveldb::DB> db_;
};

}  // namespace

std::unique_ptr<BenchEngine> NewLevelDBBenchEngine(leveldb::Env* env) {
  return std::make_unique<LevelDBBenchEngine>(env);
}

}  // namespace shale
