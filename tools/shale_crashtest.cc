// shale_crashtest: kills a process that is writing to a database, and checks what is left.
//
// Every round forks a writer. The writer opens the database, then writes one key after another
// with a synced Put, and each time a Put returns OK it sends the number of the key to the parent
// through a pipe. That is an acknowledgement: the database has said the key is stored. After a
// random time the parent sends the writer SIGKILL, reads every acknowledgement, opens the database
// again and checks it. Every acknowledged key must be there with its value; the one key that may
// have been in flight at the kill is either absent or has the value that was being written; nothing
// else may be in the database; and everything that earlier rounds left must still be there.
//
// This is the one test that runs the engine, the real filesystem and a real kill together. It
// proves what a process kill leaves behind, which is the whole of the page cache. It does not prove
// anything about power loss: that is the simulator's job, over a filesystem that can lose what was
// not synced. Run it with no arguments for the usage.

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
#include <memory>
#include <optional>
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
constexpr int kExitHarness = 3;  // the test itself could not run, so it proved nothing

constexpr char kUsage[] =
    "usage: shale_crashtest --db=PATH --rounds=N --seed=S [--min_ms=50] [--max_ms=500]\n"
    "                       [--self_test_damage]\n"
    "\n"
    "Each of N rounds forks a writer that puts keys into the database at PATH with sync on and\n"
    "reports each key it was told is stored. After a random time in [min_ms, max_ms], drawn from\n"
    "the seed, the parent kills the writer with SIGKILL, opens the database and checks it against\n"
    "the reports. PATH must not hold a database yet; it is created. A failed run leaves the\n"
    "database where it is.\n"
    "\n"
    "--self_test_damage overwrites one reported key with a wrong value, through the database,\n"
    "after the first kill that had one and before the check. The run must then name that key and\n"
    "exit with status 1: it shows that the check can fail.\n"
    "\n"
    "exit status: 0 every round passed, 1 a check failed, 2 usage error, 3 the test could not "
    "run\n";

// The size of a value, and of an acknowledgement, in bytes.
constexpr size_t kValueSize = 100;
constexpr size_t kAckSize = 8;

// How many problems of a round are printed; the rest are counted.
constexpr size_t kMaxShown = 10;

// The statuses the writer ends with when it ends by itself, which it only does when something is
// wrong. They are far from the 0 and 1 that a process can end with for other reasons.
constexpr int kWriterOpenFailed = 70;
constexpr int kWriterPutFailed = 71;
constexpr int kWriterPipeFailed = 72;

// The longest wait that --max_ms may ask for: an hour, which a round is never meant to take.
constexpr uint64_t kMaxDelayMs = 3'600'000;

struct Config {
  std::string db;
  uint64_t rounds = 0;
  uint64_t seed = 0;
  uint64_t min_ms = 50;
  uint64_t max_ms = 500;
  bool self_test_damage = false;
};

Status Errno(std::string_view what) { return Status::IOError(what, std::strerror(errno)); }

// ---- What is written. -------------------------------------------------------------------------

std::string Key(uint64_t i) {
  std::array<char, 32> text{};
  std::snprintf(text.data(), text.size(), "key%010llu", static_cast<unsigned long long>(i));
  return text.data();
}

// The value of key `i` in round `round`: "<i>-<round>-" padded to kValueSize with bytes that depend
// only on the two numbers, so that a value that belongs to another key or another round is not
// taken for this one.
std::string Value(uint64_t i, uint64_t round) {
  std::string value = std::to_string(i) + "-" + std::to_string(round) + "-";
  Random padding((i * 0x9e3779b97f4a7c15ULL) ^ round);
  while (value.size() < kValueSize) {
    value.push_back(static_cast<char>('a' + padding.Uniform(26)));
  }
  return value;
}

// The database's options. A small write buffer and small tables make the writer flush every few
// hundred keys and pile tables up for the compactions, so that a kill can land among them. Only a
// run that has acknowledged no key yet may create the database: one that has vanished since is an
// error to report, not to replace.
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

