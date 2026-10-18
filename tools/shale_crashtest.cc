// shale_crashtest: kills a process that is writing to a database, and checks what is left.
//
// Every round forks a writer. The writer applies numbered operations to the database one after
// another: operation i puts a value, or one time in eight deletes, a key that the seed and i
// choose out of a small set of keys, so that the keys are written again and again and every
// memtable that is flushed spans the whole set. Each time a call returns OK the writer sends the
// number of the operation to the parent through a pipe. That is an acknowledgement: the database
// has said the operation is done. After a random time the parent sends the writer SIGKILL, reads
// every acknowledgement, opens the database again and checks it against a model. Each key must
// hold what the last acknowledged operation on it left, a value or nothing; the one operation that
// may have been in flight at the kill has landed or has not, on its own key only; every other key
// is exactly as it was; and nothing else is in the database.
//
// This is the one test that runs the engine, the real filesystem and a real kill together. A
// process kill leaves the whole of the page cache, so it proves nothing about power loss: that is
// the simulator's job, over a filesystem that can lose what was not synced. What it can show is
// that a kill, at any point of a write, a flush or a compaction, leaves a database that opens and
// holds everything it said it did. The parent watches the directory while the writer runs and
// counts the tables that appear and the tables that are removed, so that a run says whether the
// kills reached flushes and compactions at all. Run it with no arguments for the usage.

#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cli_args.h"
#include "shale/db.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/options.h"
#include "shale/status.h"
#include "util/coding.h"
#include "util/filename.h"
#include "util/random.h"

