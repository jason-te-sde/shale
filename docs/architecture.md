# Architecture

shale is an embedded LSM-tree key-value store. This document says how the code is laid out, what each
layer may depend on, and what happens, step by step and file by file, on every path that matters:
a write, a read, a flush, a compaction, a recovery and the deletion of files nobody needs. The
reasons for the big decisions are in the [design records](design/); the bytes of every file are in
[format.md](format.md).

## Contents

- [The shape of it](#the-shape-of-it)
- [Layers](#layers)
- [Threads](#threads)
- [The write path](#the-write-path)
- [The read path](#the-read-path)
- [Flush](#flush)
- [Compaction](#compaction)
- [Recovery](#recovery)
- [Deleting obsolete files](#deleting-obsolete-files)
- [Closing](#closing)
- [Where to look](#where-to-look)

## The shape of it

```
 write                                              read
   │                                                  │
   ▼                                                  ▼
 queue of writers ── leader forms a group        mem ─► imm ─► Version (L0 .. L6)
   │                                                  │         │
   ▼                                                  │         ▼
 write-ahead log ──► memtable (skip list)             │     table cache ─► block cache
   (fsync if sync)       │ full                       │         │
                         ▼                            │         ▼
                    immutable memtable ──flush──► L0 ─┘     bloom filter ─► index ─► data block
                                                 │
                                       compaction (one background thread)
                                                 ▼
                                           L1 ─► L2 ─► ... ─► L6
```

A write is appended to the **write-ahead log** and inserted into the **memtable**, an in-memory
sorted structure. When the memtable is full it becomes **immutable** and a background task writes it
to disk as a sorted, immutable **table** (an SSTable) in **level 0**. Level 0 tables may overlap one
another; the tables of every deeper level hold disjoint key ranges. **Compaction** merges tables of
one level into the next, which is what keeps the number of tables a read has to look in small and
gives back the space of overwritten and deleted entries. The set of live tables is a **Version**, an
immutable value; the **MANIFEST** is the log of the edits that turn one Version into the next, and
`CURRENT` names the live MANIFEST.

Every entry has a sequence number. An update does not replace the entry it supersedes and a deletion
does not remove one: they are added next to it, and a read picks the newest entry it may see. That is
what lets readers run with no lock against the writer, and what lets a snapshot read an older state.

## Layers

```
                    shale_db     DBImpl, VersionSet, TableCache, compaction job, DB iterator
                   /        \
          shale_core        shale_io     files, log, tables, memtable, caches, Env, schedulers
      (a pure kernel)           |
                   \        /
                    shale_util   Status, coding, CRC-32C, arena, random, internal keys, mutex
```

| Layer | Directory | May depend on | What is in it |
| --- | --- | --- | --- |
| public API | `include/shale/` | `shale/*` and the standard library | `DB`, `Options`, `WriteBatch`, `Iterator`, `Status`, `Env`, `Scheduler` |
| util | `src/util/` | `util/*`, `shale/*`, the standard library | `Status`, varints and fixed integers, CRC-32C, hash, arena, `Random`, internal-key format, file names, `Mutex` with thread-safety annotations, `SHALE_CHECK` |
| io | `src/io/` | `util/*`, `io/*`, `shale/*`, the standard library, POSIX headers | the `Env` implementations (`PosixEnv`, `MemEnv`), log writer and reader, skip list and memtable, blocks, Bloom filter, table builder and reader, merging and two-level iterators, LRU cache, schedulers |
| core | `src/core/` | `util/*`, `core/*`, `shale/status.h`, the standard library, and **none** of `<thread>`, `<mutex>`, `<shared_mutex>`, `<condition_variable>`, `<atomic>`, `<chrono>`, `<fstream>`, `<cstdio>`, `<iostream>`, `<future>`, `<random>` | version edits and their encoding, the table-set state and `ApplyEdit`, the compaction picker, the compaction filter |
| db | `src/db/` | anything under `src/` and `include/` | `DBImpl` (open, write path, reads, flush, compaction scheduling), the version set and MANIFEST, the table cache, the compaction job, the DB iterator, properties |

Two rules do the real work.

**`shale_core` is pure.** It does no I/O, reads no clock, starts no thread and takes no lock. It decides
*what* to do (which files a compaction should merge, which entries of its output survive, whether an
edit to the table set is legal) and `shale_db` does it. A pure kernel can be tested exhaustively
against an oracle, and the simulator can replay a decision from a seed. It links `shale_util` and
nothing else, so a core source that reached for the I/O layer would fail to link, not merely fail a
review. See [ADR 0002](design/0002-pure-core-and-injected-env.md).

**Everything else reaches the world through `Env` and `Scheduler`.** Files, directories, locks and the
clock go through `Env` (`include/shale/env.h`); background work goes through `Scheduler`
(`include/shale/scheduler.h`). The engine never opens a file itself and never starts a thread itself.
That is what lets a test replace the filesystem with `MemEnv` or a fault-injecting one, and the thread
with a driver that decides when each task runs.

These rules are checked three ways: by link dependencies in `src/CMakeLists.txt`,
by [`scripts/check-layering.sh`](../scripts/check-layering.sh), which reads every `#include` (and
also checks that nothing under `src/` or `include/` writes to standard output or standard error,
`src/util/check.cc` excepted, that nothing under `src/` or `include/` throws, and that no C++ under `src/`, `tests/`
or `tools/` uses `std::rand` or `std::random_device` or includes `<random>`), and by a test of the
script itself that shows each rule alarming and passing (`tests/scripts/layering_test.sh`).

## Threads

A database is used from any number of foreground threads. Its background work, flushing and
compacting, runs one task at a time on a single `Scheduler`: by default a worker thread that the
database owns (`NewThreadScheduler()`), or a `ManualScheduler` that a test drives from its own thread.
One background thread makes the concurrency argument writable: only one task at a time makes tables,
and the picker never has to see a compaction that is in flight. The locks, and why there are so few,
are in [ADR 0006](design/0006-concurrency-model.md).

## The write path

`src/db/db_write.cc` (the queue and the commit) and `src/db/db_background.cc` (making room).

1. **Validate.** `DBImpl::Write` returns `InvalidArgument` for a null batch or one with a key over
   64 KiB or a value over 64 MiB (checked before anything is touched, so nothing is logged and no
   sequence number is taken), then takes `mutex_`. If a background error is set, it returns that error;
   an empty batch returns OK.
2. **Queue.** The write puts a `Writer` on the stack and in `writers_`, and waits until a leader has
   written for it (`done`) or it reaches the front of the queue.
3. **Make room** (the front writer, the *leader*). `MakeRoomForWrite(false)` loops, with the mutex held
   except where it says:
   1. A background error is returned as it is.
   2. If level 0 holds `l0_slowdown_trigger` files or more and this write has not been slowed, it
      gives up the mutex and sleeps for 1 ms through `Env::SleepForMicroseconds`, once per write.
   3. If the memtable is below `write_buffer_size`, there is room: return.
   4. If the previous memtable (`imm_`) is still waiting to be written out, wait for the background
      task, and start again. This is a **write stop** (`write_stops` counts it).
   5. If level 0 holds `l0_stop_trigger` files or more, wait the same way.
   6. Otherwise `SwitchMemTable()`: take a new file number and create the new log; keep the old log
      open if it holds unsynced writes (`unsynced_log_`), and mark the new log's directory entry as not
      yet durable (the first synced write after the switch syncs both before it is acknowledged, and
      the flush of the old memtable makes both unnecessary); note the newest write so far, which the
      flush will record (`imm_last_sequence_`); make the memtable `imm_`; start a new one;
      `MaybeScheduleBackgroundWork()`. **Nothing is synced.** A failure to create the log is a
      background error.
4. **Form a group.** `CommitGroup` first refuses a leader whose batch alone needs more sequence
   numbers than are left (`Corruption`, which stops no other write). `BuildBatchGroup` then takes the
   writers queued behind the leader, in order, while the group stays within 1 MiB (the leader's size
   plus 128 KiB if its batch is 128 KiB or less), while the sequence numbers last, and **never a
   writer that asks for a sync behind a leader that does not**; a request to flush ends a group.
   Every batch of the group is numbered after the one before it, and the group's batch is theirs joined.
5. **Log, sync, apply.** With the mutex given up, so that readers and the next writers are not held, a
   leader whose group asks for a sync first makes every earlier write durable: if the last switch left an
   old log with unsynced writes it syncs that log and closes it, and if the new log's directory entry is
   not yet durable it syncs the directory. Then the leader appends the group's batch to the log as one
   record (`log::Writer::AddRecord`, `src/io/log_writer.cc`), calls `Sync()` on the log file if it asked
   for a sync, and inserts the batch into the memtable (`WriteBatchInternal::InsertInto`, `src/io/write_batch.cc`, then
   `MemTable::Add`, `src/io/memtable.cc`). The log comes first: a write that a reader can see is one
   that the log holds. Only the leader inserts, and the memtable aborts if two overlap.
6. **Publish.** With the mutex held again the leader counts the group, and if the append or the sync
   failed it records the error as the **background error** (`RecordBackgroundError`) and every later
   write returns it. Otherwise it advances `last_sequence`, *after* the memtable has all of the
   group, so that a reader that takes the last sequence number sees the whole group or none of it.
7. **Wake.** `FinishGroup` pops the group off the queue, gives every other member the one status,
   wakes them, and wakes the writer now at the front, who leads next.

## The read path

`src/db/db_impl.cc` (`Get`, `NewIterator`), `src/db/version_set.cc` (`Version::Get`),
`src/db/table_cache.cc`, `src/io/table.cc`.

**Point read (`DBImpl::Get`):**

1. Under `mutex_`, take the sequence number to read as of (the snapshot's, or `last_sequence`) and a
   shared pointer to each of the memtable, the immutable memtable and the current `Version`, all three
   in one hold of the mutex, so that a flush finishing in between cannot leave the read with an
   `imm_` that is gone and a Version that lacks its table. Release the mutex; the read holds no lock
   from here on.
2. Build a `LookupKey` (user key and sequence number) and look in the memtable (`MemTable::Get`),
   then in the immutable memtable. A value or a tombstone ends the read: a tombstone is `NotFound`.
3. `Version::Get`: **level 0**, newest file first and only the files whose key range covers the key
   (level 0 files overlap, and a newer file holds the newer entries); then each of **levels 1 to 6**
   in order, where the files of a level are disjoint, so a binary search finds the one file that can
   hold the key. The search ends at the first entry for the key, a tombstone included.
4. For each file, `TableCache::Get` finds the open table (opening it on first use) and `Table::Get`:
   1. Ask the table's Bloom filter about the *user key*. If it says no, the table is done and no data
      block is read.
   2. Seek the index block for the first data block whose last key is at or after the lookup key.
   3. Read that data block through the block cache (`ReadBlock`, `src/io/table_format.cc`, checks the
      trailer's checksum when `verify_checksums` is set, and only a block that was checked is put in
      the shared cache), seek in it, and compare user keys.
   4. Return found, deleted or absent. A table that cannot be read returns `Corruption` or
      `IOError`, and **never** `NotFound`: a table that cannot be read cannot say that a key is
      absent.

**Scan (`DBImpl::NewIterator`):**

1. The same pinned view as above: the sequence number, the memtables and the Version.
2. Build one child iterator per source: the memtable, the immutable memtable, every level 0 file (newest
   first), and one concatenating iterator for each deeper level that has files, which opens a file as
   the iteration reaches it. `MergingIterator` (`src/io/merging_iterator.cc`) merges them into one
   stream of internal keys.
3. `NewDBIterator` (`src/db/db_iter.cc`) folds that stream into the user's view as of the sequence
   number: of the entries of a user key, the newest one at or below it, and nothing for a key whose
   newest such entry is a deletion.
4. The iterator pins the Version and the memtables for as long as it lives, so that a flush or a
   compaction cannot delete a file it still reads, however long the scan takes. It must be destroyed
   before the database, which aborts otherwise.

## Flush

`DBImpl::FlushImmutable`, `src/db/db_background.cc`; `BuildTable` and `SealTableFile`,
`src/db/builder.cc`. It runs as a background task whenever `imm_` is set, and has priority over
compaction because a write may be stalled for it.

1. Take a file number for the table and record it in `pending_outputs_`, so that the deletion of
   obsolete files leaves it alone until an edit names it.
2. With the mutex given up, iterate `imm_` into a `TableBuilder` (`BuildTable`): data blocks, the
   filter, the index, the footer.
3. `SealTableFile`: `Sync()` the file, close it, **open it again through the table cache** to see that
   it is a table that can be read, and only then sync the directory. Any failure removes the file.
4. With the mutex held again, `VersionSet::LogAndApply` an edit that adds the table to level 0, sets
   `log_number` to the current log, which makes every earlier log obsolete, and sets `last_sequence` to
   the newest write of the memtable (taken at the switch), which is what recovery holds the first log it
   replays to. `LogAndApply` prepares the edit with the mutex held, gives the mutex up while it writes
   the MANIFEST, and takes it again to install the version:
   1. `ApplyEdit` (`src/core/version_state.cc`) checks the edit against the state and refuses an
      illegal one with `Corruption`.
   2. The edit is appended to the MANIFEST and the MANIFEST is **synced**, with the mutex given up.
      There is one MANIFEST writer at a time, and the version set aborts a second.
   3. Only then, with the mutex held again, is the new `Version` installed, after a check that the
      version the edit was prepared against is still the current one. A version that readers can see is
      one that a crash will not take back.
5. Close the old log if no synced write has taken it, and clear the mark that the new log's directory
   entry is not yet durable (the directory was synced by step 3, after the switch made the new log).
   Clear `imm_`, wake writers that were waiting for it, update the counters.
6. `DeleteObsoleteFiles()`: the log of the memtable just written is now below `log_number` and goes.

## Compaction

`src/db/db_background.cc` (scheduling and installing), `src/core/compaction_picker.cc` (choosing),
`src/db/compaction_job.cc` (merging), `src/core/compaction_filter.cc` (what survives).

1. **Schedule.** `MaybeScheduleBackgroundWork` queues one task if there is a memtable to flush, a
   request from `CompactRange`, or a level that is due; there is never more than one task queued or
   running (`bg_scheduled_`), and none once the database is closing or an error has stopped writes.
2. **Choose.** `BackgroundCall` runs `FlushImmutable` if there is an `imm_`, and otherwise
   `BackgroundCompaction`, which asks the pure picker for a plan (`PickCompaction`, or
   `PickRangeCompaction` for a request of `CompactRange`).
   - A level's *score* is, for level 0, the number of files over `l0_compaction_trigger`, and for the
     others its bytes over its limit: `max_bytes_for_level_base × level_size_multiplier^(level-1)`.
     The level with the highest score of 1.0 or more is chosen.
   - Level 0 takes its oldest file and every file that overlaps the ones taken, until none does, so
     that an older version of a key is never left beside a newer one in level 1. Any other level takes
     **one** file, the first whose largest key is after the level's *compact pointer*, so that
     successive compactions walk the level from one end to the other.
   - The plan also lists the files of the next level that the inputs overlap, and the
     *grandparents*: the files two levels down that the output will have to sit beside.
3. **Trivial move.** A plan whose one input has nothing to merge with in the next level, and whose
   grandparents total no more than 10 × `target_file_size`, is carried out without reading the file:
   one edit deletes it from its level and adds it to the next (`MoveFileDown`). A manual plan is never
   a move: it is asked for in order to rewrite files and give back what is dead in them.
4. **Merge.** Otherwise `CompactFiles` builds a `CompactionJob` (with the snapshots that are open,
   copied at this moment), gives up the mutex and runs it:
   1. Read the inputs, with checksums on and without filling the cache, as one stream through a
      `MergingIterator`.
   2. Pass each entry through `CompactionFilter::Next`. An entry is **shadowed**, and dropped, when
      the entry before it for the same user key is newer and in the same snapshot stripe, so that no
      reader can see it. A **tombstone** is dropped only if it is in the oldest stripe, that is no
      open snapshot is older, and no level below the output can hold the key
      (`IsBaseLevelForKey`); the rule is conservative, and a tombstone that nobody needs may be
      kept.
   3. Write the survivors to output tables. An output is closed before an entry that begins a new user
      key when it has reached `target_file_size` or overlaps more than 10 × `target_file_size` of the
      grandparents, and **never between two entries of one user key**, since the levels below 0
      must be disjoint.
   4. Each output is sealed as in a flush. After each, the job gives the database a chance to write
      out `imm_` (`FlushBetweenOutputs`), so that a write stalled for the flush does not wait for the
      whole compaction. Before each new output it asks whether the database is closing, and if so
      removes what it made and stops without an error.
5. **Install.** `LogAndApply` an edit that deletes every input, adds every output at the next level and
   moves the compact pointer (it leaves `last_sequence` as it was), in the same order as a flush: check
   with the mutex held, append and sync with it given up, install with it held again. If that fails,
   the outputs are **not** deleted, because the MANIFEST may hold the edit all the same.
6. `DeleteObsoleteFiles()` removes the inputs, unless a reader still holds a Version that names them.

## Recovery

`DB::Open`, `DBImpl::OpenLocked`, `src/db/db_impl.cc`; `VersionSet::Recover`,
`src/db/version_set.cc`. The order is the point: nothing is deleted until what replaces it is durable.

1. `ValidateOptions` refuses an out-of-range option with `InvalidArgument`, before any file is touched.
2. Create the directory if `create_if_missing`; if there is no `CURRENT` and it is not set, fail with
   `InvalidArgument` and create nothing. Take the `LOCK` (`flock`), which a second open in this
   process or another fails with an `IOError` naming the file. Only then open the `LOG`, so that a
   refused open does not rename away the log of the process that holds the lock.
3. No `CURRENT`: `VersionSet::CreateNew` writes `MANIFEST-000001`, syncs it and the directory, and
   writes `CURRENT`. Otherwise `VersionSet::Recover`: read `CURRENT` and check it
   (`ParseCurrentFile`), then replay the MANIFEST it names with the log reader, decoding each record
   as a version edit and applying it with `ApplyEdit`. A MANIFEST that ends short, or in zeros from a
   record boundary to the end of the file, as a crash leaves it, is read up to there, and the `LOG`
   names the zeros. Any defect the reader reports, any record that does not decode and any edit
   `ApplyEdit` refuses is `Corruption` naming the MANIFEST.
4. `FindLogsToReplay`: every log numbered at or above the MANIFEST's `log_number`, in ascending
   order. Every log and table number found in the directory is marked as used, so that a stray file
   is never overwritten.
5. `ReplayLog` each into **one** memtable (`ApplyRecoveredBatch`: a batch is applied whole or not
   at all, and a batch whose sequence number does not come after the last one is `Corruption`). The
   first batch of each log must be at most one past the newest write of the tables (the MANIFEST's
   `last_sequence`) and of the logs before it. If it is beyond that, a power loss took the end of an
   earlier log: replay stops, and that log and every later one are renamed `.dropped`, whatever
   `paranoid_checks` says. What the reader reports decides the rest: nothing for a log that merely
   ends short, or ends in zeros from a record boundary to the end of the file, as a crash leaves it
   (the `LOG` names the file and the number of zero bytes); otherwise `paranoid_checks` (the default)
   fails the open with `Corruption`, and without it replay stops at the first defect, applies nothing
   after it, and the logs from there on are **renamed to `.dropped`, never deleted**. A log that
   cannot be *read* fails the open with `IOError` whatever `paranoid_checks` says.
6. Move `last_sequence` to the larger of the MANIFEST's and the replay's. (The MANIFEST's is the newest
   write that its tables hold, so it is also what the first log was held to.)
7. Create a **new** log and sync the directory. A replayed log is never appended to: it may end in a
   torn record, which a writer cannot continue.
8. If anything was recovered, write it to a **level 0 table** (`WriteRecoveredMemTable`) *before* the
   logs it came from are deleted. Without this the recovered writes would be in memory only, and the
   next open would find nothing to replay.
9. `VersionSet::WriteNewManifest`: a new MANIFEST whose first record is a snapshot of the whole state
   including that table, synced, directory synced, `CURRENT` switched to it (a `.dbtmp` file written,
   synced and renamed over `CURRENT`, then the directory synced).
10. If replay stopped at a gap or, in a lenient open, at a defect, the logs from there on are set aside
    now: rename them (the later logs first, so a failure part-way never leaves a later log replayable
    behind one that was set aside) and move `log_number` past them.
11. `DeleteObsoleteFiles()`, and a check of whether the new level 0 table makes a compaction due.

## Deleting obsolete files

`DBImpl::DeleteObsoleteFiles`, `src/db/db_background.cc`. It runs after every flush, every compaction
and every open, and it is what keeps the directory from growing. It does nothing after a background
error, since whether the version that a failed edit would have installed is the one a restart finds
is not known.

1. Under the mutex, compute the set of file numbers that are **needed**: the files of every `Version`
   that is still alive anywhere (`VersionSet::LiveFiles`, which tracks versions by weak pointer so
   that one dropped by its last reader leaves the set by itself), plus the tables that are being
   written (`pending_outputs_`). Note the log number, the number of the log in use, and the MANIFEST's
   number.
2. Give up the mutex and list the directory.
3. For each name that `ParseFileName` recognises and whose number is below 2^56: a **table** goes if
   its number is not needed (and is evicted from the table cache); a **log** goes if its number is
   below `log_number` and it is not the log in use; a **MANIFEST** goes if it is not the current one;
   a `.dbtmp` file goes. `CURRENT`, `LOCK`, `LOG` and `LOG.old` are never touched, and **a name that is
   not one of the database's is left alone**, which is also what keeps `.dropped` logs and anything
   the operator put in the directory.
4. Take the mutex again and check that no table was begun while it was away (`table_makers_`),
   which would have been taken for one nobody needs. It aborts if one was.

A failure to delete a file is logged and tried again at the next sweep. A file a reader holds is
deleted at the first sweep after the reader is gone, which is the next flush or compaction, not the
moment the iterator is destroyed.

## Closing

The destructor aborts if an iterator or a snapshot is still alive, or if a write or a flush is still
in progress, since they point into what is about to be freed. It then sets `shutting_down_`, waits for
the background task that is running (a compaction stops at its next output), closes the log, and an old
log that is still waiting for a sync without syncing it (no acknowledgement depends on it, and a power
loss afterwards leaves a gap that the next open handles), and releases the lock. **Closing does not
flush**: what the memtables hold is in the logs, which the next open replays. That is deliberate, and
it is why a copy of a closed database needs no more than the next open to be whole (see
[operations](operations.md#backup)).

## Where to look

| To understand | Read |
| --- | --- |
| the public contract | `include/shale/db.h`, `options.h` (every public item has a Doxygen comment) |
| how a write becomes durable | `src/db/db_write.cc`, [ADR 0004](design/0004-durability.md) |
| the log and its defects | `src/io/log_format.h`, `log_reader.h`, [ADR 0003](design/0003-wal-format-and-recovery.md) |
| the table file | `src/io/table_format.h`, `table_builder.h`, `table.h`, [ADR 0005](design/0005-sstable-format.md) |
| which files exist | `src/db/version_set.h`, `src/core/version_state.h` |
| what compaction may drop | `src/core/compaction_filter.h`, [ADR 0001](design/0001-lsm-and-leveled-compaction.md) |
| how it is tested | [testing.md](testing.md) |
