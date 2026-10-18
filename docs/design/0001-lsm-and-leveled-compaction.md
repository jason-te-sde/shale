# 0001. An LSM tree with leveled compaction

Status: accepted

## Context

shale is an embedded, ordered key-value store. It has to take a stream of small writes, serve point
reads and ordered scans, and survive a crash at any byte. Two facts about storage shape the choice. A
disk of any kind does sequential appends far better than scattered in-place updates. And a crash can
stop a write anywhere, so the cheaper it is to argue that a half-finished write is harmless, the cheaper
it is to be sure of the engine.

## Decision

A log-structured merge tree. Writes go to a write-ahead log and an in-memory sorted table (the
memtable); a full memtable is written out as an immutable sorted file (a table) in level 0; compaction
merges files down into deeper levels. Compaction is **leveled**, and it is the only policy:

- There are 7 levels. Level 0 holds tables flushed from memtables, which may overlap one another. Every
  other level holds tables whose key ranges are disjoint.
- Level 0 is compacted by **file count** (`l0_compaction_trigger`, default 4), because what level 0
  costs a read is one lookup per file, whatever their size. Level 1 and below are compacted by **bytes**:
  level 1 may hold `max_bytes_for_level_base` (10 MiB) and each level below it `level_size_multiplier`
  (10) times the level above.
- A compaction of a level below 0 takes **one file**, chosen round-robin by a per-level compact pointer
  that is stored in the MANIFEST, so successive compactions walk the level from one end to the other.
  A compaction of level 0 takes its oldest file and every file that overlaps the ones taken.
- An output file is closed at `target_file_size` (2 MiB), or when it overlaps more than ten times that of
  the level below its target (the *grandparents*), so that the next compaction of that level cannot be
  forced to merge an unbounded amount at once. It is never closed between two entries of one user key.
- A file that overlaps nothing in the next level, and little in the one after, is **moved** down by a
  MANIFEST edit alone, without being read.
- What a compaction may drop is decided by a pure function of the entries and of the snapshots that are
  open: an entry is dropped as shadowed only if no reader can see it; a deletion is dropped only if
  nothing older can lie below it.
- When level 0 grows faster than compaction empties it, writes are slowed at 8 files and wait at 12
  (`l0_slowdown_trigger`, `l0_stop_trigger`).

The picker and the filter are in `src/core/` and are pure functions, so they are tested against
oracles and replayed by the simulator.

## Consequences

**What this buys.**

- Every write is sequential: an append to the log and an insert in memory. Flush and compaction write
  whole new files.
- Every file except the log is immutable once written. A crash can therefore leave extra files, which
  recovery deletes, and can never leave a half-modified one. That is what makes the crash argument
  short: see [ADR 0004](0004-durability.md).
- A read looks in the memtables, in each level 0 file whose range covers the key, and in at most one
  file for each deeper level, and a Bloom filter lets it skip most of those without reading a block.
- The invariant that a level's files are disjoint is checkable: `ApplyEdit` refuses an edit that breaks
  it, and the simulator checks the Version after every step.

**What it costs.**

- **Write amplification.** A byte is rewritten as it moves down, up to about the level multiplier for each
  level it passes in the worst case. The engine counts it (`shale.write-amplification`) and
  `shale_bench` reports it; the measured figure is in [benchmarks](../benchmarks.md).
- **Stalls.** If compaction cannot keep up, level 0 fills, and writes slow and then wait. With one
  background thread ([ADR 0006](0006-concurrency-model.md)) a long compaction also delays the next
  flush; a compaction writes out the waiting memtable between its output files to bound that.
- **Space.** An overwritten or deleted entry stays until a compaction reaches it. A deletion is dropped
  conservatively (only in the oldest snapshot stripe, and only if no deeper level can hold the key), so
  a tombstone that nobody needs can outlive its use for as long as an old snapshot is held.
- **Static level sizes.** The limits of the levels are fixed by the options and not adjusted to the size
  of the data, so a database whose last level is mostly empty wastes capacity on the levels above it.
- **Read amplification for a miss.** A key that is absent costs a filter probe in every level 0 file
  that covers it and in each level, and a data block read each time a filter says "maybe".
- **`CompactRange` is not free.** It rewrites every file it touches, including a lone file that could have
  moved down whole, because it is asked for in order to give space back.

## Alternatives considered

- **A B-tree with in-place updates.** Rejected. Every update is a random write into a page, a crash in
  the middle of one tears the page, and the engine then needs either a write-ahead log *and* a way to
  protect against torn pages, or copy-on-write all the way to the root. An LSM tree needs the log and
  nothing more, because it never modifies a file after it is written.
- **Universal (size-tiered) compaction.** Rejected. It writes less per byte, but a read has to consult
  every overlapping run, and the space a database takes can be a multiple of its data while runs wait
  to be merged. It also gives the invariant checker less to say: the "levels are disjoint" property that
  leveled compaction gives is the one that the version-state rules and the simulator hold the engine
  to. Out of scope; see [ADR 0007](0007-scope.md).
- **FIFO compaction.** Rejected: it drops the oldest data to bound space, which is a cache policy and not
  a storage engine's.
- **Compaction triggered by reads that seek (LevelDB's seek compaction).** Rejected. It puts mutable state
  on the read path, and makes when a compaction happens depend on the pattern of reads, which a
  simulator would have to model. Size and file count are enough to bound read cost.
- **Dynamic level sizes.** Rejected for now. They need the picker to know the size of the data, and a
  picker that is a function of the table set and the options alone is easy to state and to test.
- **Several compaction threads or subcompactions.** Rejected, for the reason in
  [ADR 0006](0006-concurrency-model.md): one background task at a time is what lets the picker ignore
  compactions in flight and lets the concurrency argument be written down.

## References

- `src/core/compaction_picker.h`, `src/core/compaction_filter.h`, `src/db/compaction_job.h`
- Tests: `tests/core/compaction_picker_test.cc`, `tests/core/compaction_filter_test.cc`,
  `tests/db/compaction_job_test.cc`, `tests/db/db_compaction_test.cc`; the invariants of the
  simulator in [testing](../testing.md).
- Flaws that the simulator is shown to catch because of this design: `drop_tombstones_early` and
  `ignore_snapshots_in_compaction`.
