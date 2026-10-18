// Writes the seed corpus of the fuzz targets: for each target, a few valid inputs of its format,
// made by the engine's own writers. A fuzzer that starts from a valid file gets to the code that
// decodes one at once, where one that starts from nothing would spend its first minutes finding
// out what a magic number is.
//
// Usage: make_corpus <corpus directory>
//
// The files go to <corpus directory>/<target>/<seed name>. Nothing is committed: the seeds are made
// at build time, so they cannot differ from what the writers produce today. The seeds are built in
// a MemEnv and written to the disk once each, from the finished string.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/version_edit.h"
#include "db_ops_program.h"
#include "io/block_builder.h"
#include "io/env_util.h"
#include "io/log_writer.h"
#include "io/table_builder.h"
#include "shale/env.h"
#include "shale/status.h"
#include "util/dbformat.h"
#include "util/random.h"

namespace shale {
namespace {

// Stops the program with a message: a corpus that is not there is a build that failed.
void Require(const Status& s, const std::string& what) {
  if (!s.ok()) {
    std::fprintf(stderr, "make_corpus: %s: %s\n", what.c_str(), s.ToString().c_str());
    std::exit(1);
  }
}

std::string InternalKey(const std::string& user_key, SequenceNumber sequence, ValueType type) {
  std::string key;
  AppendInternalKey(&key, ParsedInternalKey{user_key, sequence, type});
  return key;
}

std::string NumberedKey(int i) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "key%04d", i);
  return buffer;
}

// Writes seeds into one target's directory.
class SeedWriter {
 public:
  SeedWriter(const std::string& corpus_dir, const std::string& target)
      : dir_(corpus_dir + "/" + target) {
    Require(Env::Default()->CreateDir(corpus_dir), "create " + corpus_dir);
    Require(Env::Default()->CreateDir(dir_), "create " + dir_);
  }

  void Write(const std::string& name, std::string_view bytes) const {
    const std::string path = dir_ + "/" + name;
    Require(WriteStringToFile(Env::Default(), bytes, path, /*sync=*/false), "write " + path);
  }

 private:
  const std::string dir_;
};

// A write-ahead log holding `records`, as a Writer lays it out.
std::string LogFile(const std::vector<std::string>& records) {
  const std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  Require(env->NewWritableFile("/seed.log", &file), "create a log");
  log::Writer writer(file.get());
  for (const std::string& record : records) {
    Require(writer.AddRecord(record), "append a record");
  }
  Require(file->Close(), "close the log");
  std::string bytes;
  Require(ReadFileToString(env.get(), "/seed.log", &bytes), "read the log back");
  return bytes;
}

void SeedLogReader(const std::string& corpus_dir) {
  const SeedWriter out(corpus_dir, "fuzz_log_reader");
  out.Write("records", LogFile({"first", "", std::string(100, 'x'), "last record"}));
  // A record that does not fit a block is cut into FIRST, MIDDLE and LAST fragments, and one that
  // ends a block leaves padding: the cases that the reader's bookkeeping is for.
  out.Write("fragmented", LogFile({std::string(100, 'a'), std::string(70000, 'b'),
                                   std::string(32768 - 7 - 7, 'c'), "tail"}));
  out.Write("empty", "");
}

// A block of internal keys, as a data block holds, and one of short plain keys.
void SeedBlock(const std::string& corpus_dir) {
  const SeedWriter out(corpus_dir, "fuzz_block");
  BlockBuilder internal(4);
  for (int i = 0; i < 24; i++) {
    internal.Add(InternalKey(NumberedKey(i), 100 - static_cast<SequenceNumber>(i),
                             i % 5 == 4 ? ValueType::kDeletion : ValueType::kValue),
                 i % 5 == 4 ? "" : "value-" + std::to_string(i));
  }
  out.Write("internal_keys", internal.Finish());

  BlockBuilder plain(2);
  for (const char* key : {"a", "ab", "abc", "b", "ba", "c"}) {
    plain.Add(key, std::string("v") + key);
  }
  out.Write("plain_keys", plain.Finish());
}

// A table of `entries` internal keys, as TableBuilder writes it.
std::string TableFile(const TableBuilderOptions& options,
                      const std::vector<std::pair<std::string, std::string>>& entries) {
  const std::unique_ptr<Env> env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  Require(env->NewWritableFile("/seed.sst", &file), "create a table");
  TableBuilder builder(options, file.get());
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  Require(builder.Finish(), "finish the table");
  Require(file->Close(), "close the table");
  std::string bytes;
  Require(ReadFileToString(env.get(), "/seed.sst", &bytes), "read the table back");
  return bytes;
}

