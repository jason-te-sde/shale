#include "bench_engine.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "shale/db.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"

namespace shale {

BenchEngine::~BenchEngine() = default;

std::string FilesPerLevelText(const std::vector<uint64_t>& files_per_level) {
  std::string text;
  for (const uint64_t files : files_per_level) {
    text += text.empty() ? "" : "/";
    text += std::to_string(files);
  }
  return text;
}

double ShaleHighestScore(const std::vector<uint64_t>& files_per_level,
                         const std::vector<uint64_t>& bytes_per_level, const Options& options) {
  const int trigger = std::min(options.l0_compaction_trigger, options.l0_stop_trigger);
  double highest = 0;
  double limit = static_cast<double>(options.max_bytes_for_level_base);
  // The last level is left out: its files have nowhere to go.
  const size_t levels = std::min(files_per_level.size(), bytes_per_level.size());
  for (size_t level = 0; level + 1 < levels; level++) {
    double score = 0;
    if (level == 0) {
      score = static_cast<double>(files_per_level[0]) / static_cast<double>(trigger);
    } else {
      score = static_cast<double>(bytes_per_level[level]) / limit;
      limit *= static_cast<double>(options.level_size_multiplier);
    }
    highest = std::max(highest, score);
  }
  return highest;
}

namespace {

// A Scheduler that hands every task to another one and counts the tasks that are queued or running.
// A task is counted from the moment it is handed in until it has returned, and a task that queues
// another, as shale's does when there is more to do, has counted the next before it returns: so
// the count is zero only when nothing is left to run.
class TaskCountingScheduler final : public Scheduler {
 public:
  explicit TaskCountingScheduler(Scheduler* target) : target_(target) {}

  void Schedule(std::function<void()> task) override {
    in_flight_->fetch_add(1, std::memory_order_acq_rel);
    // The count is shared with the task, which may still be returning when the scheduler is gone.
    target_->Schedule([task = std::move(task), in_flight = in_flight_] {
      task();
      in_flight->fetch_sub(1, std::memory_order_acq_rel);
    });
  }

  bool RunsOnCallerThread() const override { return target_->RunsOnCallerThread(); }

  size_t RunPending() override { return target_->RunPending(); }

  // The tasks handed in that have not returned.
  uint64_t in_flight() const { return in_flight_->load(std::memory_order_acquire); }

 private:
  Scheduler* const target_;
  const std::shared_ptr<std::atomic<uint64_t>> in_flight_ =
      std::make_shared<std::atomic<uint64_t>>(0);
};

// Reads a whole decimal number. Returns whether `text` is one and nothing else.
bool ParseWhole(std::string_view text, uint64_t* out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *out);
  return !text.empty() && error == std::errc() && end == text.data() + text.size();
}

// Reads the text of "shale.stats": a header line, a line "Level <n>: <files> files, <bytes>
// bytes" for each level, and "Counters: " with the counters as "name=value" pairs separated by
// single spaces.
Status ParseStats(std::string_view text, EngineState* state) {
  const auto unreadable = [](std::string_view line) {
    return Status::NotSupported("shale.stats has a line that shale_bench cannot read",
                                std::string(line));
  };
  state->files_per_level.clear();
  state->bytes_per_level.clear();
  state->counters.clear();
  bool counters_seen = false;
  while (!text.empty()) {
    const size_t newline = text.find('\n');
    const std::string_view line = text.substr(0, newline);
    text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
    constexpr std::string_view kLevel = "Level ";
    constexpr std::string_view kCounters = "Counters: ";
    if (line.starts_with(kLevel)) {
      const std::string level_name =
          "Level " + std::to_string(state->files_per_level.size()) + ": ";
      const size_t files_end = line.find(" files, ");
      uint64_t files = 0;
      uint64_t bytes = 0;
      if (!line.starts_with(level_name) || files_end == std::string_view::npos ||
          !line.ends_with(" bytes") ||
          !ParseWhole(line.substr(level_name.size(), files_end - level_name.size()), &files) ||
          !ParseWhole(line.substr(files_end + 8, line.size() - 6 - (files_end + 8)), &bytes)) {
        return unreadable(line);
      }
      state->files_per_level.push_back(files);
      state->bytes_per_level.push_back(bytes);
    } else if (line.starts_with(kCounters)) {
      std::string_view pairs = line.substr(kCounters.size());
      while (!pairs.empty()) {
        const size_t space = pairs.find(' ');
        const std::string_view pair = pairs.substr(0, space);
        pairs.remove_prefix(space == std::string_view::npos ? pairs.size() : space + 1);
        const size_t equals = pair.find('=');
        uint64_t value = 0;
        if (equals == std::string_view::npos || equals == 0 ||
            !ParseWhole(pair.substr(equals + 1), &value)) {
          return unreadable(line);
        }
        state->counters.emplace_back(std::string(pair.substr(0, equals)), value);
      }
      counters_seen = true;
    } else if (line != "shale stats" && !line.empty()) {
      return unreadable(line);
    }
  }
  if (state->files_per_level.empty() || !counters_seen) {
    return Status::NotSupported("shale.stats has no levels or no counters");
  }
  return Status::OK();
}

class ShaleBenchEngine final : public BenchEngine {
 public:
  explicit ShaleBenchEngine(const Options& base)
      : options_(base), given_scheduler_(base.scheduler) {
    options_.create_if_missing = true;
  }

