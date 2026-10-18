# 0004. The durability contract

Status: accepted

## Context

"Crash safe" is not a property until it is a list of sentences that a test can break. This record is
that list, the order of writes and syncs that makes each sentence true, and what it costs. Every
sentence below is checked by the simulator, by a test of the engine, or both; none is argued only in
prose.

## Decision

### The contract

<!-- table:contract -->
| # | The promise |
| --- | --- |
| **D1** | With `WriteOptions::sync = true` (**the default**), a `Write` that returns OK is present after a crash of the process and after a loss of power. This assumes the device does what `fsync` asks of it; on macOS the engine uses `F_FULLFSYNC`. |
| **D2** | With `sync = false`, a write that returns OK is present after a crash of the process. After a loss of power, the state recovered is the state after some **prefix** of the acknowledged writes, and that prefix includes every write up to the last one that was synced. There are no holes: a write is never recovered when an earlier one is not. |
| **D3** | A `WriteBatch` is, after any crash, entirely visible or entirely invisible. |
| **D4** | A read returns the right value or an error (`Corruption`, `IOError`). It never returns a wrong value, including after a bit flips on the disk (`ReadOptions::verify_checksums` is `true` by default). |
| **D5** | After a background I/O error, every write returns that error until the database is opened again. Reads keep being served. The effect of the write that failed is undefined (it may or may not be there after the next open), but the batch is still atomic. |
| **D6** | Two traces of a crash are tolerated in the write-ahead logs whatever `paranoid_checks` says. The first is a record or a header **cut off by the end of a file**, which is ignored. The second is a **gap in the sequence numbers at the start of a log**: its first write is not the one after the newest write of the tables and of the logs replayed before it. That is what a power loss leaves when it takes the unsynced end of one log and keeps unsynced writes of the next. At a gap, replay stops; that log and every later one are renamed `<name>.dropped`, never read or deleted by the engine, and the `LOG` says why. Neither trace drops a write that was acknowledged as synced (D1, rule 2). Every other defect the log reader reports is an error at `Open` when `paranoid_checks` is `true` (the default): `Corruption`, or `IOError` for a read error. With `paranoid_checks = false`, replay stops at the first defect, and what is dropped is kept, renamed. A memtable switch does not sync the old log, so after a power loss the last two logs can both end in unsynced bytes. A zero-filled or garbage tail in either is a defect, not a cut-off record. See [ADR 0003](0003-wal-format-and-recovery.md). |
| **D7** | One directory is open by one `DB` at a time: a `LOCK` file held with `flock` refuses a second open, in the same process or another. |
<!-- /table:contract -->

D6 was revised twice during development. The first statement said that a defect with valid records after
it was corruption and a defect at the end was a torn tail. That is false, because the reader drops the
rest of a block at a defect and can take acknowledged records with it, so the contract was restated in
terms of what the reader can distinguish. The second revision added the gap: when a memtable switch
stopped syncing the old log (rule 2), a power loss could leave the end of one log missing and the next
log's first writes present, and a default open that refused that would leave a database unopenable
after an ordinary power loss with unsynced writes.

### The order that makes it true

Each rule below exists because a crash between two steps would otherwise break a sentence above. They
are in the code in the places named.

1. **Log, then memtable, then acknowledge** (`CommitGroup`, `src/db/db_write.cc`). The record is appended
   to the log, synced if the group asked for it, inserted in the memtable, and only then does
   `last_sequence` move, so a reader never sees a write the log does not hold. (D1, D3: a batch is one
   record, so it is in the log whole or not.)
2. **A synced write makes every earlier write durable first** (`CommitGroup`, `src/db/db_write.cc`;
   `SwitchMemTable`, `src/db/db_background.cc`). A memtable switch syncs nothing. An old log that holds
   unsynced writes is kept open, and the new log's directory entry is marked as not yet durable. The
   first synced group after a switch syncs the old log, then the directory, then appends and syncs its
   own record, and only then is acknowledged. The flush of the switched memtable makes both earlier syncs
   unnecessary: its table is synced, and its directory is synced after the switch made the new log,
   before its edit is written; on success it closes the old log. A failure of any of these syncs is a
   background error (D5).
3. **Recovery replays a prefix, and never across a gap** (`ReplayLog`, `src/db/db_impl.cc`). Writes are
   numbered one after another. An edit of the MANIFEST records the newest write its tables hold: a
   flush, the newest write of its memtable; other edits leave the value as it was. The first write of
   each log replayed must be at most one past the newest write of the tables and of the logs before it.
   A first write beyond that means the writes in between were the unsynced end of an earlier log that a
   power loss took. Replay stops there, and that log and the later ones are set aside (D6). None of them
   holds a write that was acknowledged as synced, since rule 2 syncs the earlier log before
   acknowledging one.
4. **A table is whole before anything names it** (`SealTableFile`, `src/db/builder.cc`): the file is
   synced and closed, opened again through the table cache to see that it reads, and only then is the
   directory synced.