namespace shale {
namespace {

// The exit statuses.
constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;   // the database lost or changed something, or the writer broke
constexpr int kExitUsage = 2;    // the command line is wrong
constexpr int kExitHarness = 3;  // the test itself could not run, or did not reach what it is for

constexpr char kUsage[] =
    "usage: shale_crashtest --db=PATH --rounds=N --seed=S [--min_ms=50] [--max_ms=500]\n"
    "                       [--sync] [--keys=4096] [--min_compactions=0] [--self_test_damage]\n"
    "\n"
    "Each of N rounds forks a writer that applies numbered operations to the database at PATH: an\n"
    "operation puts a value, or one time in eight deletes, a key that the seed and its number\n"
    "choose out of --keys keys, and the writer reports the number when the call returns OK. After\n"
    "a random time in [min_ms, max_ms] the parent kills the writer with SIGKILL, opens the\n"
    "database and checks it: each key must hold what the last reported operation on it left, the\n"
    "one operation in flight may or may not have landed, and nothing else may be there. PATH must\n"
    "not hold a database yet; it is created.\n"
    "\n"
    "The seed fixes the delays before the kills, the keys and the operations, but not how far the\n"
    "engine has got when a kill lands, so a run is not repeated by its seed. A failed run leaves\n"
    "the database where it is, and that is what to look at.\n"
    "\n"
    "--sync makes the writer's calls sync. A kill of the process cannot tell the difference, "
    "since\n"
    "the page cache survives it, so by default they do not: the writer is then about fifty times\n"
    "faster, and fills memtables, flushes them and compacts under the kill. --sync is for putting\n"
    "the fsync calls in the path as well.\n"
    "\n"
    "While it waits for a kill the parent lists the directory, and counts the tables that appear\n"
    "and the tables that are removed while a writer runs. A table is removed after a compaction\n"
    "that merged it. --min_compactions=N makes a run that saw fewer than N tables removed exit\n"
    "with status 3, as a run that did not reach what it is for.\n"
    "\n"
    "--self_test_damage overwrites one key that holds a value with a wrong one, through the\n"
    "database, after the first kill that had one and before the check. The run must then name "
    "that\n"
    "key and exit with status 1: it shows that the check can fail.\n"
    "\n"
    "exit status: 0 every round passed, 1 a check failed, 2 usage error, 3 the test could not run\n"
    "or did not reach what it is for\n";

// The size of a value, and of an acknowledgement, in bytes.
constexpr size_t kValueSize = 100;
constexpr size_t kAckSize = 8;

// How many problems of a round are printed; the rest are counted.
constexpr size_t kMaxShown = 10;

// How often the parent lists the directory while it waits, in milliseconds.
constexpr uint64_t kSampleMs = 5;

// The statuses the writer ends with when it ends by itself, which it only does when something is
// wrong. They are far from the 0 and 1 that a process can end with for other reasons.
constexpr int kWriterOpenFailed = 70;
constexpr int kWriterPutFailed = 71;
constexpr int kWriterPipeFailed = 72;

// The longest wait that --max_ms may ask for: an hour, which a round is never meant to take.
constexpr uint64_t kMaxDelayMs = 3'600'000;

// The most keys that --keys may ask for.
constexpr uint64_t kMaxKeys = 1'000'000;

struct Config {
  std::string db;
  uint64_t rounds = 0;
  uint64_t seed = 0;
  uint64_t min_ms = 50;
  uint64_t max_ms = 500;
  // How many keys the operations write to. With the 64 KB memtable and the 100 byte values a
  // memtable takes about five hundred operations, so a few thousand keys make the tables that a
  // flush writes overlap each other and the tables of the level below, which a compaction must
  // then merge and remove. Too many keys and the writes seldom meet each other again.
  uint64_t keys = 4096;
  uint64_t min_compactions = 0;
  bool sync = false;
  bool self_test_damage = false;
};

Status Errno(std::string_view what) { return Status::IOError(what, std::strerror(errno)); }

// ---- What is written. -------------------------------------------------------------------------

std::string Key(uint64_t index) {
  std::array<char, 32> text{};
  std::snprintf(text.data(), text.size(), "key%010llu", static_cast<unsigned long long>(index));
  return text.data();
}

// The index of the key that `name` is the name of, or nothing if it is not one of the keys of the
// run.
std::optional<uint64_t> ParseKey(const Config& config, std::string_view name) {
  uint64_t index = 0;
  if (!name.starts_with("key")) {
    return std::nullopt;
  }
  const std::string_view digits = name.substr(3);
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
  if (error != std::errc() || end != digits.data() + digits.size() || index >= config.keys ||
      Key(index) != name) {
    return std::nullopt;
  }
  return index;
}

// The value that operation `operation` of round `round` puts: "<operation>-<round>-" padded to
// kValueSize with bytes that depend only on the two numbers, so that a value that belongs to
// another operation or another round is not taken for this one.
std::string Value(uint64_t operation, uint64_t round) {
  std::string value = std::to_string(operation) + "-" + std::to_string(round) + "-";
  Random padding((operation * 0x9e3779b97f4a7c15ULL) ^ round);
  while (value.size() < kValueSize) {
    value.push_back(static_cast<char>('a' + padding.Uniform(26)));
  }
  return value;
}

// What an operation does to the database: the key, and whether it is a delete rather than a put.
struct Operation {
  uint64_t key = 0;
  bool is_delete = false;
};

// Operation number `operation`. It depends on the seed and the number alone, so that the writer
// and the parent agree on every operation without being told.
Operation OperationAt(const Config& config, uint64_t operation) {
  Random draw(config.seed ^ (operation * 0x9e3779b97f4a7c15ULL));
  Operation chosen;
  chosen.key = draw.Next() % config.keys;
  chosen.is_delete = draw.OneIn(8);
  return chosen;
}

// The database's options. A small write buffer and small tables make a writer that is not slowed by
// syncs fill a memtable, flush it and pile tables up for a compaction every few hundred
// operations. Only a run that has acknowledged nothing yet may create the database: one that has
// vanished since is an error to report, not to replace.
Options DatabaseOptions(bool create) {
  Options options;
  options.create_if_missing = create;
  options.write_buffer_size = 65536;
  options.target_file_size = 65536;
  return options;
}

// ---- The writer: the child process. -----------------------------------------------------------

bool WriteAll(int fd, const char* data, size_t size) {
  while (size > 0) {
    const ssize_t written = ::write(fd, data, size);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    data += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

// Applies operations from `first` on and acknowledges each, until it is killed. It ends itself only
// if something fails, and says what on standard error.
[[noreturn]] void WriteUntilKilled(const Config& config, int ack_fd, uint64_t first,
                                   uint64_t round) {
  std::unique_ptr<DB> db;
  const Status opened = DB::Open(DatabaseOptions(/*create=*/first == 0), config.db, &db);
  if (!opened.ok()) {
    std::fprintf(stderr, "shale_crashtest: the writer could not open the database: %s\n",
                 opened.ToString().c_str());
    _exit(kWriterOpenFailed);
  }
  WriteOptions write_options;
  write_options.sync = config.sync;
  for (uint64_t i = first;; i++) {
    const Operation operation = OperationAt(config, i);
    const std::string key = Key(operation.key);
    const Status done = operation.is_delete ? db->Delete(write_options, key)
                                            : db->Put(write_options, key, Value(i, round));
    if (!done.ok()) {
      std::fprintf(stderr, "shale_crashtest: the writer's operation %llu on %s failed: %s\n",
                   static_cast<unsigned long long>(i), key.c_str(), done.ToString().c_str());
      _exit(kWriterPutFailed);
    }
    std::array<char, kAckSize> ack{};
    EncodeFixed64(ack.data(), i);
    if (!WriteAll(ack_fd, ack.data(), ack.size())) {
      std::fprintf(stderr, "shale_crashtest: the writer could not report operation %llu: %s\n",
                   static_cast<unsigned long long>(i), std::strerror(errno));
      _exit(kWriterPipeFailed);
    }
  }
}

// The body of the writer process. It leaves with _exit(): the parent's buffers and destructors are
// not its to run, and an exception must not unwind into the frames that were copied from the
// parent's stack.
[[noreturn]] void RunWriter(const Config& config, int ack_fd, uint64_t first, uint64_t round) {
  try {
    WriteUntilKilled(config, ack_fd, first, round);
  } catch (...) {
    std::fprintf(stderr, "shale_crashtest: the writer threw an exception\n");
    _exit(kWriterPutFailed);
  }
}

// ---- The parent's side of the pipe. -----------------------------------------------------------

// A file descriptor that is closed when it goes out of scope. A failure to close matters where
// Close() is called and is ignored in the destructor, which only runs on the way out of a failure.
class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  int get() const { return fd_; }

  Status Close() {
    const int fd = fd_;
    fd_ = -1;
    if (fd >= 0 && ::close(fd) != 0) {
      return Errno("close");
    }
    return Status::OK();
  }

 private:
  int fd_;
};

// The writer process. If it is still alive when this goes out of scope, which is on a failure of
// the test, it is killed and collected, so that no writer outlives its round and goes on writing.
class Writer {
 public:
  explicit Writer(pid_t pid) : pid_(pid) {}
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  ~Writer() {
    // The results are ignored: this runs only on the way out of a failure that is being reported.
    if (!reaped_) {
      ::kill(pid_, SIGKILL);
      int ignored = 0;
      ::waitpid(pid_, &ignored, 0);
    }
  }

  // Sends SIGKILL, waits for the process to be gone and stores how it ended in `*wait_status`. A
  // writer that has ended by itself is a zombie that the signal still reaches, and one that the
  // system no longer knows (ESRCH) is collected below all the same.
  Status KillAndReap(int* wait_status) {
    if (::kill(pid_, SIGKILL) != 0 && errno != ESRCH) {
      return Errno("kill");
    }
    for (;;) {
      const pid_t done = ::waitpid(pid_, wait_status, 0);
      if (done == pid_) {
        reaped_ = true;
        return Status::OK();
      }
      if (done < 0 && errno == EINTR) {
        continue;
      }
      return Errno("waitpid");
    }
  }

 private:
  const pid_t pid_;
  bool reaped_ = false;
};

// The acknowledgements read so far. One is eight whole bytes, the number of an operation, and the
// operations come one after the other from `first`. Bytes that do not make eight are not an
// acknowledgement and are never counted as one: the writer may be killed in the middle of a write.
class Acks {
 public:
  explicit Acks(uint64_t first) : first_(first), next_(first) {}

  // Takes the bytes just read. Returns an error if an acknowledgement is not the operation after
  // the one before it, which means the writer and this reader disagree about the protocol, and
  // nothing read in this round can be trusted.
  Status Add(std::string_view bytes) {
    pending_.append(bytes);
    size_t used = 0;
    while (pending_.size() - used >= kAckSize) {
      const uint64_t operation = DecodeFixed64(pending_.data() + used);
      if (operation != next_) {
        return Status::Corruption("the writer acknowledged operation " + std::to_string(operation) +
                                  " when operation " + std::to_string(next_) + " was next");
      }
      next_++;
      used += kAckSize;
    }
    pending_.erase(0, used);
    return Status::OK();
  }

  // How many operations were acknowledged: first, first + 1, ... first + count() - 1.
  uint64_t count() const { return next_ - first_; }

  // How many bytes of an acknowledgement that did not come whole were read, and so were dropped.
  size_t torn_bytes() const { return pending_.size(); }

 private:
  const uint64_t first_;
  uint64_t next_;
  std::string pending_;
};

// ---- What the directory shows. ----------------------------------------------------------------

// The table files of the database directory, seen by listing it now and then. A table that is
// there in one listing and gone in the next was removed, and a table is removed only after a
// compaction has merged it into others. A table that is in a listing and was not in the one before
// was written by a flush or by a compaction. Tables that come and go between two listings are not
// seen, so the counts are least counts, which is the safe side for a claim that the kills reached
// them.
class TableWatch {
 public:
  TableWatch(Env* env, std::string dbname) : env_(env), dbname_(std::move(dbname)) {}

  // Lists the directory and counts what changed since the listing before.
  Status Sample() { return Take(/*count=*/true); }

  // Lists the directory and takes it as the state to compare with, without counting what changed:
  // for the changes that the parent's own opening of the database made.
  Status Absorb() { return Take(/*count=*/false); }

  uint64_t written() const { return written_; }
  uint64_t removed() const { return removed_; }

 private:
  Status Take(bool count) {
    std::set<uint64_t> now;
    if (env_->FileExists(dbname_)) {
      std::vector<std::string> names;
      if (const Status listed = env_->GetChildren(dbname_, &names); !listed.ok()) {
        return listed;
      }
      for (const std::string& name : names) {
        uint64_t number = 0;
        FileType type = FileType::kCurrentFile;
        if (ParseFileName(name, &number, &type) && type == FileType::kTableFile) {
          now.insert(number);
        }
      }
    }
    if (count) {
      for (const uint64_t number : now) {
        if (alive_.count(number) == 0) {
          written_++;
        }
      }
      for (const uint64_t number : alive_) {
        if (now.count(number) == 0) {
          removed_++;
        }
      }
    }
    alive_ = std::move(now);
    return Status::OK();
  }

  Env* const env_;
  const std::string dbname_;
  std::set<uint64_t> alive_;
  uint64_t written_ = 0;
  uint64_t removed_ = 0;
};

// Reads the acknowledgements from `fd` for `delay_ms`, or until the writer's end of the pipe is
// closed, which only a writer that has ended does. The pipe is read while waiting, so that the
// writer is never held up by a full pipe, and the directory is listed every few milliseconds.
Status CollectAcks(Env* env, int fd, uint64_t delay_ms, Acks* acks, TableWatch* watch) {
  const uint64_t deadline = env->NowMicros() + delay_ms * 1000;
  uint64_t last_sample = env->NowMicros();
  std::array<char, 4096> buffer{};
  for (;;) {
    const uint64_t now = env->NowMicros();
    if (now >= deadline) {
      return Status::OK();
    }
    if (now - last_sample >= kSampleMs * 1000) {
      if (const Status sampled = watch->Sample(); !sampled.ok()) {
        return sampled;
      }
      last_sample = now;
    }
    // Rounded up, so that a wait that is a fraction of a millisecond short is not a busy loop.
    const uint64_t remaining_ms = (deadline - now + 999) / 1000;
    pollfd wait{fd, POLLIN, 0};
    const int ready =
        ::poll(&wait, 1, static_cast<int>(std::min<uint64_t>(remaining_ms, kSampleMs)));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Errno("poll");
    }
    if (ready == 0) {
      continue;  // the clock decides whether the time is up
    }
    const ssize_t got = ::read(fd, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Errno("read");
    }
    if (got == 0) {
      return Status::OK();  // the writer has gone; the kill and the wait say how
    }
    if (const Status added = acks->Add(std::string_view(buffer.data(), static_cast<size_t>(got)));
        !added.ok()) {
      return added;
    }
  }
}

// Reads what is left in the pipe once the writer is dead, up to the end of it. Every
// acknowledgement that was written before the kill is there, and only those: nothing can be written
// any more.
Status DrainAcks(int fd, Acks* acks) {
  std::array<char, 4096> buffer{};
  for (;;) {
    const ssize_t got = ::read(fd, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Errno("read");
    }
    if (got == 0) {
      return Status::OK();
    }
    if (const Status added = acks->Add(std::string_view(buffer.data(), static_cast<size_t>(got)));
        !added.ok()) {
      return added;
    }
  }
}

// How the writer ended, from what waitpid() reported.
struct Ending {
  bool killed = false;  // by the SIGKILL that was sent, which is how it should end
  int exit_status = kExitFailed;
  std::string text;
};

Ending ClassifyEnding(int wait_status) {
  Ending ending;
  if (WIFSIGNALED(wait_status)) {
    const int signal_number = WTERMSIG(wait_status);
    if (signal_number == SIGKILL) {
      ending.killed = true;
    } else {
      ending.text = std::string("was ended by signal ") + std::to_string(signal_number) + " (" +
                    strsignal(signal_number) + ") before it was killed";
    }
  } else if (WIFEXITED(wait_status)) {
    const int status = WEXITSTATUS(wait_status);
    if (status == kWriterOpenFailed) {
      ending.text = "could not open the database";
    } else if (status == kWriterPutFailed) {
      ending.text = "stopped because an operation failed";
    } else if (status == kWriterPipeFailed) {
      ending.text = "stopped because it could not report to the parent";
      ending.exit_status = kExitHarness;
    } else {
      ending.text = "exited with status " + std::to_string(status) + " before it was killed";
    }
  } else {
    ending.text = "ended in a way waitpid() does not describe";
    ending.exit_status = kExitHarness;
  }
  return ending;
}

// ---- The model and the check. -----------------------------------------------------------------

// What the acknowledged operations left in one key: a value, put by `operation` of `round`, or
// nothing, because the key was never written or its last operation deleted it.
struct Slot {
  bool present = false;
  uint64_t operation = 0;
  uint64_t round = 0;
};

// The slot of every key, by index: what the database must hold.
using Model = std::vector<Slot>;

// What a read of a key gave: a value, or nothing.
using Seen = std::optional<std::string>;

// What operation number `operation` of round `round` leaves in its key.
Slot Leaves(const Operation& what, uint64_t operation, uint64_t round) {
  Slot slot;
  if (!what.is_delete) {
    slot.present = true;
    slot.operation = operation;
    slot.round = round;
  }
  return slot;
}

// Whether what was seen is what the slot holds.
bool Holds(const Slot& slot, const Seen& seen) {
  if (!slot.present) {
    return !seen.has_value();
  }
  return seen.has_value() && *seen == Value(slot.operation, slot.round);
}

// What a check found: the number of problems, the first few in words, and the index of every key
// that a problem names.
struct Findings {
  uint64_t count = 0;
  std::vector<std::string> shown;
  std::vector<uint64_t> keys;

