// Runs the input as a program of database operations, against a database on a MemEnv and a
// ManualScheduler and against the reference model, and stops the run if they ever disagree. The
// format of the program is in db_ops_program.h.
//
// The database is opened with the smallest write buffer there is, and with level 0 compacted as
// soon as it holds two tables, so that the flushes and compactions that the program does not ask
// for happen anyway. The scheduler runs nothing by itself: a flush or a compaction that the engine
// queues runs when the program waits for it or reaches a flush or a compact, so where the
// background work falls in the stream of writes is a part of the input.
//
// Nothing here injects a failure, so every call that the engine could refuse for a bad argument
// must succeed, and any status but OK is a disagreement with the model like a wrong value is.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "db_ops_program.h"
#include "fuzz_util.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/scheduler.h"
#include "shale/status.h"
#include "testkit/model_db.h"

namespace shale {
namespace {

// Reads the program a byte at a time.
class Program {
 public:
  explicit Program(std::string_view bytes) : bytes_(bytes) {}

  // Takes the next byte, or returns false when the program has ended.
  bool Next(uint8_t* byte) {
    if (pos_ == bytes_.size()) {
      return false;
    }
    *byte = static_cast<uint8_t>(bytes_[pos_++]);
    return true;
  }

 private:
  const std::string_view bytes_;
  size_t pos_ = 0;
};

Options FuzzOptions(Env* env, Scheduler* scheduler) {
  Options options;
  options.env = env;
  options.scheduler = scheduler;
  options.create_if_missing = true;
  options.write_buffer_size = 16384;  // the smallest the engine allows
  options.block_size = 256;           // so that a table of a few entries has several blocks
  options.block_restart_interval = 4;
  options.block_cache_capacity = size_t{1} << 20;
  options.l0_compaction_trigger = 2;  // a second flush makes a compaction due
  options.l0_slowdown_trigger = 4;
  options.l0_stop_trigger = 6;
  options.target_file_size = 16384;          // the smallest, so that a compaction cuts its output
  options.max_bytes_for_level_base = 65536;  // the smallest
  return options;
}

// What a database must show for the keys at or after `from` (all of them if it is empty), as the
// model holds them.
std::map<std::string, std::string> ExpectedFrom(const ModelDB& model, const std::string& from) {
  std::map<std::string, std::string> expected;
  for (auto it = model.state().lower_bound(from); it != model.state().end(); ++it) {
    expected.insert(*it);
  }
  return expected;
}

// The checks of the reads below are the simulator's I1 (CheckReadsMatch() in testkit/invariants.h)
// for a database that nothing has disturbed, with two things that it does not have: the scan that
// starts at a key, and a miss on a key that was deleted. They are written out here, in the form of
// a trap, so that a failure is a crash that libFuzzer saves and not a Status to be read.

// Reads `db` from `seek_to`, or from its first entry if that is empty, to its end, and requires
// what it finds to be the model's contents from there on, in key order.
void CheckScan(DB* db, const ModelDB& model, const std::string& seek_to) {
  const std::unique_ptr<Iterator> it = db->NewIterator(ReadOptions());
  if (seek_to.empty()) {
    it->SeekToFirst();
  } else {
    it->Seek(seek_to);
  }
  const std::map<std::string, std::string> expected = ExpectedFrom(model, seek_to);
  auto next = expected.begin();
  for (; it->Valid(); it->Next()) {
    SHALE_FUZZ_CHECK(next != expected.end());
    SHALE_FUZZ_CHECK(it->key() == next->first);
    SHALE_FUZZ_CHECK(it->value() == next->second);
    ++next;
  }
  SHALE_FUZZ_CHECK(it->status().ok());
  SHALE_FUZZ_CHECK(next == expected.end());
}

void CheckGet(DB* db, const ModelDB& model, const std::string& key) {
  std::string value;
  const Status s = db->Get(ReadOptions(), key, &value);
  const std::optional<std::string> expected = model.Get(key);
  if (expected.has_value()) {
    SHALE_FUZZ_CHECK(s.ok());
    SHALE_FUZZ_CHECK(value == *expected);
  } else {
    SHALE_FUZZ_CHECK(s.IsNotFound());
  }
}

// The value of the put with number `serial`: bytes that differ from one put to the next, so that a
// read that returns an older value for a key is not mistaken for the right one.
std::string ValueOf(uint64_t serial, unsigned length) {
  std::string value(length, 'a');
  for (unsigned i = 0; i < length; i++) {
    value[i] = static_cast<char>('a' + (serial * 7 + i) % 26);
  }
  return value;
}

void Run(std::string_view bytes) {
  const std::unique_ptr<Env> env = NewMemEnv();
  // Declared before the database, so that it is destroyed after it: the database leaves tasks in
  // it.
  ManualScheduler scheduler;
  const Options options = FuzzOptions(env.get(), &scheduler);
  const std::string name = "/fuzzdb";
  std::unique_ptr<DB> db;
  SHALE_FUZZ_CHECK(DB::Open(options, name, &db).ok());

  ModelDB model;
  WriteOptions write_options;
  write_options.sync = false;  // a MemEnv has nothing to sync, and the log is replayed at the end
  uint64_t puts = 0;

  Program program(bytes);
  uint8_t op_byte = 0;
  while (program.Next(&op_byte)) {
    uint8_t a = 0;
    uint8_t b = 0;
    switch (static_cast<fuzz::DbOp>(op_byte % fuzz::kNumDbOps)) {
      case fuzz::DbOp::kPut: {
        if (!program.Next(&a) || !program.Next(&b)) {
          break;
        }
        const std::string key = fuzz::DbOpKey(a);
        const std::string value = ValueOf(puts++, b % (fuzz::kMaxDbValueLength + 1U));
        SHALE_FUZZ_CHECK(db->Put(write_options, key, value).ok());
        model.Put(key, value);
        continue;
      }
      case fuzz::DbOp::kDelete: {
        if (!program.Next(&a)) {
          break;
        }
        const std::string key = fuzz::DbOpKey(a);
        SHALE_FUZZ_CHECK(db->Delete(write_options, key).ok());
        model.Delete(key);
        continue;
      }
      case fuzz::DbOp::kGet: {
        if (!program.Next(&a)) {
          break;
        }
        CheckGet(db.get(), model, fuzz::DbOpKey(a));
        continue;
      }
      case fuzz::DbOp::kFlush:
        SHALE_FUZZ_CHECK(db->Flush().ok());
        // What the flush made due, a compaction among it, runs now and not at some later write.
        scheduler.RunPending();
        continue;
      case fuzz::DbOp::kCompact: {
        if (!program.Next(&a) || !program.Next(&b)) {
          break;
        }
        std::string low = fuzz::DbOpKey(a);
        std::string high = fuzz::DbOpKey(b);
        if (high < low) {
          std::swap(low, high);
        }
        SHALE_FUZZ_CHECK(db->CompactRange(std::string_view(low), std::string_view(high)).ok());
        scheduler.RunPending();
        continue;
      }
      case fuzz::DbOp::kScan: {
        if (!program.Next(&a)) {
          break;
        }
        CheckScan(db.get(), model, "");
        CheckScan(db.get(), model, fuzz::DbOpKey(a));
        continue;
      }
    }
    break;  // an operation that the program ended inside
  }

  // What the program left must be what the model holds, and so must what a restart reads back from
  // the tables and the logs that the database has written.
  CheckScan(db.get(), model, "");
  scheduler.RunPending();
  db.reset();
  SHALE_FUZZ_CHECK(DB::Open(options, name, &db).ok());
  CheckScan(db.get(), model, "");
  for (unsigned key = 0; key < fuzz::kNumDbKeys; key++) {
    CheckGet(db.get(), model, fuzz::DbOpKey(key));
  }
}

}  // namespace
}  // namespace shale

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  shale::Run(std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
