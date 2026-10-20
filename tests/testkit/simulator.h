#ifndef SHALE_TESTKIT_SIMULATOR_H_
#define SHALE_TESTKIT_SIMULATOR_H_

// The deterministic simulator: a seeded driver that runs random operations against a database
// over a fault-injecting Env and a manual scheduler, and checks it against the reference model
// after every step. It lives in the shale_simulator library, which only test targets link.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "shale/db.h"

namespace shale {

/// What one run does.
struct SimOptions {
  /// Fixes every random choice of the run: the operations, the keys and values, and what the Env
  /// loses at a power loss and fails on purpose. The same options give the same run.
  uint64_t seed = 0;
  /// How many operations the run makes.
  int steps = 2000;
  /// How many different keys the run uses, "k0000" and up.
  int key_space = 200;
  /// Whether the run loses power and kills the process. Without it nothing disturbs the database
  /// except an injected failure, and after one it is closed and opened again, not crashed.
  bool crashes = true;
  /// Whether the run makes calls of the Env fail.
  bool io_errors = true;
  /// Whether the run flips bits in what the Env reads.
  bool read_corruption = true;
  /// A hook for the tests of the simulator itself: if set, each database that the run opens is
  /// given to it, and the run drives what it returns, so that a test can put a wrapper in front of
  /// the real database that misbehaves in one named way and see the run fail. The wrapper owns the
  /// database it is given and passes the calls it does not change through to it. Empty, the run
  /// drives the database itself.
  std::function<std::unique_ptr<DB>(std::unique_ptr<DB>)> wrap_db;
};

/// What a run did, counted. Everything is a total over the run, and two runs with the same options
/// have equal stats.
struct SimStats {
  uint64_t steps = 0;               ///< Operations made.
  uint64_t writes = 0;              ///< Puts, deletes and batches issued.
  uint64_t sync_writes = 0;         ///< Of them, those that asked for a sync.
  uint64_t power_losses = 0;        ///< Power losses simulated, whatever caused them.
  uint64_t process_crashes = 0;     ///< Process crashes simulated.
  uint64_t flushes = 0;             ///< Memtables written out, by the databases of the run.
  uint64_t compactions = 0;         ///< Compactions that merged files.
  uint64_t trivial_moves = 0;       ///< Files moved down a level as they were.
  uint64_t tombstones_dropped = 0;  ///< Deletion entries that a compaction dropped.
  uint64_t snapshots_across_compaction = 0;  ///< Snapshots that were held while one ran.
  uint64_t iterators_across_compaction = 0;  ///< Iterators that were held while one ran.
  uint64_t injected_errors = 0;              ///< Calls that the Env failed on purpose.
  uint64_t injected_corruptions = 0;         ///< Reads that the Env returned with a flipped bit.
  uint64_t reopens = 0;                      ///< Times the database was closed and opened again.
  uint64_t write_failures = 0;               ///< Times a failure stopped the database's writes.
  uint64_t read_failures = 0;                ///< Reads that failed, for a fault that explains it.
  uint64_t open_failures = 0;                ///< Opens that failed, for a fault that explains it.
  uint64_t orphan_checks = 0;                ///< Times the directory was checked for orphan tables.
  uint64_t deepest_level = 0;                ///< The deepest level that held a table at a check.
  uint64_t damaged_opens = 0;                ///< Opens that worked with a read corrupted in them.
  /// Opens that worked and dropped the zeros that a log or the MANIFEST ended in (D6), as the LOG
  /// of the open says.
  uint64_t zero_tail_opens = 0;
  /// Of them, those where it was the MANIFEST that ended in zeros.
  uint64_t manifest_zero_tail_opens = 0;

  /// Whether every counter is equal.
  bool operator==(const SimStats&) const = default;
};

/// The outcome of a run.
struct SimResult {
  /// Whether every check of every step held.
  bool ok = true;
  /// If not ok, what failed: the message of the invariant, then "seed=<n> step=<k>", then the last
  /// 50 operations, one to a line, oldest first. Empty if ok.
  std::string failure;
  /// What the run did.
  SimStats stats;
  /// A hash over every operation's kind, key, outcome code and value digest, and then over the
  /// final counters. Two runs with the same options have the same digest, and a run that went
  /// otherwise almost surely has a different one.
  uint64_t trace_digest = 0;
};

/// Runs the simulation that `options` describe and returns what happened.
///
/// A run opens a database over a FaultInjectionEnv and a ManualScheduler, with options small enough
/// that a few kilobytes of writes fill a memtable and a few of them fill a level. For
/// `options.steps` steps it picks an operation by weight: a put, a delete, a batch, a get, a scan,
/// a snapshot taken, read or released, an iterator opened or stepped, a flush, a range compaction,
/// one background task, a power loss, a process crash, an injected failure of one kind of Env call,
/// or read corruption for the next twenty steps. After every step it checks the invariants: every
/// read against the model (I1), the version and the sequence number (I2), the directory for orphan
/// tables when that can be known (I3); after every reopen that the database holds what a crash may
/// leave (I4); that an error is allowed only where a fault that fired explains it (I5); and that a
/// database that has failed to write goes on failing (I6).
///
/// The run stops at the first failure and returns it. It does not throw.
///
/// **The crash model.** A crash is made between two operations, never inside one, and a background
/// task, a flush or a compaction, is one operation to the driver, so the machine is never stopped
/// in the middle of one. What a crash finds in the middle of a task is reached the other way: an
/// armed failure of an Env call that fires inside the task leaves what the task had written, and
/// stops the database, and the reopen that follows, after the next write has been shown to fail,
/// is a power loss in four cases of six, a process crash in one and a clean close in one, so that
/// the files are mostly as a power loss leaves them. A power loss keeps what was synced and a
/// prefix of the rest or, for one file in four, zeros in place of the rest, and one in three keeps
/// only what was synced. An open that fails for a fault is followed, half the time, by another
/// crash before the open is tried again.
///
/// **Corruption during an open.** On one process crash in four, read corruption is armed for the
/// open. An open that fails for it must lose nothing: the open that follows, with nothing armed,
/// is held to I4 in full. An open that works although a read was corrupted is held to I4 in full
/// too, with every write that was acknowledged and every one that was synced: the reader of a log
/// reports a record whose length a flipped bit has made longer than what is left of the file when
/// the record is whole, by finding its checksum, and a flipped bit anywhere else in a log, a
/// MANIFEST or a table is reported by the checksum of its record or block, so a corrupted read
/// that the open did not fail on is one that changed nothing that it used. (Until the reader did
/// that, such an open lost the rest of the file, a synced write among it, in about one run in
/// three hundred, and the simulator could not hold it to I4.)
///
/// **Replay.** A run is a function of its options, on every toolchain that CI builds with: the same
/// options give the same operations, the same answers and the same trace digest, in any build, with
/// any of those compilers and standard libraries. The engine's memtable is full when its arena says
/// so, and the arena pads to a constant, not to alignof(std::max_align_t), which is why.
/// determinism_test pins the digests of three seeds, so that a toolchain that differs is seen.
/// @param options the seed and the shape of the run.
SimResult RunSimulation(const SimOptions& options);

}  // namespace shale

#endif  // SHALE_TESTKIT_SIMULATOR_H_