  ~ShaleBenchEngine() override { Close(); }

  ShaleBenchEngine(const ShaleBenchEngine&) = delete;
  ShaleBenchEngine& operator=(const ShaleBenchEngine&) = delete;

  std::string Name() const override { return "shale"; }

  Status Open(const std::string& path, bool fresh) override {
    Close();
    if (fresh) {
      if (const Status destroyed = DestroyDB(path, options_); !destroyed.ok()) {
        return destroyed;
      }
    }
    return OpenDatabase(path, /*create_if_missing=*/true);
  }

  // shale makes nothing at a path that holds no database when it is not asked to create one.
  Status OpenExisting(const std::string& path) override {
    Close();
    return OpenDatabase(path, /*create_if_missing=*/false);
  }

  Status Put(std::string_view key, std::string_view value, bool sync) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    WriteOptions write_options;
    write_options.sync = sync;
    return db_->Put(write_options, key, value);
  }

  Status Get(std::string_view key, std::string* value) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    return db_->Get(ReadOptions(), key, value);
  }

  Status Scan(std::string_view from, size_t limit, size_t* seen) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    const std::unique_ptr<Iterator> iterator = db_->NewIterator(ReadOptions());
    if (from.empty()) {
      iterator->SeekToFirst();
    } else {
      iterator->Seek(from);
    }
    size_t visited = 0;
    for (; iterator->Valid() && (limit == 0 || visited < limit); iterator->Next()) {
      // A scan hands the caller the entry, so the key and the value are both produced. Their bytes
      // are not read: the engine's work is what is measured, as db_bench does.
      static_cast<void>(iterator->key());
      static_cast<void>(iterator->value());
      visited++;
    }
    if (const Status status = iterator->status(); !status.ok()) {
      return status;
    }
    *seen = visited;
    return Status::OK();
  }

  Status CompactAll() override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    return db_->CompactRange(std::nullopt, std::nullopt);
  }

  std::string WriteAmplification() override {
    std::string figure;
    if (db_ == nullptr || !db_->GetProperty("shale.write-amplification", &figure)) {
      return "n/a";
    }
    return figure;
  }

  Status GetState(EngineState* state) override {
    if (db_ == nullptr) {
      return NotOpen();
    }
    // Whether the background is busy first, and the tables after it: once nothing is queued or
    // running, nothing is queued again until something is written, so a state that says idle
    // shows the tables that the background left.
    state->background_busy = counting_scheduler_->in_flight() > 0;
    std::string stats;
    if (!db_->GetProperty("shale.stats", &stats)) {
      return Status::NotSupported("the engine has no property shale.stats");
    }
    if (const Status s = ParseStats(stats, state); !s.ok()) {
      return s;
    }
    state->highest_score =
        ShaleHighestScore(state->files_per_level, state->bytes_per_level, options_);
    state->write_amplification = WriteAmplification();
    return Status::OK();
  }

  // The database first, then the scheduler it ran its work on: closing a database waits for its
  // task, and a scheduler with a thread runs what is still queued when it is destroyed.
  void Close() override {
    db_.reset();
    counting_scheduler_.reset();
    thread_scheduler_.reset();
  }

 private:
  static Status NotOpen() { return Status::InvalidArgument("the benchmark database is not open"); }

  Status OpenDatabase(const std::string& path, bool create_if_missing) {
    // A scheduler with a thread of its own for each database, as shale makes for a database that is
    // opened without one, unless the options gave one; either way, counted.
    if (given_scheduler_ == nullptr) {
      thread_scheduler_ = NewThreadScheduler();
    }
    counting_scheduler_ = std::make_unique<TaskCountingScheduler>(
        given_scheduler_ != nullptr ? given_scheduler_ : thread_scheduler_.get());
    Options options = options_;
    options.scheduler = counting_scheduler_.get();
    options.create_if_missing = create_if_missing;
    const Status opened = DB::Open(options, path, &db_);
    if (!opened.ok()) {
      Close();
    }
    return opened;
  }

  Options options_;
  Scheduler* const given_scheduler_;
  std::unique_ptr<Scheduler> thread_scheduler_;
  std::unique_ptr<TaskCountingScheduler> counting_scheduler_;
  std::unique_ptr<DB> db_;
};

}  // namespace

std::unique_ptr<BenchEngine> NewShaleBenchEngine(const Options& base) {
  return std::make_unique<ShaleBenchEngine>(base);
}

}  // namespace shale