// Writes keys from `first` on and acknowledges each, until it is killed. It ends itself only if
// something fails, and says what on standard error. It leaves with _exit(): the parent's buffers
// and destructors are not its to run.
[[noreturn]] void RunWriter(const std::string& dbname, int ack_fd, uint64_t first, uint64_t round) {
  std::unique_ptr<DB> db;
  const Status opened = DB::Open(DatabaseOptions(/*create=*/first == 0), dbname, &db);
  if (!opened.ok()) {
    std::fprintf(stderr, "shale_crashtest: the writer could not open the database: %s\n",
                 opened.ToString().c_str());
    _exit(kWriterOpenFailed);
  }
  WriteOptions write_options;
  write_options.sync = true;
  for (uint64_t i = first;; i++) {
    const Status put = db->Put(write_options, Key(i), Value(i, round));
    if (!put.ok()) {
      std::fprintf(stderr, "shale_crashtest: the writer's Put of %s failed: %s\n", Key(i).c_str(),
                   put.ToString().c_str());
      _exit(kWriterPutFailed);
    }
    std::array<char, kAckSize> ack{};
    EncodeFixed64(ack.data(), i);
    if (!WriteAll(ack_fd, ack.data(), ack.size())) {
      std::fprintf(stderr, "shale_crashtest: the writer could not report %s: %s\n", Key(i).c_str(),
                   std::strerror(errno));
      _exit(kWriterPipeFailed);
    }
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

// The acknowledgements read so far. One is eight whole bytes, the number of a key, and the keys
// come one after the other from `first`. Bytes that do not make eight are not an acknowledgement
// and are never counted as one: the writer may be killed in the middle of a write.
class Acks {
 public:
  explicit Acks(uint64_t first) : first_(first), next_(first) {}

  // Takes the bytes just read. Returns an error if an acknowledgement is not the key after the one
  // before it, which means the writer and this reader disagree about the protocol, and nothing
  // read in this round can be trusted.
  Status Add(std::string_view bytes) {
    pending_.append(bytes);
    size_t used = 0;
    while (pending_.size() - used >= kAckSize) {
      const uint64_t key = DecodeFixed64(pending_.data() + used);
      if (key != next_) {
        return Status::Corruption("the writer acknowledged key " + std::to_string(key) +
                                  " when key " + std::to_string(next_) + " was next");
      }
      next_++;
      used += kAckSize;
    }
    pending_.erase(0, used);
    return Status::OK();
  }

  // How many keys were acknowledged: first, first + 1, ... first + count() - 1.
  uint64_t count() const { return next_ - first_; }

  // How many bytes of an acknowledgement that did not come whole were read, and so were dropped.
  size_t torn_bytes() const { return pending_.size(); }

 private:
  const uint64_t first_;
  uint64_t next_;
  std::string pending_;
};

// Reads the acknowledgements from `fd` for `delay_ms`, or until the writer's end of the pipe is
// closed, which only a writer that has ended does. The pipe is read while waiting, so that the
// writer is never held up by a full pipe.
Status CollectAcks(Env* env, int fd, uint64_t delay_ms, Acks* acks) {
  const uint64_t deadline = env->NowMicros() + delay_ms * 1000;
  std::array<char, 4096> buffer{};
  for (;;) {
    const uint64_t now = env->NowMicros();
    if (now >= deadline) {
      return Status::OK();
    }
    // Rounded up, so that a wait that is a fraction of a millisecond short is not a busy loop.
    const uint64_t remaining_ms = (deadline - now + 999) / 1000;
    pollfd wait{fd, POLLIN, 0};
    const int ready = ::poll(&wait, 1, static_cast<int>(std::min<uint64_t>(remaining_ms, INT_MAX)));
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
      ending.text = "stopped because a Put failed";
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

// ---- The check. -------------------------------------------------------------------------------

// What a check found: the number of problems, the first few in words, and the number of every key
// that a problem names.
struct Findings {
  uint64_t count = 0;
  std::vector<std::string> shown;
  std::vector<uint64_t> keys;

  void Add(uint64_t key_number, const std::string& problem) {
    keys.push_back(key_number);
    AddOther(Key(key_number) + " " + problem);
  }

  void AddOther(const std::string& problem) {
    count++;
    if (shown.size() < kMaxShown) {
      shown.push_back(problem);
    }
  }

  bool Names(uint64_t key_number) const {
    return std::find(keys.begin(), keys.end(), key_number) != keys.end();
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

// The round that wrote key `key`. `round_starts[r - 1]` is the first key of round r, and the
// rounds that wrote nothing repeat the start of the one after them.
uint64_t RoundOf(const std::vector<uint64_t>& round_starts, uint64_t key) {
  return static_cast<uint64_t>(std::upper_bound(round_starts.begin(), round_starts.end(), key) -
                               round_starts.begin());
}

// What the check of one round saw, besides what is wrong.
struct Observed {
  uint64_t acked_found = 0;        // the acknowledged keys that a Get returned with the right value
  bool in_flight_present = false;  // the key after the last acknowledgement is in the database
};

// Checks the reopened database `db` after a round that acknowledged keys `first` to
// `first + acked - 1`.
//
// The acknowledged keys are read with Get. Then one scan of the whole database is compared with
// what the rounds so far have written: keys from 0 up to the last acknowledgement, each with the
// value of the round that wrote it, and then at most the one key after, which the writer may have
// been writing at the kill. A key that is missing, one that holds another value, and one that
// should not be there at all are three different findings. The two reads are different paths
// through the engine, and the scan is the only one that sees what earlier rounds left.
Findings Check(DB* db, const std::vector<uint64_t>& round_starts, uint64_t first, uint64_t acked,
               Observed* observed) {
  Findings findings;
  const uint64_t round = RoundOf(round_starts, first);
  for (uint64_t i = first; i < first + acked; i++) {
    std::string value;
    const Status s = db->Get(ReadOptions(), Key(i), &value);
    if (s.IsNotFound()) {
      findings.Add(i, "was acknowledged and Get says it is not there");
    } else if (!s.ok()) {
      findings.Add(i, "was acknowledged and Get failed: " + s.ToString());
    } else if (value != Value(i, round)) {
      findings.Add(i, "was acknowledged and Get returned " + Mismatch(value, Value(i, round)));
    } else {
      observed->acked_found++;
    }
  }

  const uint64_t first_unacked = first + acked;
  const std::unique_ptr<Iterator> scan = db->NewIterator(ReadOptions());
  uint64_t want = 0;  // the next key below first_unacked that the scan has yet to meet
  scan->SeekToFirst();
  while (scan->Valid()) {
    const std::string_view key = scan->key();
    if (want < first_unacked) {
      const std::string wanted = Key(want);
      if (key == wanted) {
        const std::string right = Value(want, RoundOf(round_starts, want));
        if (scan->value() != right) {
          findings.Add(want, "was found by the scan holding " + Mismatch(scan->value(), right));
        }
        want++;
        scan->Next();
      } else if (key < wanted) {
        findings.AddOther("the scan found " + Excerpt(key) + ", which nothing wrote");
        scan->Next();
      } else {
        findings.Add(want, "is missing from the scan");
        want++;
      }
      continue;
    }
    if (key == Key(first_unacked) && !observed->in_flight_present) {
      if (scan->value() == Value(first_unacked, round)) {
        observed->in_flight_present = true;
      } else {
        findings.Add(first_unacked, "follows the last acknowledgement and holds " +
                                        Mismatch(scan->value(), Value(first_unacked, round)) +
                                        ", a value nobody wrote");
      }
    } else {
      findings.AddOther("the scan found " + Excerpt(key) + " past the last acknowledgement");
    }
    scan->Next();
  }
  if (const Status s = scan->status(); !s.ok()) {
    findings.AddOther("the scan stopped with an error: " + s.ToString());
  }
  for (; want < first_unacked; want++) {
    findings.Add(want, "is missing from the scan");
  }
  return findings;
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
  uint64_t next = 0;  // the key the next writer starts at: the highest present, plus one
  std::vector<uint64_t> round_starts;
  uint64_t acked = 0;
  uint64_t present = 0;
  uint64_t in_flight = 0;  // rounds whose last key was present although it was not acknowledged
  std::string levels = "none";
  std::optional<uint64_t> damaged;  // the key --self_test_damage overwrote
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

void PrintRound(uint64_t round, uint64_t acked, uint64_t present, const char* verdict) {
  std::printf("round %llu: acked %llu, present %llu, %s\n", static_cast<unsigned long long>(round),
              static_cast<unsigned long long>(acked), static_cast<unsigned long long>(present),
              verdict);
}

// Runs one round. Returns kExitOk if it passed and the run goes on, or the exit status to stop
// with.
int RunRound(const Config& config, Env* env, Random* random, uint64_t round, Run* run) {
  const uint64_t first = run->next;
  run->round_starts.push_back(first);

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
    RunWriter(config.db, write_end.get(), first, round);
  }
  Writer writer(pid);
  if (const Status s = write_end.Close(); !s.ok()) {
    return Stop(config, round, kExitHarness, s.ToString());
  }

  const uint64_t spread = config.max_ms - config.min_ms + 1;
  const uint64_t delay_ms = config.min_ms + random->Uniform(static_cast<uint32_t>(spread));
  Acks acks(first);
  if (const Status s = CollectAcks(env, read_end.get(), delay_ms, &acks); !s.ok()) {
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
  if (const Ending ending = ClassifyEnding(wait_status); !ending.killed) {
    return Stop(config, round, ending.exit_status, "the writer " + ending.text);
  }
  // Bytes of a torn acknowledgement are dropped, not counted: a key is acknowledged only when
  // all eight bytes of its number were read.
  const uint64_t acked = acks.count();

  std::unique_ptr<DB> db;
  const Status opened =
      DB::Open(DatabaseOptions(/*create=*/first == 0 && acked == 0), config.db, &db);
  if (!opened.ok()) {
    PrintRound(round, acked, 0, "FAILED");
    return Stop(config, round, kExitFailed,
                "the database did not open after the kill: " + opened.ToString());
  }

  if (config.self_test_damage && !run->damaged.has_value() && acked > 0) {
    const uint64_t victim = first + random->Uniform(static_cast<uint32_t>(std::min<uint64_t>(
                                        acked, std::numeric_limits<uint32_t>::max())));
    WriteOptions write_options;
    write_options.sync = true;
    if (const Status s = db->Put(write_options, Key(victim), "damaged by --self_test_damage");
        !s.ok()) {
      return Stop(config, round, kExitHarness, "the damage could not be written: " + s.ToString());
    }
    run->damaged = victim;
    std::printf("self test: key %s, which was acknowledged, now holds a wrong value\n",
                Key(victim).c_str());
  }

  Observed observed;
  const Findings findings = Check(db.get(), run->round_starts, first, acked, &observed);
  const uint64_t present = observed.acked_found + (observed.in_flight_present ? 1 : 0);
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
  run->in_flight += observed.in_flight_present ? 1 : 0;
  run->next = first + acked + (observed.in_flight_present ? 1 : 0);
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
  std::printf("shale_crashtest: database %s, %llu rounds, seed %llu, kill after %llu to %llu ms\n",
              config.db.c_str(), static_cast<unsigned long long>(config.rounds),
              static_cast<unsigned long long>(config.seed),
              static_cast<unsigned long long>(config.min_ms),
              static_cast<unsigned long long>(config.max_ms));
  Random random(config.seed);
  Run run;
  for (uint64_t round = 1; round <= config.rounds; round++) {
    if (const int exit_status = RunRound(config, env, &random, round, &run);
        exit_status != kExitOk) {
      return exit_status;
    }
  }
  std::printf(
      "ok: %llu rounds, %llu keys acknowledged, %llu present, %llu kills with a write in "
      "flight; tables at the end: %s\n",
      static_cast<unsigned long long>(config.rounds), static_cast<unsigned long long>(run.acked),
      static_cast<unsigned long long>(run.present), static_cast<unsigned long long>(run.in_flight),
      run.levels.c_str());
  if (run.damaged.has_value()) {
    std::fprintf(stderr, "shale_crashtest: self test: key %s was damaged and no check noticed\n",
                 Key(*run.damaged).c_str());
  }
  if (config.self_test_damage && !run.damaged.has_value()) {
    std::fprintf(stderr,
                 "shale_crashtest: self test: no round acknowledged a key, so there was nothing "
                 "to damage\n");
    return kExitHarness;
  }
  // A run in which the writers hardly wrote has not tested a kill of anything.
  if (run.acked < config.rounds) {
    std::fprintf(stderr,
                 "shale_crashtest: only %llu keys were acknowledged in %llu rounds, too few to "
                 "show anything; is --max_ms too small for this disk?\n",
                 static_cast<unsigned long long>(run.acked),
                 static_cast<unsigned long long>(config.rounds));
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
    if (name == "self_test_damage") {
      if (value.has_value() && *value != "true" && *value != "false") {
        return "--self_test_damage is true or false, not '" + EscapeBytes(*value) + "'";
      }
      config->self_test_damage = !value.has_value() || *value == "true";
      continue;
    }
    if (name != "db" && name != "rounds" && name != "seed" && name != "min_ms" &&
        name != "max_ms") {
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
    } else {
      config->max_ms = number;
    }
  }
  if (!has_db) {
    return "--db=PATH is needed to say where the database goes";
  }
  if (!has_rounds || config->rounds == 0) {
    return "--rounds=N is needed, and N is at least 1";
  }
  if (!has_seed) {
    return "--seed=S is needed, so that a run can be repeated";
  }
  if (config->max_ms > kMaxDelayMs || config->min_ms > config->max_ms) {
    return "--min_ms must not be more than --max_ms, which must not be more than " +
           std::to_string(kMaxDelayMs);
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
