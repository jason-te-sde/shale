# 0003. The log format, and what recovery does with a damaged log

Status: accepted

## Context

After a crash the write-ahead log is the only record of the writes that no table holds yet, and what
the engine does with it at the next open decides whether acknowledged writes come back. A log that a
crash cut short is normal and must open. A log that is damaged in some other way must not be mistaken
for one that was merely cut short, because the difference is whether the engine silently drops writes
it acknowledged.

## Decision

**The format is LevelDB's log format.** A file is a run of 32768-byte blocks; a record is a 7-byte
header (a masked CRC-32C, a 2-byte length, a type) and a payload, and a record that does not fit in
the rest of a block is cut into `FIRST`, `MIDDLE` and `LAST` fragments. A block boundary is a place
where a reader that has met a defect can always start again. The exact bytes are in
[format](../format.md#the-write-ahead-log). The same format holds the MANIFEST.

**The reader separates a crash from corruption, with one rare false report that is loud, and recovery
acts on the difference.**

- The only thing that is silent is the file *ending* inside a record: inside a header, inside a record
  that would have fitted in its block, or after a `FIRST` or `MIDDLE` fragment with no `LAST`. That is
  what a writer that died leaves behind.
- Everything else is reported to the caller: a checksum that does not match, a length that leaves its
  block, a type that is not one of the four (zero included, so a zero-filled tail is reported), a
  fragment with no start, a record that is abandoned. A defect drops the rest of its block.
- A header whose length runs past the end of the file is a torn tail only if the bytes that are there
  are not a whole fragment. The reader looks for a length, up to what is left, that gives the checksum
  the header holds; if there is one, the fragment was whole and its length has been damaged, and it
  is reported (commit [`89915a1`][89915a1]).
- A report is **never benign by its position in the file**. After a defect the reader drops the rest
  of the block, which can include records that were acknowledged, so "nothing valid follows" does not
  mean "this is where the log ended".

**Recovery** (`DBImpl::OpenLocked`, `ReplayLog`) acts on that:

| What the reader says | `paranoid_checks = true` (default) | `paranoid_checks = false` |
| --- | --- | --- |
| nothing, or a record cut by the end of the file | the writes before it are recovered | the same |
| a first write beyond the one after the newest write of the tables and of the logs before it (a gap) | the writes before the gap are recovered; that log and every later one are renamed `<name>.dropped`, never deleted except by `DestroyDB()`, and the `LOG` says which and why | the same |
| a defect in the contents | `Open` fails with `Corruption`, naming the file | the writes before the defect are recovered, nothing after it is applied, in that log or in any later one; the logs from the defect on are renamed `<name>.dropped`, never deleted except by `DestroyDB()`, and the `LOG` says which and why |
| a read error | `Open` fails with `IOError` | `Open` fails with `IOError` |

A read that failed says nothing about what the log holds, so it is never taken for a defect of the
contents. A log that is set aside is never deleted except by `DestroyDB()`: the data may be there,
and may be the operator's only copy.

The gap is the one case in which recovery drops writes although no byte is damaged, and it is treated
as what a power loss leaves, whatever `paranoid_checks` says. A memtable switch does not sync the old
log ([ADR 0004](0004-durability.md), rule 2), so a power loss can take the unsynced end of the old log
and keep unsynced writes of the new one; writes are numbered one after another, so the gap shows at the
new log's first write. Nothing in the logs set aside was acknowledged as synced, because a synced write
syncs the earlier log first. To check the first log too, an edit of the MANIFEST records the newest
write that its tables hold, and not the newest the database had made.

Four further rules:

1. **A replayed log is never appended to.** Every open starts a new log. A log that ends in a torn
   record cannot be continued, because the header of the torn record goes on claiming bytes that would
   now be the first of the new records.
2. **The recovered writes go into a level 0 table before the logs they came from are deleted**, and the
   MANIFEST that names that table is durable first. Otherwise they would exist in memory only.
3. **Replay checks what it applies.** A batch that does not decode is applied not at all; a batch whose
   sequence number does not come after the last one, or would run past 2^56, is `Corruption`. (The
   memtable aborts on a repeated sequence number, so a log that repeats one is refused before it gets
   there.) The first batch of each log must also be at most one past the newest write of the tables and
   of the logs before it, or it is a gap (the table above).
4. **A failed or interrupted open loses nothing.** A file that a failed open made is removed only where the
   code can show that nothing names it (a new MANIFEST when `CURRENT` is seen to name the old one, a table
   that failed to seal, an empty new log). Any other is left, because the MANIFEST may name it; the next
   open finds it and sweeps what nothing names.

## Consequences

**What this buys.**

- A damaged length in a MANIFEST or a log can no longer make the engine drop the rest of the file
  silently. The simulator found it with read corruption armed during an open (seeds 185 and 191, about
  one run in three hundred), and both seeds are in `tests/sim/regression_seeds.txt`.
- The default is the safe one: a log that is damaged is not opened and is not guessed at.
- Lenient mode is a point-in-time recovery, and loses nothing silently: what it did not apply is in
  files that are named in the `LOG`.

**What it costs.**

- **A CRC pass at recovery**, over at most one block, and only for a file that ends inside a fragment.
- **A false report is possible, and loud.** A torn tail whose bytes happen to have the checksum of the
  whole fragment at some shorter length is reported as a damaged length. The chance is at most about
  one in 2^17 for a crashed log whose cut leaves a full block of bytes, and the result is a refused
  open (or a stop, in lenient mode), never silent loss.
- **A filesystem that leaves a zero-filled tail after a power loss** makes the default refuse to open,
  because zeros read as a record of type zero, a defect. Such a database opens with
  `paranoid_checks = false`. The simulator does not model zero-filled tails (see
  [testing](../testing.md#what-the-tests-do-not-cover)), so this is argued and tested at the level of
  one log, not shown over crashes.
- **Without checksums nothing is found.** The reader's check of a damaged length needs the checksum; a
  caller that turns checksums off gets the older, silent behaviour. The engine never does.
- **`.dropped` files accumulate.** A lenient open leaves them, and so does any open after a power loss that
  leaves a gap, one or more each time, each as large as a log. Someone must remove them by hand, or
  `DestroyDB()` does; the engine never does.
- **A gap that damage made is treated as a power loss's.** A log deleted by hand, or truncated at a record
  boundary, also leaves a gap, and recovery now stops there, where before it replayed straight past,
  which made a hole. The data after the gap is intact, so it is kept, renamed.

## Alternatives considered

- **Treat a defect with no valid record after it as the torn tail.** This was the first form of the
  rule, and it is wrong: the reader drops the rest of a block at a defect, which can include records
  that were acknowledged, so the position of a report says nothing about whether it was a crash. It
  was revised, and the contract D6 in [ADR 0004](0004-durability.md) with it (commit [`956bdc1`][956bdc1]).
- **Refuse every log that ends inside a record.** Rejected: a crash makes that the normal case.
- **Record the synced length of each log somewhere**, and treat anything beyond it as torn. Rejected: it
  changes the format and adds a second file whose own durability would have to be argued.
- **Make leniency the default**, as LevelDB does. Rejected: the cost of the default being wrong is lost
  writes, so the default is the one that cannot lose them. `paranoid_checks` is `true`.
- **Delete what a lenient open does not apply.** Rejected. An early version did, and a transient read
  error then turned into permanent loss: the reader reported a failed read, the lenient path took it for
  a defect, `Open` succeeded, and the sweep deleted that log and every later one. A read error now fails
  `Open`, and what a lenient open drops is renamed, not deleted (commit [`39793ad`][39793ad]).
- **Continue appending to the last log.** Rejected for the reason above: the torn record.

## References

- `src/io/log_format.h`, `log_reader.h`, `log_writer.h`; `src/db/db_impl.cc` (`ReplayLog`,
  `SetAsideLogs`)
- Tests: `tests/io/log_test.cc` (a cut at every byte stays silent; every one of the 16 bits of a record's
  length, 400 random single-bit damages to a length, none silent); `tests/db/db_recovery_test.cc`
  (`TornWalTailIsTruncated`, `MidWalCorruptionFailsParanoidOpen`,
  `MidWalCorruptionStopsReplayWhenNotParanoid`, `CorruptionInTheLastBlockIsNotMistakenForATornTail`,
  `ALengthDamagedPastTheEndOfTheLogIsNotMistakenForATornTail`, `ZeroFilledTailFailsParanoidOpen`,
  `AReadErrorAnywhereInOpenIsAnIOErrorAndLosesNothing`,
  `LenientOpenSetsAsideTheLogWithTheDefectAndEveryLaterLog`); `fuzz/fuzz_log_reader.cc`
- Commits: [`89915a1`][89915a1], [`956bdc1`][956bdc1], [`39793ad`][39793ad], [`b07bbcd`][b07bbcd]
  (`DestroyDB` removes `.dropped` logs).

[39793ad]: https://github.com/jason-te-sde/shale/commit/39793ada1cbde96f02a4011bd4abddf4bce2eb96
[89915a1]: https://github.com/jason-te-sde/shale/commit/89915a11fe4fc0b9c394960b096fef976f928463
[956bdc1]: https://github.com/jason-te-sde/shale/commit/956bdc18ce550d2b03c897f22722ddca47e20ddf
[b07bbcd]: https://github.com/jason-te-sde/shale/commit/b07bbcde5ca44c966500d62b8b2745e5630f7deb
