# 0007. What shale does not do

Status: accepted

## Context

A storage engine can always do one more thing, and each thing it does has to be done all the way: the
failure paths, the crash behaviour and the tests, not just the case where it works. shale is built to
be believed, and an engine that "supports" a feature only on its happy path is a worse thing to trust
than one that says it does not have it. So the scope is set small, and the exclusions are written down
with reasons, so that a reader can tell a deliberate omission from an oversight.

## Decision

shale is an **embedded, single-process, POSIX, byte-ordered key-value library with one write path, one
compaction policy and one background thread**. These are not built, on purpose:

| Not built | Why |
| --- | --- |
| **Compression** (Snappy, Zstd) | It would be the engine's first third-party runtime dependency, and there are none. The block trailer reserves a type byte, so adding compression changes values and does not change the layout ([format](../format.md#tables)). |
| **Reverse iteration** (`Prev`, `SeekToLast`) | It roughly doubles the work of the merging iterator and of the iterator that folds versions into a user's view, and contributes nothing to the crash-consistency argument that the engine exists to make. |
| **Custom comparators** | Keys compare as bytes. That removes a whole class of failure, a comparator that disagrees with the data already on disk, which would have to be checked at every open. |
| **Column families, transactions, merge operators, TTL, range deletion** | Each is a subsystem with its own invariants, its own recovery rules and its own tests. |
| **Prefix Bloom filters, partitioned index, direct I/O, memory-mapped reads** | Performance features that do not change what has to be proved. Memory-mapped reads would also turn a read error or a truncated file into a signal instead of a `Status`. |
| **More than one background thread, subcompactions, universal or FIFO compaction, compaction triggered by seeks** | One background task at a time is what lets the concurrency argument be written down ([ADR 0006](0006-concurrency-model.md)); leveled compaction is the one policy ([ADR 0001](0001-lsm-and-leveled-compaction.md)). |
| **Index separator shortening** (`FindShortestSeparator`) | A space optimisation. The last key of a block is a valid separator, so the index is correct without it ([ADR 0005](0005-sstable-format.md)). |
| **A repair tool, a backup or checkpoint API** | [Operations](../operations.md#backup) gives the way to take a backup (close the database, or stop the writes, flush, and copy the directory) and `shalectl verify` checks one. A repair tool that guesses at damaged files is a promise the engine would have to keep. |
| **A network server, an RPC layer, a metrics endpoint** | shale is a library that a process links. A service that exposes a store over a network is a different program with a different failure model, and sits on top. |
| **Replication, consensus and failover** | Replicating a store needs log shipping, membership and a leader, and the correctness argument is a distributed one. It belongs to a system built on a storage engine, not in it. |
| **Queue semantics: consumer groups, retry scheduling, dead-letter queues** | These are the features of a message queue. Such a system would use an ordered store like this one for its state; none of them is a question about how bytes get to a disk safely. |
| **Windows** | POSIX only (Linux, macOS). The engine depends on `flock`, on the meaning of `fsync` on a directory, and on `F_FULLFSYNC` on macOS. |

Also by design: a database is opened by **one process at a time** (a `LOCK` file, D7 in
[ADR 0004](0004-durability.md)), keys are at most 64 KiB and values at most 64 MiB, and there are
7 levels.

## Consequences

**What this buys.** Every feature that is there is there fully. The failure paths and crash behaviour of
each are tested, and the list of what the test suite does not cover ([testing](../testing.md#what-the-tests-do-not-cover))
is short enough to read.

**What it costs.**

- A user who needs reverse scans, transactions, compression, a network interface or replication has to
  build them, or use a larger engine. That is the point of this list.
- Without a checkpoint call, a backup of a database that stays open is a procedure with a condition
  ([operations](../operations.md#backup)), not an API that is safe by construction.
- Without compression, data takes more disk than it could.
- Without a repair tool, a database whose MANIFEST or `CURRENT` is damaged is restored from a backup;
  the tables can still be read one file at a time with `shalectl dump-sst`.

**What would be easy to add, and what would not.**

- Compression, index shortening and a prefix filter would change what is inside a block or the filter,
  not the structure around them: the reserved type byte and the table format version are there for them.
- Reverse iteration, custom comparators, transactions and more background threads would each add an
  invariant to the version-state rules and to the simulator's model. They are rejected not because they
  are hard to write but because each one has to be *shown* correct to the standard of the rest.

## Alternatives considered

- **Build more, and do the normal path of each.** Rejected: it makes the engine look bigger and the
  claim "every confirmed write survives" mean less. The project's own rule is that a feature that is
  built is built with its failure paths.
- **Build a smaller engine** with no snapshots, no `CompactRange` or no iterators. Rejected: an engine
  without ordered scans and consistent reads is not what a user of an embedded store needs, and the
  hardest parts of compaction (what may be dropped while a snapshot is open) are the interesting ones.

## References

- `include/shale/db.h` and `include/shale/options.h` for what *is* there.
- [operations](../operations.md), [testing](../testing.md).
