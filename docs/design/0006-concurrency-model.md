# 0006. The concurrency model

Status: accepted

## Context

A `DB` is used from any number of threads at once: many readers, many writers, scans that run while the
engine flushes and compacts. The way to get that wrong is to have a lock discipline that lives in
people's heads, so the model has to be small enough to state, and enforced where it can be by the
compiler, a runtime check, or a test that fails when it is broken.

## Decision

**One mutex guards every piece of shared mutable state of the database.** The memtables and the log
that the newer one is written to, the version set, the snapshot list, the queue of writers and the first
background error are all `SHALE_GUARDED_BY(mutex_)`. A read holds it only to take what it will read: the
sequence number, the two memtables and the current `Version`. It then runs with no lock.

**Writers queue, and the one at the front writes for all.** A write puts itself in a queue
(`std::deque<Writer*>`). The writer at the front is the *leader*: it makes room, takes the writers behind
it into a group (up to 1 MiB, never a writer that wants a sync behind a leader that does not), gives
the group's batches consecutive sequence numbers, **gives up the mutex**, appends the group to the log
as one record, syncs it if any member asked, inserts it into the memtable, takes the mutex, publishes
the new last sequence number, and wakes the group with one status. That is **group commit**: the cost of
the sync is shared by the writers that queued while the previous group was being written.

**The memtable is single-writer, multi-reader, and lock-free to read.** A skip list in an arena. A node
is built completely and then linked in level by level with a release store, and every pointer a reader
follows is loaded with acquire, so whatever node a reader reaches, it sees that node as the writer left
it. Nothing is ever removed. Only the leader inserts, and **the class refuses to be misused**: `Insert`
and `MemTable::Add` abort, naming the class, if two threads are inside at once.

**Versions are immutable and shared.** A `Version` is a value (the files of each level). A read takes a
`std::shared_ptr<const Version>`; a flush or a compaction makes a new one and installs it; an old one
lives until its last reader lets go. The set of files that must not be deleted is the union of the files
of every version still alive, tracked with weak pointers (`VersionSet::LiveFiles`), so a version that
the last reader dropped leaves the set by itself.

**One background task at a time.** Flush and compaction run on one `Scheduler`, one task at a time. A
task prefers a flush of the waiting memtable to a compaction, because writes may be stalled for it; a
compaction writes out the waiting memtable between its output files. Only one task at a time makes
tables, so the picker never has to be shown a compaction that is in flight, and the sweep of obsolete
files can ask whether a table was begun while it was away.

**The mutex is given up for the slow parts and held for the decisions.** The leader gives it up for the
log append and sync; a flush and a compaction give it up while they build tables; the sweep gives it up
to list and delete files.

**The discipline is checked, not remembered.**

- `Mutex` (`src/util/mutex.h`) is annotated for Clang's thread-safety analysis and built with
  `-Wthread-safety -Werror`: reading a `SHALE_GUARDED_BY` member without the lock, or calling a
  `SHALE_REQUIRES` function without it, does not compile. It also records its owner, so locking a mutex
  the thread holds, unlocking one it does not, and waiting without it **abort** at run time, where the
  analysis cannot follow.
- The analysis can silently stop working, so the build is tested: `tests/compile_fail/` holds a unit that
  reads a guarded member without the lock, which the build must reject, and one with the lock, which it
  must accept (`thread_safety_rejects_unguarded_access`, `thread_safety_accepts_guarded_access`).
- `tests/stress/concurrency_test.cc` runs real threads against the database (writers, readers, scans that
  span flushes and compactions, snapshots held across both), and the sanitize job runs the whole suite
  three times under ThreadSanitizer.
- Misuse that no lock can make safe aborts with a message and does not corrupt: closing a database with
  a live iterator or snapshot, releasing a snapshot twice or on another database, a `ManualScheduler`
  entered from two threads at once.

## Consequences

**What this buys.** An argument that fits on a page: one mutex, one writer at a time into the memtable,
immutable versions, one background task. Each part has a check that fails when it is broken.

**What it costs.**

- **One background thread bounds the write rate the engine can sustain.** If compaction cannot keep up,
  level 0 fills and writes slow and then wait; `write_slowdowns` and `write_stops` count it. More threads
  would need a picker that knows which files are already being compacted.
- **Some I/O happens with the mutex held.** The memtable switch syncs the old log, creates the new one and
  syncs the directory under the mutex, and `LogAndApply` appends to and syncs the MANIFEST under it, so a
  read that needs the mutex to take its view waits for those syncs. They happen once per memtable and
  once per flush or compaction, and not per write.
- **Writes are serialised through the memtable.** The leader inserts the whole group. A concurrent skip
  list would let writers insert in parallel, at the price of a much harder argument.
- **Thread-safety analysis needs Clang.** The annotations expand to nothing under GCC, so the compile-time
  check runs in the Clang and AppleClang builds only; the other builds still get the runtime checks and
  ThreadSanitizer.
- **A read at a snapshot is linear in the number of open snapshots**, since the snapshot is found by
  address in a list under the mutex. Open snapshots are meant to be few.
- **A `ManualScheduler` is single-threaded.** A database that a test drives with one cannot have two
  threads write, flush or compact at once, and aborts if they do. The simulator relies on that; a
  database for several threads uses the default scheduler.
- **A file that a reader holds is deleted late**, at the first sweep after the reader is gone, which is the
  next flush or compaction and not the moment the iterator is destroyed.

## Alternatives considered

- **A reader-writer lock around the whole database.** Rejected: readers would wait for every writer. The
  read path takes the mutex for a pointer copy and leaves.
- **Several writers inserting into the memtable at once** with a concurrent skip list. Rejected: the
  leader already amortises the log and the sync, which dominate a write, and a lock-free multi-writer
  list would give up the argument that makes the memtable easy to check.
- **A lock-free queue for the writers.** Rejected: the queue is also where a flush takes its place and
  where a group is formed, and the mutex already serialises it.
- **Reclaiming old versions by epochs or hazard pointers.** Rejected: reference counting with shared
  pointers is what a reader needs and is checked by ThreadSanitizer, and the files a version names are
  kept by the weak-pointer set.
- **A thread for each compaction, or subcompactions.** Rejected for the reasons above, and for
  [ADR 0001](0001-lsm-and-leveled-compaction.md).
- **Run on the caller's thread with no background thread.** Rejected as the default, because a write
  would then do a compaction. It is what `ManualScheduler` gives a test.

## References

- `src/db/db_impl.h` (the description of the model is at the top of `DBImpl`), `src/db/db_write.cc`,
  `src/db/db_background.cc`, `src/io/skiplist.h`, `src/io/memtable.h`, `src/util/mutex.h`,
  `src/util/thread_annotations.h`, `include/shale/scheduler.h`
- Tests: `tests/stress/concurrency_test.cc`, `tests/db/db_write_test.cc`, `tests/io/skiplist_test.cc`,
  `tests/io/memtable_test.cc`, `tests/util/mutex_test.cc`, `tests/compile_fail/`
- [testing](../testing.md#stress) for how the stress tests are run.