  void Add(uint64_t key_index, const std::string& problem) {
    keys.push_back(key_index);
    AddOther(Key(key_index) + " " + problem);
  }

  void AddOther(const std::string& problem) {
    count++;
    if (shown.size() < kMaxShown) {
      shown.push_back(problem);
    }
  }

  bool Names(uint64_t key_index) const {
    return std::find(keys.begin(), keys.end(), key_index) != keys.end();
  }
};

// Bytes as a short quoted string for a message.
std::string Excerpt(std::string_view bytes) {
  constexpr size_t kShownBytes = 28;
  std::string text = EscapeBytes(bytes.substr(0, kShownBytes));
  if (bytes.size() > kShownBytes) {
    text += "...";
  }
  return "\"" + text + "\" (" + std::to_string(bytes.size()) + " bytes)";
}

// "<got>, not <wanted>", and the first byte at which they differ, which an excerpt of the start may
// not show.
std::string Mismatch(std::string_view got, std::string_view wanted) {
  const auto differ = std::mismatch(got.begin(), got.end(), wanted.begin(), wanted.end());
  return Excerpt(got) + ", not " + Excerpt(wanted) + ", first different at byte " +
         std::to_string(differ.first - got.begin());
}

// Compares what `source` (a Get, or the scan) saw in key `index` with what the acknowledged
// operations left there, `left`. The key of the operation in flight may hold what that operation
// leaves, `maybe`, as well. Sets `*landed` if that is what was seen and it is not also what `left`
// holds: an operation that changes nothing, a delete of a key that is not there, cannot be told
// from one that did not land, and is taken for one that did not.
void Judge(uint64_t index, const Slot& left, const Slot* maybe, const Seen& seen,
           const char* source, Findings* findings, bool* landed) {
  *landed = false;
  if (Holds(left, seen)) {
    return;
  }
  if (maybe != nullptr && Holds(*maybe, seen)) {
    *landed = true;
    return;
  }
  std::string problem;
  if (!seen.has_value()) {
    // Nothing was seen, and the slot holds a value: if it held none, the slot would hold what was
    // seen and have returned above.
    problem = "was left a value by operation " + std::to_string(left.operation) +
              ", which was acknowledged, and " + source + " says it is not there";
  } else if (left.present) {
    problem = std::string(source) + " returned " +
              Mismatch(*seen, Value(left.operation, left.round)) + ", the value that operation " +
              std::to_string(left.operation) + " left";
  } else {
    problem = std::string(source) + " returned " + Excerpt(*seen) +
              ", and the acknowledged operations left nothing in it";
  }
  if (maybe != nullptr) {
    problem += ", and it is not what the operation in flight would leave either";
  }
  findings->Add(index, problem);
}

// Checks the reopened database `db` against `model`, which holds what the acknowledged operations
// left, and the operation in flight, number `in_flight`, which may or may not have landed. Every
// key is read with Get, and then one scan of the whole database is compared with the model too. The
// two reads are different paths through the engine. Sets `*landed` if the operation in flight is
// seen to have landed.
void Check(DB* db, const Config& config, const Model& model, uint64_t in_flight, uint64_t round,
           Findings* findings, bool* landed) {
  const Operation flying = OperationAt(config, in_flight);
  const Slot flying_leaves = Leaves(flying, in_flight, round);

  bool landed_by_get = false;
  for (uint64_t index = 0; index < config.keys; index++) {
    std::string value;
    const Status s = db->Get(ReadOptions(), Key(index), &value);
    if (!s.ok() && !s.IsNotFound()) {
      findings->Add(index, "could not be read by Get: " + s.ToString());
      continue;
    }
    const Seen seen = s.ok() ? Seen(value) : std::nullopt;
    const bool is_flying = index == flying.key;
    bool here = false;
    Judge(index, model[index], is_flying ? &flying_leaves : nullptr, seen, "Get", findings, &here);
    landed_by_get = landed_by_get || (is_flying && here);
  }

  std::map<std::string, std::string> found;
  {
    const std::unique_ptr<Iterator> scan = db->NewIterator(ReadOptions());
    for (scan->SeekToFirst(); scan->Valid(); scan->Next()) {
      found.emplace(std::string(scan->key()), std::string(scan->value()));
    }
    if (const Status s = scan->status(); !s.ok()) {
      findings->AddOther("the scan stopped with an error: " + s.ToString());
    }
  }
  bool landed_by_scan = false;
  for (uint64_t index = 0; index < config.keys; index++) {
    const auto it = found.find(Key(index));
    const Seen seen = it == found.end() ? std::nullopt : Seen(it->second);
    const bool is_flying = index == flying.key;
    bool here = false;
    Judge(index, model[index], is_flying ? &flying_leaves : nullptr, seen, "the scan", findings,
          &here);
    landed_by_scan = landed_by_scan || (is_flying && here);
  }
  for (const auto& [name, value] : found) {
    if (!ParseKey(config, name).has_value()) {
      findings->AddOther("the scan found " + Excerpt(name) + ", which nothing wrote");
    }
  }
  if (landed_by_get != landed_by_scan) {
    findings->Add(flying.key, "is in one state for Get and in another for the scan");
  }
  *landed = landed_by_get;
}

// How many table files each level holds, as "L0=2 L1=5", the empty levels left out.
std::string LevelSummary(DB* db) {
  std::string summary;
  for (int level = 0; level < 7; level++) {
    std::string files;
    if (!db->GetProperty("shale.num-files-at-level" + std::to_string(level), &files) ||
        files == "0") {
      continue;
    }
    summary +=
        (summary.empty() ? "" : " ") + std::string("L") + std::to_string(level) + "=" + files;
  }
  return summary.empty() ? "none" : summary;
}

// ---- The rounds. ------------------------------------------------------------------------------

// What the rounds so far have written and measured.
struct Run {
  explicit Run(const Config& config, Env* env) : model(config.keys), watch(env, config.db) {}