void SeedTable(const std::string& corpus_dir) {
  const SeedWriter out(corpus_dir, "fuzz_table");

  // Several data blocks, a deletion, a user key with two versions, and a filter.
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 40; i++) {
    const std::string user_key = NumberedKey(i);
    if (i % 7 == 3) {
      entries.emplace_back(
          InternalKey(user_key, 200 + static_cast<SequenceNumber>(i), ValueType::kValue),
          "new-" + std::to_string(i));
      entries.emplace_back(
          InternalKey(user_key, 100 + static_cast<SequenceNumber>(i), ValueType::kValue),
          "old-" + std::to_string(i));
    } else if (i % 11 == 5) {
      entries.emplace_back(
          InternalKey(user_key, 100 + static_cast<SequenceNumber>(i), ValueType::kDeletion), "");
    } else {
      entries.emplace_back(
          InternalKey(user_key, 100 + static_cast<SequenceNumber>(i), ValueType::kValue),
          "value-" + std::to_string(i));
    }
  }
  TableBuilderOptions small_blocks;
  small_blocks.block_size = 256;
  small_blocks.block_restart_interval = 4;
  out.Write("blocks_and_filter", TableFile(small_blocks, entries));

  TableBuilderOptions no_filter;
  no_filter.block_size = 256;
  no_filter.bloom_bits_per_key = 0;
  out.Write("no_filter", TableFile(no_filter, entries));

  out.Write("empty_table", TableFile(TableBuilderOptions(), {}));
}

void SeedVersionEdit(const std::string& corpus_dir) {
  const SeedWriter out(corpus_dir, "fuzz_version_edit");

  VersionEdit full;
  full.log_number = 7;
  full.next_file_number = 12;
  full.last_sequence = 1234;
  full.compact_pointers.emplace_back(1, InternalKey("key0010", 90, ValueType::kValue));
  full.compact_pointers.emplace_back(3, InternalKey("key0200", 5, ValueType::kDeletion));
  full.deleted_files.insert({0, 3});
  full.deleted_files.insert({1, 5});
  FileMetaData a;
  a.number = 9;
  a.file_size = 4096;
  a.smallest = InternalKey("key0001", 80, ValueType::kValue);
  a.largest = InternalKey("key0099", 12, ValueType::kValue);
  FileMetaData b;
  b.number = 10;
  b.file_size = uint64_t{1} << 33;
  b.smallest = InternalKey("a", 1, ValueType::kDeletion);
  b.largest = InternalKey("zzzz", kMaxSequenceNumber, ValueType::kValue);
  full.new_files.emplace_back(0, a);
  full.new_files.emplace_back(2, b);
  std::string encoded;
  full.EncodeTo(&encoded);
  out.Write("every_field", encoded);

  VersionEdit counters;
  counters.last_sequence = 1;
  std::string counters_encoded;
  counters.EncodeTo(&counters_encoded);
  out.Write("counters_only", counters_encoded);

  out.Write("empty_edit", "");
}

// Appends one operation to `program`.
void Emit(std::string* program, fuzz::DbOp op, std::initializer_list<unsigned> operands = {}) {
  program->push_back(static_cast<char>(static_cast<unsigned>(op)));
  for (const unsigned operand : operands) {
    program->push_back(static_cast<char>(operand));
  }
}

void SeedDbOps(const std::string& corpus_dir) {
  const SeedWriter out(corpus_dir, "fuzz_db_ops");
  using fuzz::DbOp;

  // One of each, so that a mutation starts from every operation in context.
  std::string tour;
  Emit(&tour, DbOp::kPut, {1, 10});
  Emit(&tour, DbOp::kPut, {2, 64});
  Emit(&tour, DbOp::kPut, {1, 0});
  Emit(&tour, DbOp::kGet, {1});
  Emit(&tour, DbOp::kFlush);
  Emit(&tour, DbOp::kDelete, {2});
  Emit(&tour, DbOp::kScan, {0});
  Emit(&tour, DbOp::kPut, {3, 30});
  Emit(&tour, DbOp::kFlush);
  Emit(&tour, DbOp::kCompact, {0, 15});
  Emit(&tour, DbOp::kGet, {2});
  Emit(&tour, DbOp::kScan, {2});
  out.Write("tour", tour);

  // Enough puts that a memtable of 16 KiB fills and the engine switches it without being asked.
  std::string fills;
  for (unsigned i = 0; i < 500; i++) {
    Emit(&fills, DbOp::kPut, {i % fuzz::kNumDbKeys, fuzz::kMaxDbValueLength});
  }
  Emit(&fills, DbOp::kScan, {0});
  out.Write("fills_memtable", fills);

  // A random mix, from a fixed seed: mostly writes, with a flush or a compaction now and then.
  Random random(1);
  std::string mixed;
  for (int i = 0; i < 900; i++) {
    const uint32_t pick = random.Uniform(100);
    const auto key = static_cast<unsigned>(random.Uniform(fuzz::kNumDbKeys));
    if (pick < 55) {
      Emit(&mixed, DbOp::kPut, {key, random.Uniform(fuzz::kMaxDbValueLength + 1)});
    } else if (pick < 70) {
      Emit(&mixed, DbOp::kDelete, {key});
    } else if (pick < 85) {
      Emit(&mixed, DbOp::kGet, {key});
    } else if (pick < 90) {
      Emit(&mixed, DbOp::kScan, {key});
    } else if (pick < 97) {
      Emit(&mixed, DbOp::kFlush);
    } else {
      Emit(&mixed, DbOp::kCompact, {key, random.Uniform(fuzz::kNumDbKeys)});
    }
  }
  out.Write("mixed", mixed);
}

}  // namespace
}  // namespace shale

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: make_corpus <corpus directory>\n");
    return 2;
  }
  const std::string corpus_dir = argv[1];
  shale::SeedLogReader(corpus_dir);
  shale::SeedBlock(corpus_dir);
  shale::SeedTable(corpus_dir);
  shale::SeedVersionEdit(corpus_dir);
  shale::SeedDbOps(corpus_dir);
  return 0;
}
