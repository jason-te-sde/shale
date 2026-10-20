// shalectl: the command-line tool for a shale database. It reads and writes keys, shows the
// statistics, checks a database offline, and dumps the files the engine writes in a form a person
// can read. Run it with no arguments for the usage.

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "cli_args.h"
#include "core/version_edit.h"
#include "db/named_status.h"
#include "io/log_reader.h"
#include "io/table.h"
#include "io/write_batch_internal.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "shale/write_batch.h"
#include "util/dbformat.h"
#include "verify.h"

namespace shale {
namespace {

// The exit statuses.
constexpr int kExitOk = 0;
constexpr int kExitNotFound = 1;  // `get` found no such key
constexpr int kExitUsage = 2;     // the command line is wrong
constexpr int kExitEngine = 3;    // the engine, or a file it reads, reported an error

constexpr char kUsage[] =
    "usage: shalectl --db=PATH [--create] put KEY VALUE\n"
    "       shalectl --db=PATH [--create] get KEY\n"
    "       shalectl --db=PATH [--create] delete KEY\n"
    "       shalectl --db=PATH [--create] scan [--from=KEY] [--limit=N]\n"
    "       shalectl --db=PATH [--create] stats\n"
    "       shalectl --db=PATH [--create] flush\n"
    "       shalectl --db=PATH verify\n"
    "       shalectl dump-sst FILE\n"
    "       shalectl dump-wal FILE\n"
    "       shalectl dump-manifest FILE\n"
    "\n"
    "get prints the value; scan prints a line of key, a tab and value for each key from --from,\n"
    "at most --limit of them; stats prints the database's statistics. Every key and value is\n"
    "shown as printable ASCII: a backslash as \\\\ and any byte that is not printable as \\xNN.\n"
    "A key or value that starts with -- is given after a lone --. A binary key can be given\n"
    "with the shell's $'\\xNN' quoting; a NUL byte cannot be given on the command line.\n"
    "--create makes the database if there is none. Flags may come before the command or after\n"
    "it.\n"
    "\n"
    "Every command except verify and the dumps opens the database for writing: it replays the\n"
    "write-ahead log into a table, writes a new MANIFEST and deletes the files that are no\n"
    "longer needed. To look at a database that must not be touched, run verify and the dumps\n"
    "on a copy of it.\n"
    "\n"
    "flush is a maintenance command. Opening a database turns the write-ahead log it finds\n"
    "into a table, and flush then writes out whatever the memtable holds, so that nothing is\n"
    "left in a log.\n"
    "\n"
    "verify changes no byte of what the database holds, but it is not read-only. It takes the\n"
    "LOCK, so it refuses a database that is open; it creates an empty LOCK if the directory has\n"
    "none; it needs write access to the directory, so it cannot check read-only media. It reads\n"
    "the MANIFEST and every table with checksums on and does not read the write-ahead logs\n"
    "(dump-wal does). It prints ok, or a line for each problem found on standard output and\n"
    "exits with status 3. A failure to take the lock or to read at all is also exit status 3,\n"
    "with the reason on standard error and nothing on standard output.\n"
    "\n"
    "dump-sst, dump-wal and dump-manifest print one entry of a table, a write-ahead log or a\n"
    "MANIFEST on each line, whatever database the file is from.\n"
    "\n"
    "exit status: 0 success, 1 key not found (get), 2 usage error, 3 engine error\n";

void Print(std::string_view text) { std::fwrite(text.data(), 1, text.size(), stdout); }

// Prints a line to standard error. What was printed to standard output is flushed first, so that on
// a terminal, or one stream shared by both, the lines of a result come before the error about it.
void PrintError(std::string_view text) {
  std::fflush(stdout);
  std::fputs("shalectl: ", stderr);
  std::fwrite(text.data(), 1, text.size(), stderr);
  std::fputc('\n', stderr);
}

int UsageError(std::string_view why) {
  PrintError(why);
  std::fputs(kUsage, stderr);
  return kExitUsage;
}

int EngineError(const Status& status) {
  PrintError(status.ToString());
  return kExitEngine;
}

// The flags that take a value. ParseCliArgs() stores a bare flag as "true", which would be taken
// for the path of a database or the first key of a scan, and the usual way to give one is to write
// the value after a space, which makes it a bare flag and the value a word; so a bare one is
// refused before the command runs.
constexpr std::array<std::string_view, 3> kFlagsWithValues = {"--db", "--from", "--limit"};

// The first word that is one of kFlagsWithValues spelled with no value. The search stops at the
// first lone `--`, as ParseCliArgs() stops reading flags there: what follows it is a key or a
// value, and one that is spelled `--db` is that and not a flag.
std::optional<std::string> FindFlagWithoutValue(const std::vector<std::string>& words) {
  for (const std::string& word : words) {
    if (word == "--") {
      break;
    }
    for (const std::string_view flag : kFlagsWithValues) {
      if (word == flag) {
        return word;
      }
    }
  }
  return std::nullopt;
}

// Reads --db into `*path`. Returns kExitOk, or kExitUsage, reported, if it is missing or empty: an
// empty path would be a database in no directory at all, and is a slip of the shell, as in
// --db=$DB with DB not set.
int DatabasePath(const CliArgs& args, std::string* path) {
  const auto flag = args.flags.find("db");
  if (flag == args.flags.end()) {
    return UsageError("--db=PATH is needed to say which database");
  }
  if (flag->second.empty()) {
    return UsageError("--db needs a path, and it is empty");
  }
  *path = flag->second;
  return kExitOk;
}

// Opens the database that --db names, with --create deciding whether to make it. Returns kExitOk,
// or the exit status of the failure, which has been reported.
int OpenDatabase(const CliArgs& args, std::unique_ptr<DB>* db) {
  std::string path;
  if (const int code = DatabasePath(args, &path); code != kExitOk) {
    return code;
  }
  Options options;
  const auto create = args.flags.find("create");
  if (create != args.flags.end()) {
    if (create->second != "true" && create->second != "false") {
      return UsageError("--create is true or false, not '" + EscapeBytes(create->second) + "'");
    }
    options.create_if_missing = create->second == "true";
  }
  const Status s = DB::Open(options, path, db);
  if (!s.ok()) {
    return EngineError(s);
  }
  return kExitOk;
}

int RunPut(const CliArgs& args) {
  std::unique_ptr<DB> db;
  if (const int code = OpenDatabase(args, &db); code != kExitOk) {
    return code;
  }
  const Status s = db->Put(WriteOptions(), args.positional[0], args.positional[1]);
  return s.ok() ? kExitOk : EngineError(s);
}

int RunGet(const CliArgs& args) {
  std::unique_ptr<DB> db;
  if (const int code = OpenDatabase(args, &db); code != kExitOk) {
    return code;
  }
  std::string value;
  const Status s = db->Get(ReadOptions(), args.positional[0], &value);
  if (s.IsNotFound()) {
    PrintError("not found");
    return kExitNotFound;
  }
  if (!s.ok()) {
    return EngineError(s);
  }
  Print(EscapeBytes(value) + "\n");
  return kExitOk;
}

int RunDelete(const CliArgs& args) {
  std::unique_ptr<DB> db;
  if (const int code = OpenDatabase(args, &db); code != kExitOk) {
    return code;
  }
  const Status s = db->Delete(WriteOptions(), args.positional[0]);
  return s.ok() ? kExitOk : EngineError(s);
}

// Reads --limit: a decimal number, with nothing else in it. nullopt, with `*valid` false, if the
// flag is there and is not one.
std::optional<uint64_t> ParseLimit(const CliArgs& args, bool* valid) {
  *valid = true;
  const auto flag = args.flags.find("limit");
  if (flag == args.flags.end()) {
    return std::nullopt;
  }
  const std::string& text = flag->second;
  uint64_t limit = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), limit);
  if (error != std::errc() || end != text.data() + text.size()) {
    *valid = false;
    return std::nullopt;
  }
  return limit;
}