  Model model;
  uint64_t next_operation = 0;  // where the next writer starts
  TableWatch watch;
  uint64_t acked = 0;
  uint64_t present = 0;
  uint64_t landed = 0;  // rounds whose operation in flight was in the database at the kill
  std::string levels = "none";
  std::optional<uint64_t> damaged;  // the index of the key --self_test_damage overwrote
};

// Says why the run stops, on standard error, after the lines already printed on standard output.
// Returns `exit_status`.
int Stop(const Config& config, uint64_t round, int exit_status, const std::string& why) {
  std::fflush(stdout);
  std::fprintf(stderr, "shale_crashtest: round %llu: %s\n(seed %llu, database in %s)\n",
               static_cast<unsigned long long>(round), why.c_str(),
               static_cast<unsigned long long>(config.seed), config.db.c_str());
  return exit_status;
}

// Prints the line of a round and flushes it, so that a run that hangs shows which round it hung in.
void PrintRound(uint64_t round, uint64_t acked, uint64_t present, const char* verdict) {
  std::printf("round %llu: acked %llu, present %llu, %s\n", static_cast<unsigned long long>(round),
              static_cast<unsigned long long>(acked), static_cast<unsigned long long>(present),
              verdict);
  std::fflush(stdout);
}

// Runs one round. Returns kExitOk if it passed and the run goes on, or the exit status to stop
// with.
int RunRound(const Config& config, Env* env, Random* random, uint64_t round, Run* run) {
  const uint64_t first = run->next_operation;

  // What the parent's own opening of the database did to the directory, last round, is not what a
  // writer did.
  if (const Status s = run->watch.Absorb(); !s.ok()) {
    return Stop(config, round, kExitHarness, s.ToString());
  }
  // What this process has buffered for standard output would be flushed a second time by a child
  // that left through exit(); the writer leaves through _exit(), but nothing is left to chance.
  if (std::fflush(stdout) != 0) {
    return Stop(config, round, kExitHarness, Errno("flush of standard output").ToString());
  }
  std::array<int, 2> ends{};
  if (::pipe(ends.data()) != 0) {
    return Stop(config, round, kExitHarness, Errno("pipe").ToString());
  }
  Fd read_end(ends[0]);
  Fd write_end(ends[1]);
  const pid_t pid = ::fork();
  if (pid < 0) {
    return Stop(config, round, kExitHarness, Errno("fork").ToString());
  }
  if (pid == 0) {
    if (::close(read_end.get()) != 0) {
      _exit(kWriterPipeFailed);
    }
    RunWriter(config, write_end.get(), first, round);
  }
  Writer writer(pid);
  if (const Status s = write_end.Close(); !s.ok()) {
    return Stop(config, round, kExitHarness, s.ToString());
  }

  const uint64_t spread = config.max_ms - config.min_ms + 1;
  const uint64_t delay_ms = config.min_ms + random->Uniform(static_cast<uint32_t>(spread));
  Acks acks(first);
  if (const Status s = CollectAcks(env, read_end.get(), delay_ms, &acks, &run->watch); !s.ok()) {
    return Stop(config, round, kExitHarness, s.ToString());
  }
  int wait_status = 0;
  if (const Status s = writer.KillAndReap(&wait_status); !s.ok()) {
    return Stop(config, round, kExitHarness, s.ToString());
  }
  if (const Status s = DrainAcks(read_end.get(), &acks); !s.ok()) {
    return Stop(config, round, kExitHarness, s.ToString());
  }
  if (const Status s = read_end.Close(); !s.ok()) {
    return Stop(config, round, kExitHarness, s.ToString());
  }
  // The last look at the directory before the parent opens the database and changes it.
  if (const Status s = run->watch.Sample(); !s.ok()) {
    return Stop(config, round, kExitHarness, s.ToString());
  }
  if (const Ending ending = ClassifyEnding(wait_status); !ending.killed) {
    return Stop(config, round, ending.exit_status, "the writer " + ending.text);
  }
  // Bytes of a torn acknowledgement are dropped, not counted: an operation is acknowledged only
  // when all eight bytes of its number were read.
  const uint64_t acked = acks.count();
  for (uint64_t i = first; i < first + acked; i++) {
    const Operation operation = OperationAt(config, i);
    run->model[operation.key] = Leaves(operation, i, round);
  }
  const uint64_t in_flight = first + acked;

  std::unique_ptr<DB> db;
  const Status opened =
      DB::Open(DatabaseOptions(/*create=*/first == 0 && acked == 0), config.db, &db);
  if (!opened.ok()) {
    PrintRound(round, acked, 0, "FAILED");
    return Stop(config, round, kExitFailed,
                "the database did not open after the kill: " + opened.ToString());
  }

  if (config.self_test_damage && !run->damaged.has_value()) {
    std::vector<uint64_t> holding;
    for (uint64_t index = 0; index < config.keys; index++) {
      if (run->model[index].present) {
        holding.push_back(index);
      }
    }
    if (!holding.empty()) {
      const uint64_t victim = holding[random->Uniform(static_cast<uint32_t>(
          std::min<uint64_t>(holding.size(), std::numeric_limits<uint32_t>::max())))];
      WriteOptions write_options;
      write_options.sync = true;
      if (const Status s = db->Put(write_options, Key(victim), "damaged by --self_test_damage");
          !s.ok()) {
        return Stop(config, round, kExitHarness,
                    "the damage could not be written: " + s.ToString());
      }
      run->damaged = victim;
      std::printf("self test: key %s holds a wrong value now\n", Key(victim).c_str());
    }
  }

  Findings findings;
  bool landed = false;
  Check(db.get(), config, run->model, in_flight, round, &findings, &landed);
  const uint64_t present = acked + (landed ? 1 : 0);
  if (findings.count > 0) {
    PrintRound(round, acked, present, "FAILED");
    for (const std::string& problem : findings.shown) {
      std::printf("  %s\n", problem.c_str());
    }
    if (findings.count > findings.shown.size()) {
      std::printf("  ... and %llu more\n",
                  static_cast<unsigned long long>(findings.count - findings.shown.size()));
    }
    if (run->damaged.has_value()) {
      std::printf("self test: the check %s key %s\n",
                  findings.Names(*run->damaged) ? "named" : "did NOT name",
                  Key(*run->damaged).c_str());
    }
    return Stop(config, round, kExitFailed,
                std::to_string(findings.count) + " problem(s) after the kill");
  }
  if (acks.torn_bytes() > 0) {
    std::printf("round %llu: %zu byte(s) of an acknowledgement were cut off and not counted\n",
                static_cast<unsigned long long>(round), acks.torn_bytes());
  }
  PrintRound(round, acked, present, "ok");
  run->levels = LevelSummary(db.get());
  run->acked += acked;
  run->present += present;
  if (landed) {
    // The operation is in the database, so it is in the model, and the next writer goes on after
    // it. One that did not land, or cannot be told from one that did not, is issued again.
    const Operation operation = OperationAt(config, in_flight);
    run->model[operation.key] = Leaves(operation, in_flight, round);
    run->landed++;
  }
  run->next_operation = in_flight + (landed ? 1 : 0);
  return kExitOk;
}

// Runs every round and prints the summary. Returns the exit status of the run.
int RunAll(const Config& config) {
  Env* const env = Env::Default();
  if (env->FileExists(CurrentFileName(config.db))) {
    std::fprintf(stderr,
                 "shale_crashtest: %s already holds a database, and the rounds must start from an "
                 "empty one\n",
                 config.db.c_str());
    return kExitUsage;
  }
  std::printf(
      "shale_crashtest: database %s, %llu rounds, seed %llu, %llu keys, %s, kill after %llu to "
      "%llu ms\n",
      config.db.c_str(), static_cast<unsigned long long>(config.rounds),
      static_cast<unsigned long long>(config.seed), static_cast<unsigned long long>(config.keys),
      config.sync ? "sync" : "no sync", static_cast<unsigned long long>(config.min_ms),
      static_cast<unsigned long long>(config.max_ms));
  std::fflush(stdout);
  Random random(config.seed);
  Run run(config, env);
  for (uint64_t round = 1; round <= config.rounds; round++) {
    if (const int exit_status = RunRound(config, env, &random, round, &run);
        exit_status != kExitOk) {
      return exit_status;
    }
  }
  std::printf(
      "ok: %llu rounds, %llu operations acknowledged, %llu present, %llu in flight at a kill "
      "landed; tables seen written %llu and removed %llu while a writer ran; tables at the end: "
      "%s\n",
      static_cast<unsigned long long>(config.rounds), static_cast<unsigned long long>(run.acked),
      static_cast<unsigned long long>(run.present), static_cast<unsigned long long>(run.landed),
      static_cast<unsigned long long>(run.watch.written()),
      static_cast<unsigned long long>(run.watch.removed()), run.levels.c_str());
  if (run.damaged.has_value()) {
    std::fprintf(stderr, "shale_crashtest: self test: key %s was damaged and no check noticed\n",
                 Key(*run.damaged).c_str());
  }
  if (config.self_test_damage && !run.damaged.has_value()) {
    std::fprintf(stderr,
                 "shale_crashtest: self test: no round acknowledged a put, so there was nothing "
                 "to damage\n");
    return kExitHarness;
  }
  // A run in which the writers hardly wrote has not tested a kill of anything.
  if (run.acked < config.rounds) {
    std::fprintf(stderr,
                 "shale_crashtest: only %llu operations were acknowledged in %llu rounds, too few "
                 "to show anything; is --max_ms too small for this disk?\n",
                 static_cast<unsigned long long>(run.acked),
                 static_cast<unsigned long long>(config.rounds));
    return kExitHarness;
  }
  if (run.watch.removed() < config.min_compactions) {
    std::fprintf(stderr,
                 "shale_crashtest: the run did not reach compaction: %llu tables were seen "
                 "removed, and --min_compactions asks for %llu\n",
                 static_cast<unsigned long long>(run.watch.removed()),
                 static_cast<unsigned long long>(config.min_compactions));
    return kExitHarness;
  }
  return kExitOk;
}

// ---- The command line. ------------------------------------------------------------------------

bool ParseNumber(std::string_view text, uint64_t* out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *out);
  return error == std::errc() && end == text.data() + text.size();
}

std::string NeedsValue(const std::string& name) {
  return "--" + name + " needs a value, written --" + name + "=VALUE";
}

// Reads a flag that is true or false: bare, it is true. Returns what is wrong with it, or an empty
// string.
std::string ParseBool(const std::string& name, const std::optional<std::string>& value, bool* out) {
  if (value.has_value() && *value != "true" && *value != "false") {
    return "--" + name + " is true or false, not '" + EscapeBytes(*value) + "'";
  }
  *out = !value.has_value() || *value == "true";
  return "";
}

// Reads the flags into `*config`. Returns what is wrong with them, or an empty string.
std::string ParseFlags(const std::vector<std::string>& words, Config* config) {
  bool has_db = false;
  bool has_rounds = false;
  bool has_seed = false;
  for (const std::string& word : words) {
    if (!word.starts_with("--")) {
      return "'" + EscapeBytes(word) + "' is not a flag";
    }
    const size_t equals = word.find('=');
    const std::string name = word.substr(2, equals == std::string::npos ? equals : equals - 2);
    const std::optional<std::string> value =
        equals == std::string::npos ? std::nullopt
                                    : std::optional<std::string>(word.substr(equals + 1));
    if (name == "self_test_damage" || name == "sync") {
      const std::string wrong =
          ParseBool(name, value, name == "sync" ? &config->sync : &config->self_test_damage);
      if (!wrong.empty()) {
        return wrong;
      }
      continue;
    }
    if (name != "db" && name != "rounds" && name != "seed" && name != "min_ms" &&
        name != "max_ms" && name != "keys" && name != "min_compactions") {
      return "unknown flag --" + EscapeBytes(name);
    }
    if (!value.has_value()) {
      return NeedsValue(name);
    }
    if (name == "db") {
      if (value->empty()) {
        return "--db needs a path, and it is empty";
      }
      config->db = *value;
      has_db = true;
      continue;
    }
    uint64_t number = 0;
    if (!ParseNumber(*value, &number)) {
      return "--" + name + " is a whole number, not '" + EscapeBytes(*value) + "'";
    }
    if (name == "rounds") {
      config->rounds = number;
      has_rounds = true;
    } else if (name == "seed") {
      config->seed = number;
      has_seed = true;
    } else if (name == "min_ms") {
      config->min_ms = number;
    } else if (name == "max_ms") {
      config->max_ms = number;
    } else if (name == "keys") {
      config->keys = number;
    } else {
      config->min_compactions = number;
    }
  }
  if (!has_db) {
    return "--db=PATH is needed to say where the database goes";
  }
  if (!has_rounds || config->rounds == 0) {
    return "--rounds=N is needed, and N is at least 1";
  }
  if (!has_seed) {
    return "--seed=S is needed: it fixes the delays before the kills and the operations";
  }
  if (config->max_ms > kMaxDelayMs || config->min_ms > config->max_ms) {
    return "--min_ms must not be more than --max_ms, which must not be more than " +
           std::to_string(kMaxDelayMs);
  }
  if (config->keys == 0 || config->keys > kMaxKeys) {
    return "--keys is from 1 to " + std::to_string(kMaxKeys);
  }
  return "";
}

int Main(const std::vector<std::string>& words) {
  Config config;
  if (const std::string wrong = ParseFlags(words, &config); !wrong.empty()) {
    std::fprintf(stderr, "shale_crashtest: %s\n%s", wrong.c_str(), kUsage);
    return kExitUsage;
  }
  const int exit_status = RunAll(config);
  // A summary that did not get written is a failure: a run that says OK after losing its output is
  // worse than one that fails.
  const bool flushed = std::fflush(stdout) == 0;
  if ((!flushed || std::ferror(stdout) != 0) && exit_status == kExitOk) {
    std::fprintf(stderr, "shale_crashtest: cannot write to standard output\n");
    return kExitHarness;
  }
  return exit_status;
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
    std::fprintf(stderr, "shale_crashtest: %s\n", e.what());
    return shale::kExitHarness;
  }
}