5. **An edit is durable in the MANIFEST before the version it makes is installed** (`LogAndApply`,
   `src/db/version_set.cc`). The record is appended and synced (with the DB mutex given up, and only one
   writer at a time, which the version set enforces), then the version is installed. A version that readers see is one a
   crash will not take back, and a table a version names is one the MANIFEST names too. If the append
   or the sync fails, the MANIFEST is not trusted again until a new one is written, because a `Sync()`
   that succeeds after a failed one proves nothing.
6. **`CURRENT` changes by atomic rename and a synced directory** (`SetCurrentFile`). The new
   MANIFEST is written and synced, the directory is synced (so `CURRENT` never names a MANIFEST whose
   directory entry a crash could take back), a temporary file with the new contents is written and
   synced, it is renamed over `CURRENT`, and the directory is synced again. `CURRENT` carries a
   checksum so that a flipped bit cannot make it name another MANIFEST that exists.
7. **Nothing is deleted until what replaces it is durable** (`DeleteObsoleteFiles`, `OpenLocked`). The
   recovered writes are a level 0 table, named by a durable MANIFEST, before the logs they came from
   are removed. A compaction's inputs go only after the edit that deletes them is installed. After a
   background error nothing is deleted, since whether a failed edit reached the MANIFEST is not known.