int RunScan(const CliArgs& args) {
  bool limit_valid = false;
  const std::optional<uint64_t> limit = ParseLimit(args, &limit_valid);
  if (!limit_valid) {
    return UsageError("--limit is a whole number, not '" + EscapeBytes(args.flags.at("limit")) +
                      "'");
  }
  std::unique_ptr<DB> db;
  if (const int code = OpenDatabase(args, &db); code != kExitOk) {
    return code;
  }
  ReadOptions options;
  options.fill_cache = false;  // a scan of everything must not push out what reads want
  // The iterator is destroyed before the database, which aborts if one is still alive.
  Status s;
  {
    const std::unique_ptr<Iterator> iter = db->NewIterator(options);
    const auto from = args.flags.find("from");
    if (from != args.flags.end()) {
      iter->Seek(from->second);
    } else {
      iter->SeekToFirst();
    }
    uint64_t shown = 0;
    while (iter->Valid() && (!limit.has_value() || shown < *limit)) {
      Print(EscapeBytes(iter->key()) + "\t" + EscapeBytes(iter->value()) + "\n");
      iter->Next();
      shown++;
    }
    s = iter->status();
  }
  return s.ok() ? kExitOk : EngineError(s);
}

int RunStats(const CliArgs& args) {
  std::unique_ptr<DB> db;
  if (const int code = OpenDatabase(args, &db); code != kExitOk) {
    return code;
  }
  std::string stats;
  if (!db->GetProperty("shale.stats", &stats)) {
    return EngineError(Status::NotSupported("the database has no shale.stats property"));
  }
  Print(stats);
  if (stats.empty() || stats.back() != '\n') {
    Print("\n");
  }
  return kExitOk;
}

