#ifndef SHALE_OPTIONS_H_
#define SHALE_OPTIONS_H_

#include <cstddef>
#include <cstdint>

#include "shale/status.h"

namespace shale {

class Env;
class Scheduler;
class Snapshot;

/// The longest key the database accepts, in bytes. A key above it makes a write fail with
/// InvalidArgument.
inline constexpr size_t kMaxKeySize = 65536;

/// The longest value the database accepts, in bytes. A value above it makes a write fail with
/// InvalidArgument.
inline constexpr size_t kMaxValueSize = 67108864;

/// How a database is opened and how it behaves while it is open. ValidateOptions() states what each
/// field may be; DB::Open() refuses options that break it, before it touches any file.
struct Options {
  /// The Env that every file and clock reading goes through. Null means Env::Default(). It must
  /// outlive the database.
  Env* env = nullptr;

  /// Where the database's background work runs. Null means the database owns a scheduler with a
  /// thread of its own, which is what a database used from several threads wants. A test passes a
  /// ManualScheduler to decide when background work runs. It must outlive the database.
  ///
  /// A ManualScheduler is not thread-safe, and the database uses it from any call that can write,
  /// flush, compact or close: such a call can queue a task on it (the scheduler's own guard aborts
  /// if two threads are inside it at once), and one that has to wait for a flush or a compaction
  /// runs the queued tasks on its own thread. So a database driven by a ManualScheduler must not
  /// have any of those calls overlap another on a second thread: not a write, not a Flush(), not a
  /// CompactRange(), not the close, whether or not the one that overlaps it would have waited.
  /// Reads, iterators and snapshots never touch the scheduler and may come from any thread.
  /// A database that several threads write to is given the default, with a thread of its own.
  Scheduler* scheduler = nullptr;

  /// Whether DB::Open() creates the database if there is none at the path. If false, opening a
  /// path that holds no database fails with InvalidArgument and creates nothing.
  bool create_if_missing = false;

  /// Whether DB::Open() fails with InvalidArgument if there already is a database at the path.
  bool error_if_exists = false;

  /// Whether the database treats damage as an error where it can. At open, it decides what is made
  /// of a write-ahead log with a defect other than the three that a crash leaves, which Open
  /// tolerates whatever this says: an end cut short, zeros from a record boundary to the end of the
  /// file (the info log names the file and the number of zero bytes), and a log that begins after
  /// a gap in the sequence numbers (a power loss took the unsynced end of the log before it). At a
  /// gap, the log and every later one are set aside and nothing in them is applied. For any other
  /// defect, zeros followed by any byte that is not zero and zeros inside a record among them,
  /// with this set Open fails with Corruption; without it, Open recovers the writes that come
  /// before the first defect and applies nothing after it, in that log or in any later one. Those
  /// logs are not deleted: each is renamed to its own name with ".dropped" added, which the
  /// database never reads or deletes again, and the info log says which and why. They are kept
  /// until DestroyDB() or until somebody removes them by hand. A log that cannot be read is not a
  /// damaged one: a read error fails Open with its IOError whatever this says.
  bool paranoid_checks = true;

  /// The size in bytes that a memtable may reach before it is written out as a table.
  size_t write_buffer_size = 4 << 20;

  /// The approximate size in bytes of the data blocks of a table.
  size_t block_size = 4096;

  /// The number of keys between the full keys of a block, from which the others are compressed.
  int block_restart_interval = 16;

  /// The bits of Bloom filter that a table spends for each key. 0 builds no filter.
  int bloom_bits_per_key = 10;

  /// The size in bytes of the cache of table blocks.
  size_t block_cache_capacity = 8 << 20;

  /// The most files the database keeps open; some are for tables, which it opens as reads need
  /// them, and the rest for the log and the other files it writes.
  int max_open_files = 1000;

  /// The number of files at level 0 at which that level is compacted into level 1. It is never
  /// taken to be above l0_stop_trigger: a write that waits for level 0 to shrink must have a
  /// compaction coming.
  int l0_compaction_trigger = 4;

  /// The number of files at level 0 at which each write is slowed down, so that compaction can
  /// catch up: a write sleeps for a millisecond, once, whatever the number of writers that share
  /// it.
  int l0_slowdown_trigger = 8;

  /// The number of files at level 0 at which writes wait until compaction has reduced them. A
  /// write waits when it needs a new memtable, which is what adds a file to level 0, so level 0
  /// does not grow past this under writes that never stop.
  int l0_stop_trigger = 12;

  /// The size in bytes that level 1 may reach before it is compacted into level 2.
  uint64_t max_bytes_for_level_base = 10 << 20;

  /// How many times larger each level may be than the one above it, from level 1 down.
  int level_size_multiplier = 10;

  /// The size in bytes that a compaction aims for in each table it writes.
  uint64_t target_file_size = 2 << 20;
};

/// How a read is done.
struct ReadOptions {
  /// Whether the checksum of every block read from a table is verified. A block that fails it is a
  /// Corruption, and never a wrong answer.
  bool verify_checksums = true;

  /// Whether the blocks that the read loads are kept in the cache. A scan of the whole database
  /// turns it off so as not to push out what the other reads want.
  bool fill_cache = true;

  /// The snapshot to read as of, from DB::GetSnapshot() on the same database. Null reads the
  /// newest state. It must not have been released.
  const Snapshot* snapshot = nullptr;
};

/// How a write is done.
struct WriteOptions {
  /// Whether the write is on stable storage when the call returns. With it, an acknowledged write
  /// survives a crash of the machine; without it, only a crash of the process, and a power loss may
  /// lose the most recent writes, but never loses a write and keeps a later one.
  bool sync = true;
};

/// Checks that `options` is one the database can work with. Returns OK, or InvalidArgument whose
/// message names the first field that is out of range and says what it may be.
///
/// The rules are: write_buffer_size at least 16384; block_size from 256 to 4194304;
/// block_restart_interval from 1 to 1024; bloom_bits_per_key from 0 to 30; max_open_files at least
/// 20; each of the three level 0 triggers at least 1, and l0_stop_trigger at least
/// l0_slowdown_trigger; max_bytes_for_level_base at least 65536; level_size_multiplier from 2 to
/// 100; target_file_size at least 16384. Nothing is asked of the others.
/// @param options the options to check.
Status ValidateOptions(const Options& options);

}  // namespace shale

#endif  // SHALE_OPTIONS_H_
