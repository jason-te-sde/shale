# Operating shale

This is written to be read at 3 a.m. with a pager going off. Start with the table, do what the row
says, and read the section it points to. Everything here is about a database that is already in
use; for how it works, see [architecture](architecture.md).

## Contents

- [What is wrong, and where to read](#what-is-wrong-and-where-to-read)
- [Rules that always hold](#rules-that-always-hold)
- [What each status means](#what-each-status-means)
- [A database that will not open](#a-database-that-will-not-open)
- [Corruption found while reading](#corruption-found-while-reading)
- [Writes all fail: a sticky background error](#writes-all-fail-a-sticky-background-error)
- [Disk full](#disk-full)
- [Writes are slow, or stall](#writes-are-slow-or-stall)
- [Tuning](#tuning)
- [Backup](#backup)
- [shalectl](#shalectl)
- [Reading the LOG](#reading-the-log)
- [Properties and counters](#properties-and-counters)
- [The files in the directory](#the-files-in-the-directory)

## What is wrong, and where to read

| You see | It means | Read |
| --- | --- | --- |
| `IO error: lock .../LOCK: already held by another process or handle` | another process (or another `DB` in this process) has the directory open | [A database that will not open](#a-database-that-will-not-open) |
| `Corruption: WAL corruption in .../NNNNNN.log` at open | a write-ahead log has a defect that is not a crash's torn tail | [A database that will not open](#a-database-that-will-not-open) |
| `Corruption: CURRENT ...` or a `Corruption` that names a `MANIFEST-...` at open | the file that says which tables are live is damaged | [A database that will not open](#a-database-that-will-not-open) |
| `Corruption: block checksum mismatch: .../NNNNNN.sst` on a read | one table file has a damaged block | [Corruption found while reading](#corruption-found-while-reading) |
| every `Put`, `Write` and `Flush` returns the same error, reads still work | a background error has stopped writes | [Writes all fail](#writes-all-fail-a-sticky-background-error) |
| `IO error: write ...: No space left on device` | the disk is full | [Disk full](#disk-full) |
| writes are slow, `write_slowdowns` or `write_stops` is climbing | compaction cannot keep up with the writes | [Writes are slow, or stall](#writes-are-slow-or-stall) |
| `Invalid argument: <option> must be ...` at open | an option is out of range | [Tuning](#tuning) |

## Rules that always hold

1. **Never delete, rename or edit a file in the database directory by hand** to "fix" something. The
   engine knows which files are live; a table that looks stray may be named by the MANIFEST, and a log
   that looks old may hold writes that no table does. The one exception is a `.dropped` log, below, and
   only after you have decided you do not want what is in it.
2. **One process at a time.** The `LOCK` file refuses a second open. Do not work around it: two
   processes writing to one directory corrupt it.
3. **Work on a copy** of a database that you are investigating. Every `shalectl` command except `verify`
   and the dumps *opens the database for writing* (it replays the log into a table, writes a new
   MANIFEST and deletes files that are no longer needed). That is the right thing for a healthy
   database and the wrong one for a damaged database you want to keep exactly as it is.
4. **An error is never a wrong answer.** A read returns the right value or an error. If the engine
   returns a value, you can trust it (as far as the disk honoured its syncs); if it returns
   `Corruption`, do not guess what was meant.
5. **A successful write is durable by default.** `WriteOptions::sync` is `true` unless you changed it.
   If you set it to `false`, a power cut can lose the most recent writes; a crash of the process cannot.

## What each status means

Every error is a `Status`; `ToString()` begins with the code. The text after it names the operation,
the file and, for an I/O error, the operating system's own description.

| Code | Comes from | Means | Do |
| --- | --- | --- | --- |
| `Invalid argument` | a caller's mistake | The call was refused and **nothing was done**: a key over 64 KiB or a value over 64 MiB, a null batch, an option out of range (`write_buffer_size must be at least 16384: got 100`), a path with no database and no `create_if_missing`, a path that has one and `error_if_exists`. | Fix the call. No repair needed. |
| `NotFound` | `Get` only | The key has no value, or was deleted. This is **not an error of the database**. | Nothing. |
| `Corruption` | stored data | A checksum or a format check failed, or the data contradicts itself. The message names the file where it can. The engine does **not** return a wrong value instead. | See the sections below. Do not retry in a loop: the bytes will not change. |
| `IO error` | the operating system | A call failed: `open`, `read`, `write`, `sync`, `rename`, `remove`, `lock`. The text is `<operation> <path>: <what the OS said>`, for example `write /var/db/000012.log: No space left on device`. | On a write: [sticky error](#writes-all-fail-a-sticky-background-error). At open: nothing was lost, fix the cause and retry. On a read: that read failed; retry it. |
| `Not supported` | no operation of the database returns it today | Reserved. | - |

How an error behaves depends on where it happened:

- **At `Open`**: the open fails and `*db` is null. Nothing that was in the directory has been lost. A
  new log, table or MANIFEST that the failed open made may be left, and the next open sweeps them.
- **On a read**: that read fails. The database is otherwise unchanged and keeps serving.
- **On the write path** (log append or sync, switching to a new log, a flush, a compaction, the MANIFEST):
  the first such error becomes the **background error**. Every later write returns it, until the
  database is opened again. Reads continue. See [Writes all fail](#writes-all-fail-a-sticky-background-error).
- `Corruption` that says `no sequence numbers are left` or `no file number is left` is a database that has
  taken 2^56 writes, or whose files say it has. A real database does not reach it; one that reports it has
  a forged or damaged MANIFEST.

## A database that will not open

Look at the **last lines of `LOG`** in the directory (the engine writes `open failed: <status>` there)
and at the status `Open` returned. Then:

**`IO error: lock .../LOCK: already held by another process or handle`.** Something has the database
open: another process, or a second `DB` in this one. Find it and stop it. The lock is released when
its holder exits, however it exits; there is no stale lock to remove. `shalectl verify` also takes this
lock, so it refuses an open database too.

**`Invalid argument: ... does not exist (create_if_missing is false)`** or **`... exists
(error_if_exists is true)`.** The options say so. Check the path.

**`Invalid argument: <option> must be ...`.** An option is outside the range in [Tuning](#tuning). Nothing
was touched.

**`Corruption: WAL corruption in .../NNNNNN.log: ...`.** A write-ahead log has a defect that is not
the signature of a crash. A crash leaves a log that **ends** short; this is something else: a checksum
that does not match, a length that leaves its block, a record type that does not exist, a tail of
zero bytes, or a sequence number that goes backwards. The default (`paranoid_checks = true`) refuses
to guess. Your choices:

1. **Restore from a backup** and replay what you can from your own record of the writes. This is the
   choice that loses nothing the backup had.
2. **Open with `paranoid_checks = false`** on a *copy first*. `shalectl` has no flag for it: it takes a
   small program that sets `Options::paranoid_checks` to `false` and calls `DB::Open`. The open then
   recovers every write **before the first defect**, applies nothing after it (in that log or in any
   later one), and **renames** the log with the defect and every later log to `NNNNNN.log.dropped`. It
   deletes none of the logs that it drops. (Like every open, it turns the logs it did replay into a
   table and starts a new MANIFEST, and then removes what those replace.) The `LOG` says which files
   and why. The writes in those files are what you lose, and they are still
   on disk, so you can read them with `shalectl dump-wal`. The database is a point-in-time recovery: it
   holds a consistent prefix of the writes.
3. To read what the log still holds before deciding, `shalectl dump-wal NNNNNN.log` prints every record
   it can read and reports each defect, on a copy or on the original: the dumps do not open the database.

**`Corruption: ... CURRENT ...`** (`CURRENT file does not end with newline`, `CURRENT file is malformed`,
`CURRENT checksum mismatch`, `CURRENT names an invalid file`) or **a `Corruption` that names a
`MANIFEST-NNNNNN`** (`the MANIFEST holds no record`, `VersionEdit: ...`, a defect the log reader
reports, or an edit that breaks a rule of the table set). These two files say which tables are live,
and there is **no repair tool** and no safe way to rebuild them by hand. Restore from a backup. If
there is none, the tables and the logs are still there and can be read one file at a time:

```
shalectl dump-sst 000012.sst
shalectl dump-wal 000019.log
```

`dump-sst` prints every entry of a table (key, sequence number, put or delete, value), checking every
block; `dump-wal` prints the writes of a log that no table holds yet, and reports each defect. A new
database can be loaded from them in sequence-number order, newest last. This is salvage and not
recovery, and it is your job to decide which entries are current.

**`IO error` while opening.** A read of the log, the MANIFEST or a table failed. That says nothing about
what the files hold, so it is not treated as damage: **nothing is dropped and nothing is deleted**, and
the next open reads again. Check the disk and the permissions and retry; it is the same on every setting
of `paranoid_checks`.

**A filesystem that left zeros at the end of a log after a power loss** (a tail of `0x00`) is reported
as a defect, so the default refuses the open. If you know that was a power loss and not damage, open
with `paranoid_checks = false`; the writes before the zeros are recovered. A memtable switch does not
sync the old log, so after a power loss such a tail can be in either of the last two logs.

**A `.dropped` file that you did not ask for, and a `LOG` line that says a log "lost the end of an
earlier log".** This is not a failure: `Open` succeeded. After a power loss with unsynced writes (made
with `sync = false`), the end of one log can be gone while unsynced writes of the next log are still on
the disk. Writes are numbered one after another,
so the open sees that the first write of the later log is not the one after the last write before it:
a gap. It recovers everything before the gap, applies nothing from there on, and renames that log and
every later one to `NNNNNN.log.dropped`, with `paranoid_checks` on or off, because this is what a power
loss leaves and not damage. The database is a consistent prefix of the writes, which is what D2 promises.
What is in the dropped files is **only** writes made after the last one acknowledged as synced:
nothing that was acknowledged as synced is in them. The engine never reads or deletes a `.dropped` file
again. You may delete it when you have decided that you do not want those unsynced writes; until then,
`shalectl dump-wal NNNNNN.log.dropped` prints them. One or more such files can appear after each power
loss that leaves a gap, and they take the space of the logs they were.

## Corruption found while reading

`Corruption: block checksum mismatch: .../NNNNNN.sst` (or `bad block contents`, `truncated block read`,
`data block does not hold the last key ...` and similar) means a table file has changed since it was
written: a bad sector, a bad copy, a bit flip. The engine will not return a value from that block, and
it will not say a key is *absent* either, since a table that cannot be read cannot say so: the read
returns `Corruption`. Writes and reads that do not touch that block are unaffected.

1. Find every damaged table: **`shalectl verify --db=DIR`** on a copy (or with the database closed). It
   reads the MANIFEST and every live table with checksums on and prints one line for each problem,
   naming the file.
2. Restore the database, or the damaged table, from a backup. Replacing a single `.sst` is only safe if
   the replacement is the same file, byte for byte, that the MANIFEST describes; `verify` checks the
   size and the key range against the MANIFEST, so run it again afterwards.
3. A compaction that reads a damaged table stops with the same `Corruption`, and that is a background
   error: the database then stops writes (below). The tables are read with checksums on and are not
   copied around damaged.

`ReadOptions::verify_checksums` defaults to `true`. Turning it off skips the check, so a flipped bit in a
value can come back to the caller as a value; nothing else in the engine depends on it being off.

## Writes all fail: a sticky background error

**Symptom.** `Put`, `Delete`, `Write` and `Flush` all return the same non-OK status, again and again.
`Get` and iterators still work. `GetProperty("shale.background-error")` returns that status instead of
`OK`. The `LOG` has a line `write path stopped: <status>`, and the `bg_errors` counter is 1.

**What happened.** One of the things the write path does failed: appending to or syncing the log,
creating the next log, writing a memtable out as a table, a compaction, or writing the MANIFEST. The
engine cannot know what state a file is in after a failed write or sync (a second `fsync` can report
success for data the first one lost), so it **stops accepting writes** rather than acknowledge one that
may not be on the disk. The first error is kept; later ones do not replace it.

**What to do.**

1. Read the status and the `LOG`. It names the operation and the path. The cause is nearly always the
   operating system's: the disk is full, the volume went read-only, a permission changed, the device is
   failing.
2. Fix the cause.
3. **Close the database and open it again** (restart the process). There is no way to clear the error in
   place, on purpose. Opening replays the log: every write that was acknowledged is recovered.
4. The write that failed **may or may not be there** after the reopen, but a `WriteBatch` is still all
   or nothing.
5. If the same error comes straight back at open, it is a damaged file and not a transient cause: see
   [A database that will not open](#a-database-that-will-not-open).

Reads that are served while the database is stopped are correct; they are held to the same check as
reads at any other time.

## Disk full

A full disk shows up as an `IO error` whose text ends in `No space left on device`, from a `write`,
`sync` or `open`, on the log, a table or the MANIFEST. Whichever it hits first, the result is a
[sticky background error](#writes-all-fail-a-sticky-background-error): writes stop with that error and
reads go on. A table or MANIFEST that was half written is removed by the code that was writing it where
it can be, and the next open sweeps what is left.

1. **Free space on the filesystem** without touching the database directory (remove other things,
   expand the volume). Do not delete database files to make room.
2. **Close the database and open it again.** A write that failed on the log leaves the log ending in a
   cut record, which is what a crash leaves and is not an error at open.
3. After it opens, the database is whole. Everything that was acknowledged is there.

Keep headroom. A compaction writes its output tables **before** it deletes its input tables, so it needs
room for both at the same time, and a flush needs room for a table of the size of the write buffer. A
database that is allowed to fill its disk will fail in a flush or a compaction first, and that is this
error, and not a write that is refused politely.

## Writes are slow, or stall

Two things slow writes down on purpose, both because level 0 is growing faster than compaction empties
it (see [architecture](architecture.md#compaction)):

- At `l0_slowdown_trigger` files (default 8) in level 0, each write sleeps 1 ms once. The counter
  `write_slowdowns` goes up.
- At `l0_stop_trigger` files (default 12), a write that needs a new memtable **waits** until a compaction
  has reduced level 0, and a write also waits while the previous memtable is still being written out.
  The counter `write_stops` goes up.

`shalectl stats` (or `GetProperty("shale.stats")`) shows the files in each level and the counters.
`shale.num-files-at-level0` near the triggers, with `write_slowdowns` and `write_stops` rising, is this.
A database that stalls once in a while is fine. One that stalls all the time is being written faster than
one background thread can compact, and the choices are in [Tuning](#tuning): a larger write buffer makes
fewer, larger level 0 files; a lower `l0_compaction_trigger` makes compaction start sooner. Writing less,
or batching more writes into one `WriteBatch`, helps most.

A write with `sync = true` waits for the disk, so it is slower than one with `sync = false` by the cost
of a flush of the device's cache; concurrent writers share one sync. The cost of a sync is the disk's.

## Tuning

Every option is in `include/shale/options.h`. `ValidateOptions()` states the range of each, and `Open`
refuses an option outside it with `Invalid argument` before it touches a file. The defaults and the
ranges below are checked against the code by
[`tests/docs/operations_doc_test.cc`](../tests/docs/operations_doc_test.cc).

<!-- table:options -->
| Option | Default | Minimum | Maximum |
| --- | --- | --- | --- |
| `write_buffer_size` | 4194304 | 16384 | - |
| `block_size` | 4096 | 256 | 4194304 |
| `block_restart_interval` | 16 | 1 | 1024 |
| `bloom_bits_per_key` | 10 | 0 | 30 |
| `block_cache_capacity` | 8388608 | - | - |
| `max_open_files` | 1000 | 20 | - |
| `l0_compaction_trigger` | 4 | 1 | - |
| `l0_slowdown_trigger` | 8 | 1 | l0_stop_trigger |
| `l0_stop_trigger` | 12 | l0_slowdown_trigger | - |
| `max_bytes_for_level_base` | 10485760 | 65536 | - |
| `level_size_multiplier` | 10 | 2 | 100 |
| `target_file_size` | 2097152 | 16384 | - |
<!-- /table:options -->

Sizes are in bytes. A `-` is no bound. `l0_slowdown_trigger` may not exceed `l0_stop_trigger`.

The yes-or-no options:

<!-- table:flags -->
| Option | Default |
| --- | --- |
| `Options::create_if_missing` | false |
| `Options::error_if_exists` | false |
| `Options::paranoid_checks` | true |
| `ReadOptions::verify_checksums` | true |
| `ReadOptions::fill_cache` | true |
| `WriteOptions::sync` | true |
<!-- /table:flags -->

**What to turn, and what it costs.** These describe mechanism; the measured effect on a workload is yours
to measure, and [benchmarks](benchmarks.md) shows how `shale_bench` does it.

| Option | Raise it | Lower it |
| --- | --- | --- |
| `write_buffer_size` | Fewer flushes and fewer, larger level 0 tables; so fewer compactions of level 0. More memory: a memtable of this size plus a second, immutable one while the first is written out. More log to replay at open after a crash, up to about this much times two. | More, smaller level 0 tables, and more compaction. Less memory and a shorter replay. |
| `l0_compaction_trigger` | Level 0 is compacted less often, which costs less writing; a read looks in more level 0 files, since each file whose range covers the key is probed. | Level 0 stays short and reads stay cheap; compaction runs sooner and more often. It is never taken to be above `l0_stop_trigger`, so that a write that waits for level 0 always has a compaction coming. |
| `l0_slowdown_trigger`, `l0_stop_trigger` | More level 0 files are tolerated before writes are slowed or held. Reads get worse as level 0 grows. | Writes are slowed or held sooner. |
| `max_bytes_for_level_base`, `level_size_multiplier` | The levels are larger. Level *n* (1 to 5) is held to `base × multiplier^(n-1)` bytes before it is compacted down: with the defaults 10 MiB, 100 MiB, 1000 MiB, 10 000 MiB, 100 000 MiB; level 6 has no limit. Fewer levels are used; each byte is rewritten fewer times on the way down and sits longer as garbage. | Smaller levels, more rewriting, less space taken by overwritten entries. |
| `target_file_size` | Fewer, larger table files below level 0. | More, smaller ones. |
| `block_size` | Fewer blocks to index; a lookup reads more bytes for the one block it needs, and a block in the cache is larger. | The reverse. A block is also the unit of the checksum. |
| `block_restart_interval` | Keys are compressed against more of their neighbours; a seek walks further. | The reverse. |
| `bloom_bits_per_key` | Fewer false positives. The share of absent keys that pass the filter depends on the keys. With 100,000 keys of four binary bytes counting up it is 1.5% at 10 bits and 0.26% at 15 (the figures of commit `adb4509`); with text keys of ten or so bytes it is about half as much, 0.8% and 0.1%. `BloomTest.FalsePositiveRatesAtTenAndFifteenBitsPerKey` in `tests/io/bloom_test.cc` measures all four and bounds each a little above. Each key that passes costs a block read. More memory per open table. `0` writes no filters. | The reverse. |
| `block_cache_capacity` | More data blocks kept in memory for reads. The cache holds data blocks only, charged by their size; the index and filter of each open table are in memory besides, and are not counted here. There is no hit counter: judge it by read latency. | Less memory. |
| `max_open_files` | More tables kept open. The table cache holds `max_open_files - 10` tables (ten files are reserved for the log, the MANIFEST and the like); a table that is not open is opened again, reading its footer, index and filter, when a read needs it. | If there are more tables than this, reads pay to reopen them. |

**Memory** is about `write_buffer_size` for the memtable, as much again while one is being written out,
plus `block_cache_capacity`, plus the filter and the index of every open table. `GetProperty("shale.approximate-memory-usage")`
reports the memtables and the block cache only.

**`WriteOptions::sync`** is the one setting that trades safety for speed. With `true` (the default) a
write that returns is on stable storage, and writers that arrive together share one sync. With `false`
it is in the operating system's hands: it survives a crash of the process and may not survive a power
loss, though never in a way that keeps a later write and loses an earlier one. Use `false` for data that
you can regenerate, and `Flush()` or a `sync = true` write when you need a point you can rely on.

## Backup

A database is a directory. A backup is a copy of it that is **consistent**: the MANIFEST that it holds
must name tables that it holds. There is one way to take one.

**Close the database (or stop the process that has it open), then copy the whole directory.** `LOCK` and
`LOG` may be skipped; everything else may not. Closing does not flush: the memtable is in the log, the
log is in the copy, and opening the copy replays it. Nothing runs while the directory is copied, so no
file appears or disappears under the copy.

Then **check the copy**: `shalectl verify --db=COPY` reads the MANIFEST and every table, with checksums
on, and says `ok` or names what is wrong. `verify` finds a table that is missing or damaged; it cannot
find one that is there and older than it should be, which is another reason to copy only a closed
database. To **restore**, copy the directory back while the database is closed, and open it.

No online copy is offered, for one reason: nothing the engine exposes says that no flush or compaction
is in flight (a counter moves when one *ends*), and a copy that catches one half-way can hold a MANIFEST
that names a table that was already deleted. Calling `Flush()` first does not change that, and a
backup that is wrong in that way is the one that fails when it is needed. The engine has no checkpoint
call ([ADR 0007](design/0007-scope.md)).

## shalectl

`shalectl` is built with the project (`build/<preset>/tools/shalectl`).

```
shalectl --db=PATH [--create] put KEY VALUE
shalectl --db=PATH [--create] get KEY
shalectl --db=PATH [--create] delete KEY
shalectl --db=PATH [--create] scan [--from=KEY] [--limit=N]
shalectl --db=PATH [--create] stats
shalectl --db=PATH [--create] flush
shalectl --db=PATH verify
shalectl dump-sst FILE
shalectl dump-wal FILE
shalectl dump-manifest FILE
```

Exit status: `0` success, `1` key not found (`get`), `2` a mistake on the command line, `3` the engine,
or a file it reads, reported an error. Keys and values are printed as printable ASCII, with a backslash
as `\\` and any other byte as `\xNN`.

**Every command except `verify` and the dumps opens the database for writing**, which replays the log
into a table, writes a new MANIFEST and deletes files that are no longer needed. `flush` is for the case
where you want nothing left in a log.

**`verify`** changes no byte of what the database holds, but it is not read-only: it takes the `LOCK`
(so it refuses an open database), creates an empty `LOCK` if there is none, and needs write access to
the directory. It reads the MANIFEST and every live table with checksums on. It does **not** read the
write-ahead logs. It prints `ok`, or one line per problem on standard output and exits with status 3. A
failure to take the lock, or to read at all, is also status 3, with the reason on standard error and
nothing on standard output.

**The dumps** print one entry of a table, a log or a MANIFEST on each line, whatever database the file
is from, and open nothing else:

- `dump-sst FILE`: `KEY seq=N type=put|del value=V`, for each entry, in order, checking every block.
- `dump-wal FILE`: `batch seq=N count=M`, then `  put KEY VALUE` or `  del KEY` for each operation.
  A defect is reported on standard error as `FILE: N byte(s) dropped: <status>`, and the records that
  can be read are all shown.
- `dump-manifest FILE`: one `VersionEdit { ... }` for each record, showing `log_number`,
  `next_file_number`, `last_sequence`, compact pointers, and the files each edit deletes and adds
  (`add L1 #12 size=... smallest=... largest=...`). The first record is the snapshot of the whole
  table set; each later one is one flush or compaction.

## Reading the LOG

`LOG` in the database directory is for people; the engine never reads it. Each line begins with the time
as seconds and microseconds on the engine's clock, which is a monotonic clock that counts from an
arbitrary starting point (on some platforms it happens to be close to the time of day, and nothing
promises it): it is good for the order of lines and the gaps between them, not for the date. `LOG.old` is
the previous run's. The lines
that matter:

| Line begins | Meaning |
| --- | --- |
| `opening DIR`, `opened DIR` | an open began and ended. A `LOG` that stops after `opening` is an open that died or failed: look for `open failed:`. |
| `creating a new database` | no `CURRENT` was there and `create_if_missing` was set. |
| `replayed N log(s), M operation(s), last sequence number S` | recovery found writes in N logs. |
| `wrote the recovered writes to table N, B byte(s)` | recovery turned them into a level 0 table. |
| `WAL FILE: dropped N byte(s), <defect>; nothing after this is replayed` and `WAL FILE kept as FILE.dropped, not replayed: ...` | a lenient open (`paranoid_checks = false`) stopped at a defect and set logs aside. |
| `WAL FILE: its first write is sequence number N, and the writes from M were lost with the end of an earlier log; nothing from here on is replayed`, then `WAL FILE kept as FILE.dropped, not replayed: ...` | an open found the gap that a power loss leaves when it takes the unsynced end of one log and keeps writes of the next. Nothing that was acknowledged as synced is in the dropped logs. Any open does this, strict or lenient. |
| `flushed a memtable to table N, B byte(s)` | a flush finished. |
| `compacted A table(s) of level L and B of level L+1 into C table(s) of level L+1: ...` | a compaction finished, with bytes read and written and the entries it dropped. |
| `moved table N from level L to level L+1` | a trivial move. |
| `deleted NAME` and `could not delete NAME: ...` | the sweep of obsolete files; a failed delete is tried again. |
| `write path stopped: <status>` | the **background error**. Everything after it is the aftermath. |
| `stopped a compaction of level L: the database is closing` | a compaction was cut short by a close; it is redone at the next open if it is still due. |
| `closing` | the database is being closed. |

## Properties and counters

`GetProperty(name, &value)` returns false for a name that is not a property. They are one look at the
database at one moment.

<!-- table:properties -->
| Property | Value |
| --- | --- |
| `shale.num-files-at-level0` | the number of table files at level 0; `1` to `6` likewise (`shale.num-files-at-level<N>`) |
| `shale.total-sst-bytes` | the size in bytes of all table files |
| `shale.counters` | the counters below, as `name=value` pairs separated by spaces |
| `shale.write-amplification` | the bytes written to disk by the log, flushes and compactions, over the key and value bytes the callers wrote, with two decimals; `0.00` before any write |
| `shale.stats` | a header, a line `Level N: F files, B bytes` for each of the 7 levels, and the counters |
| `shale.approximate-memory-usage` | the bytes held by the memtables and the block cache |
| `shale.background-error` | `OK`, or the error that stops writes |
<!-- /table:properties -->

The counters, in the order `shale.counters` prints them. They count what the database has done since it was
opened, the table that recovery writes at open included, and are not stored: every open starts them at
zero.

<!-- table:counters -->
| Counter | Counts |
| --- | --- |
| `flushes` | memtables written out as level 0 tables |
| `compactions` | compactions that merged files |
| `trivial_moves` | files moved to the next level as they were |
| `tombstones_dropped` | deletions that a compaction dropped |
| `shadowed_dropped` | entries a compaction dropped for a newer one |
| `user_bytes_written` | key and value bytes of the writes made |
| `wal_bytes_written` | bytes the log writer handed to the log file, headers and padding included |
| `flush_bytes_written` | bytes of the tables that flushes wrote |
| `compaction_bytes_read` | bytes of the tables that compactions read |
| `compaction_bytes_written` | bytes of the tables that compactions wrote |
| `write_groups` | groups of writes that shared one log record |
| `wal_syncs` | syncs of the log that a group of writes asked for |
| `write_slowdowns` | writes slowed for level 0 being full |
| `write_stops` | waits of a write for level 0 or for a flush |
| `bg_errors` | errors that stopped writes |
<!-- /table:counters -->

`write_groups` against the number of writes you made tells you how much group commit is sharing;
`wal_syncs` is how many times the disk was asked.

## The files in the directory

| File | What to do with it |
| --- | --- |
| `NNNNNN.sst` | A table. Never touch. |
| `NNNNNN.log` | A write-ahead log. Never touch: it may hold writes that no table does. |
| `MANIFEST-NNNNNN`, `CURRENT` | The list of live tables. Never touch. |
| `LOCK` | The lock. Leave it; it is not a sign that the database is open. |
| `LOG`, `LOG.old` | The text log. Safe to read, to copy and to remove when the database is closed. |
| `NNNNNN.log.dropped` | A log that an open set aside. After a gap that a power loss left (any open), it holds no write that was acknowledged as synced, only writes made with `sync = false` after the last synced one. After a defect (a lenient open), **it holds writes that were not recovered, possibly acknowledged ones.** Never read again by the engine. Keep it until you have decided you do not want them; `DestroyDB()` removes it. One or more such files can appear after each power loss that leaves a gap. |
| `NNNNNN.dbtmp` | A file that was being written when a crash came. The next open deletes it. |
| anything else | Not the database's. The engine never reads, renames or deletes a file whose name it does not recognise. |