int RunFlush(const CliArgs& args) {
  std::unique_ptr<DB> db;
  if (const int code = OpenDatabase(args, &db); code != kExitOk) {
    return code;
  }
  const Status s = db->Flush();
  return s.ok() ? kExitOk : EngineError(s);
}

int RunVerify(const CliArgs& args) {
  std::string path;
  if (const int code = DatabasePath(args, &path); code != kExitOk) {
    return code;
  }
  std::vector<std::string> problems;
  const Status s = VerifyDatabase(Env::Default(), path, &problems);
  if (!s.ok()) {
    return EngineError(s);
  }
  if (problems.empty()) {
    Print("ok\n");
    return kExitOk;
  }
  for (const std::string& problem : problems) {
    Print(problem + "\n");
  }
  PrintError("verify found " + std::to_string(problems.size()) + " problem(s)");
  return kExitEngine;
}

// ---- The dumps. They read one file, whatever database it belongs to, and take no flags. --------

int RunDumpSst(const CliArgs& args) {
  const std::string& path = args.positional[0];
  Env* const env = Env::Default();
  uint64_t size = 0;
  Status s = env->GetFileSize(path, &size);
  if (!s.ok()) {
    return EngineError(s);
  }
  std::unique_ptr<RandomAccessFile> file;
  s = env->NewRandomAccessFile(path, &file);
  if (!s.ok()) {
    return EngineError(s);
  }
  std::shared_ptr<Table> table;
  s = Table::Open(std::move(file), size, /*cache_id=*/0, /*cache=*/nullptr, &table);
  if (!s.ok()) {
    return EngineError(WithFileName(s, path));
  }
  TableReadOptions options;
  options.verify_checksums = true;
  options.fill_cache = false;
  const std::unique_ptr<Iterator> iter = table->NewIterator(options);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey key;
    if (!ParseInternalKey(iter->key(), &key)) {
      return EngineError(Status::Corruption("an entry's key is not an internal key", path));
    }
    Print(EscapeBytes(key.user_key) + " seq=" + std::to_string(key.sequence) +
          (key.type == ValueType::kValue ? " type=put" : " type=del") +
          " value=" + EscapeBytes(iter->value()) + "\n");
  }
  s = iter->status();
  return s.ok() ? kExitOk : EngineError(WithFileName(s, path));
}

// Tells the user of every defect the log reader finds, as it finds it. The reader goes on after a
// defect, so a damaged log is dumped as far as it can be read.
class PrintingReporter final : public log::Reader::Reporter {
 public:
  explicit PrintingReporter(std::string path) : path_(std::move(path)) {}

  void Corruption(size_t bytes, const Status& status) override {
    PrintError(path_ + ": " + std::to_string(bytes) + " byte(s) dropped: " + status.ToString());
    damaged_ = true;
  }

  // Whether the reader has reported a defect.
  bool damaged() const { return damaged_; }

 private:
  const std::string path_;
  bool damaged_ = false;
};

// Reads every record of the log-format file at `path` — a write-ahead log or a MANIFEST — and
// hands each to `on_record`, which prints it and returns the Status of a record that is not what
// it should be. Returns kExitOk if every record was whole and the file had no defect, and
// kExitEngine if not: each defect has been reported, and the records that can be read have all
// been shown. A file that ends as a crash leaves it, in a record cut short or in zeros from a
// record boundary to its end, has no defect; the zeros are said on standard error, as the LOG of an
// open says them (D6).
int DumpLog(const std::string& path,
            const std::function<Status(std::string_view record)>& on_record) {
  std::unique_ptr<SequentialFile> file;
  const Status opened = Env::Default()->NewSequentialFile(path, &file);
  if (!opened.ok()) {
    return EngineError(opened);
  }
  PrintingReporter reporter(path);
  log::Reader reader(file.get(), &reporter, /*checksum=*/true);
  bool records_whole = true;
  std::string_view record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    const Status s = on_record(record);
    if (!s.ok()) {
      PrintError(path + ": the record at offset " + std::to_string(reader.LastRecordOffset()) +
                 ": " + s.ToString());
      records_whole = false;
    }
  }
  if (reader.ZeroTailBytes() > 0) {
    PrintError(path + ": ends in " + std::to_string(reader.ZeroTailBytes()) +
               " zero byte(s), from a record boundary to the end of the file, which a crash leaves "
               "(D6): not records, and not damage");
  }
  return records_whole && !reporter.damaged() ? kExitOk : kExitEngine;
}