8. **A failed sync is final** (`WritableFile`'s contract). After an `fsync` fails, the data it covered
   may have been dropped while a second `fsync` reports success. The engine does not retry: the error is
   sticky (D5), and the next open reads the log again.

On Linux the engine uses `fdatasync` for files and `fsync` for directories. On macOS neither `fsync` nor
`fdatasync` asks the drive to empty its write cache, so it calls `fcntl(F_FULLFSYNC)`, which does.

## How each sentence is checked

<!-- table:checks -->
| | By |
| --- | --- |
| D1, D2, D3 | The simulator's **I4**: after every simulated crash the reopened database must equal the model after some prefix of the writes, at least as long as the last sync, and a batch is never half there; and **I2**'s durable floor: the reopened database's last sequence number is not below that of the last synced write (`tests/testkit/invariants.h`, `tests/sim/sim_test.cc`). The flaws `skip_wal_sync`, `skip_dir_sync` and `skip_manifest_sync` each break one rule above, and each is caught. `tests/db/db_recovery_test.cc` (`PowerLossKeepsSyncedWrites`, `ProcessCrashKeepsUnsyncedWrites`, `APowerLossThatTakesTheEndOfTheOldLogLeavesAPrefix`), `tests/db/db_flush_test.cc` (`ASyncedWriteMakesTheWritesInTheOldLogDurable`, `ASyncedWriteAfterASwitchSyncsTheOldLogAndTheDirectoryFirst`). The real process kill: `shale_crashtest`. |
| D4 | The simulator's **I1**, which holds every read that returns OK to the model, including while it is flipping bits in what the Env reads, and **I5**, which allows a read to fail only where an injected fault explains it. `tests/io/table_test.cc`, `tests/io/block_test.cc`; the fuzz targets for the block and table readers. |
| D5 | The simulator's **I6**: a write succeeds after a failed one, and the run fails. `tests/db/db_basic_test.cc` (`WalSyncFailureFailsTheWriteAndEveryOneAfterIt`), `tests/db/db_flush_test.cc`, `tests/db/db_write_test.cc` (`WalSyncFailureStopsWrites`, `WalSyncFailureRecoversAfterReopen`). |
| D6 | `tests/db/db_recovery_test.cc` and `tests/io/log_test.cc`, listed in [ADR 0003](0003-wal-format-and-recovery.md), among them `ALaterLogIsSetAsideWhenAnEarlierLogLostItsEnd` and `AFirstLogThatLostEverythingIsCaughtByTheManifestsSequenceNumber` for the gap; the simulator holds an open that read corrupted bytes to I4 in full, and reaches the gap at recovery: a deliberate break that removes the check fails I4 at seed 6 (commit [`824b318`][824b318]). |
| D7 | `tests/db/db_basic_test.cc` (`OpenSamePathTwiceFails`); `tests/io/env_posix_test.cc` for the lock. |
<!-- /table:checks -->

## Consequences

**What this buys.** A short, enumerable argument. A table is written once and made whole before anything
names it. Two files grow after they are written, and each grows in one way only: the log and the
MANIFEST only have a record appended, each record checksummed. The log is covered by rule 1 (a record is
in the log before the write is acknowledged) and by [ADR 0003](0003-wal-format-and-recovery.md) (a
replayed log is never appended to), and the MANIFEST by rule 5 (an edit is synced before the version it
makes is installed), so a crash that cuts the last record of either leaves a file that ends short, which
the reader takes for its end
(`ManifestCutAnywhereAfterItsFirstRecordRecoversTheRecordsBeforeTheCut` in
`tests/db/version_set_test.cc`) and that no later write is appended to, because every open starts a new
log and a new MANIFEST. A third file, `CURRENT`, is never changed in place: it is replaced whole by an
atomic rename (rule 6). The simulator
holds the engine to D1 to D6 after every crash, not only at the end of a run.

**What it costs.**

- **The default is slow.** `sync = true` makes an acknowledged write wait for the device. Group commit
  ([ADR 0006](0006-concurrency-model.md)) shares one sync among the writers that queue, which helps
  under concurrency and not for a single writer. [benchmarks](../benchmarks.md) reports the cost
  (`fillsync`). The default is the direction in which a wrong choice does no irreversible harm;
  `sync = false` is an explicit decision to lose the last writes to a power cut.
- **More syncs than the minimum.** A directory sync follows every table and every MANIFEST, and the first
  synced write after a memtable switch syncs the old log and the directory before its own record; the
  MANIFEST is synced on every edit; a new MANIFEST is written at every open.
- **A power loss can set a log aside.** After a power loss that takes the unsynced end of one log and
  keeps unsynced writes of the next, an open sets the later logs aside as `.dropped` files (D6). They hold
  no write that was acknowledged as synced, and one or more of them can appear after each such power
  loss; the engine never deletes them ([operations](../operations.md)).
- **Two logs can end in unsynced bytes.** Because a switch does not sync the old log, a zero-filled or
  garbage tail, which the reader reports as a defect and a default open refuses, can be in either of the
  last two logs and not only the newest. The simulator's filesystem leaves clean prefixes only, and does
  not model such tails ([testing](../testing.md#what-the-tests-do-not-cover)).
- **D1 is as good as the device.** A drive that acknowledges a flush it has not done breaks D1 and D2
  and nothing in the engine can tell. The simulator models "unsynced bytes are lost" and does not model
  a device that lies.
- **macOS `F_FULLFSYNC` is far slower than `fsync`.** The numbers for the two systems are not
  comparable, and [benchmarks](../benchmarks.md) says which system they were taken on.
- **After a background error the database must be reopened.** Reads continue, but there is no way to
  clear the error in place.

## Alternatives considered

- **`sync = false` as the default**, as LevelDB and RocksDB have it. Rejected: an engine whose default
  can lose an acknowledged write has chosen speed over the one property that makes it a database.
- **`O_DSYNC` or `O_SYNC` on the log file** instead of `fdatasync` after the write. Rejected: it gives up
  the choice of syncing a group once, which is the point of group commit.
- **Sync the old log and the directory at every memtable switch, on the writers' path.** This was the first
  design, and it is simpler to argue. Rejected for what it costs the writer that makes the switch: it is a
  sync of the old log and one of the directory with the DB mutex held, shown in the profiles in
  [benchmarks](../benchmarks.md). The first synced write after the switch does the same work only when
  somebody asked for durability, and the flush makes it unnecessary when nobody does (commit [`824b318`][824b318]).
- **Let a background thread sync the old log while writes go into the new one.** Rejected: it leaves the
  same hole for recovery to find, and adds a thread that touches the log.
- **Delete the logs after a gap.** Rejected: they hold writes that were acknowledged, if not synced, and
  an operator may want them back; they are renamed, not deleted.
- **Retry a failed `fsync`.** Rejected. A second `fsync` can succeed on data the first one failed to
  write and then dropped, so a retry would acknowledge writes that are not on the disk.
- **Assume a filesystem persists directory changes in order.** Rejected: the engine syncs the directory
  at each point where it matters (a new MANIFEST before `CURRENT` names it; the renames of a lenient open
  before the one that hides the log with the defect) and does not rely on the order. The simulator's
  filesystem keeps directory changes as a seeded in-order prefix, and the flaw `skip_dir_sync` shows the
  syncs matter.
- **Rely on `rename` alone for `CURRENT`.** Rejected: the rename is atomic but not durable until the
  directory is synced, and the name it publishes must exist on disk when it does.
- **A `CURRENT` without a checksum.** Rejected after it failed: a misread of `CURRENT` made a failed switch
  delete the MANIFEST that `CURRENT` named (seed 2240, commit [`cacf403`][cacf403]), and a flipped
  digit selected an older MANIFEST that was still in the directory (commit [`d8c7e7b`][d8c7e7b]).

## References

- `src/db/db_write.cc`, `src/db/db_background.cc`, `src/db/builder.cc`, `src/db/filename_io.cc`,
  `src/db/version_set.h` (the two orders it names), `include/shale/env.h`
- [testing](../testing.md) for the invariants I1 to I6 and the injected flaws.

[824b318]: https://github.com/jason-te-sde/shale/commit/824b3189b6c0600e8b39c4d9a0a287831eda8d5e
[cacf403]: https://github.com/jason-te-sde/shale/commit/cacf403bbc7d37e67e02db58e74334de391a5ef9
[d8c7e7b]: https://github.com/jason-te-sde/shale/commit/d8c7e7bd50067c4de9f259b5fc9facdda5ca1848