// Prints the operations of one write batch as `  put KEY VALUE` and `  del KEY`.
class BatchPrinter final : public WriteBatch::Handler {
 public:
  void Put(std::string_view key, std::string_view value) override {
    Print("  put " + EscapeBytes(key) + " " + EscapeBytes(value) + "\n");
  }

  void Delete(std::string_view key) override { Print("  del " + EscapeBytes(key) + "\n"); }
};

int RunDumpWal(const CliArgs& args) {
  return DumpLog(args.positional[0], [](std::string_view record) {
    WriteBatch batch;
    Status s = WriteBatchInternal::SetContents(&batch, record);
    if (!s.ok()) {
      return s;
    }
    Print("batch seq=" + std::to_string(WriteBatchInternal::Sequence(&batch)) +
          " count=" + std::to_string(WriteBatchInternal::Count(&batch)) + "\n");
    BatchPrinter printer;
    return batch.Iterate(&printer);
  });
}

int RunDumpManifest(const CliArgs& args) {
  return DumpLog(args.positional[0], [](std::string_view record) {
    VersionEdit edit;
    Status s = edit.DecodeFrom(record);
    if (!s.ok()) {
      return s;
    }
    Print(edit.DebugString() + "\n");
    return Status::OK();
  });
}

// ---- The commands, and what each one takes. ---------------------------------------------------

struct Command {
  std::string_view name;
  size_t positional;  // how many words follow the command
  // The flags the command takes, by name; the unused slots are empty. A flag that is not here is a
  // usage error, so that a mistyped one is not silently ignored.
  std::array<std::string_view, 4> flags;
  int (*run)(const CliArgs& args);
};

constexpr Command kCommands[] = {
    {"put", 2, {"db", "create"}, RunPut},
    {"get", 1, {"db", "create"}, RunGet},
    {"delete", 1, {"db", "create"}, RunDelete},
    {"scan", 0, {"db", "create", "from", "limit"}, RunScan},
    {"stats", 0, {"db", "create"}, RunStats},
    {"flush", 0, {"db", "create"}, RunFlush},
    {"verify", 0, {"db"}, RunVerify},
    {"dump-sst", 1, {}, RunDumpSst},
    {"dump-wal", 1, {}, RunDumpWal},
    {"dump-manifest", 1, {}, RunDumpManifest},
};

// Whether `command` takes the flag `name`.
bool TakesFlag(const Command& command, std::string_view name) {
  for (const std::string_view flag : command.flags) {
    if (!flag.empty() && flag == name) {
      return true;
    }
  }
  return false;
}

int Main(const std::vector<std::string>& words) {
  CliArgs args;
  const Status parsed = ParseCliArgs(words, &args);
  if (!parsed.ok()) {
    return UsageError(parsed.ToString());
  }
  if (const std::optional<std::string> bare = FindFlagWithoutValue(words); bare.has_value()) {
    return UsageError(*bare + " needs a value, written " + *bare + "=VALUE");
  }
  for (const Command& command : kCommands) {
    if (command.name != args.command) {
      continue;
    }
    for (const auto& [name, value] : args.flags) {
      if (!TakesFlag(command, name)) {
        return UsageError(EscapeBytes(args.command) + " does not take --" + EscapeBytes(name));
      }
    }
    if (args.positional.size() != command.positional) {
      return UsageError(args.command + " takes " + std::to_string(command.positional) +
                        " word(s) after it, and was given " +
                        std::to_string(args.positional.size()));
    }
    const int code = command.run(args);
    // Output that did not get written is a failure, and a command that says OK after losing its
    // output is worse than one that fails: flush, which can fail, and the stream's own error flag,
    // which a write that failed before this point left set.
    const bool flushed = std::fflush(stdout) == 0;
    if ((!flushed || std::ferror(stdout) != 0) && code == kExitOk) {
      PrintError("cannot write to standard output");
      return kExitEngine;
    }
    return code;
  }
  return UsageError("unknown command '" + EscapeBytes(args.command) + "'");
}

}  // namespace
}  // namespace shale

int main(int argc, char** argv) {
  try {
    std::vector<std::string> words;
    for (int i = 1; i < argc; i++) {
      words.emplace_back(argv[i]);
    }
    return shale::Main(words);
  } catch (const std::exception& e) {
    // The engine does not throw; the standard library does, for memory it cannot get.
    std::fprintf(stderr, "shalectl: %s\n", e.what());
    return shale::kExitEngine;
  }
}
