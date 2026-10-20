# shale

**A crash-safe LSM-tree storage engine in C++20, written from scratch, with a deterministic
fault-injection simulator.**

[![CI](https://github.com/jason-te-sde/shale/actions/workflows/ci.yml/badge.svg)](https://github.com/jason-te-sde/shale/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

shale is an embedded, ordered key-value store in the shape of LevelDB and RocksDB: a library that a
process links, with `Put`, `Get`, `Delete`, atomic batches, ordered iterators, snapshots and
compaction. It has **no third-party runtime dependency** (the checksums, the hashing and the caches
are its own), runs on Linux and macOS, and is built to be believed rather than to be fast: every
claim below is backed by a test, a commit or a measurement that you can run.

## Why you can believe it

**1. Every write the database confirmed is still there after a crash.** A deterministic simulator runs
the real engine over a filesystem that can lose power: it keeps the bytes that were synced and a seeded
prefix of the rest (so a file is cut at a seeded byte), keeps only a seeded in-order prefix of the
directory changes (new files, renames, removals) that were never synced, kills the process, fails
calls, and flips bits in what is read. After **every** step it compares the database with a reference
model, and after every reopen it requires the recovered state to be the state after some prefix of the
acknowledged writes, at least as long as the last synced one, with no half-applied batch.
[`tests/sim/sim_test.cc`](tests/sim/sim_test.cc) · [`tests/testkit/simulator.h`](tests/testkit/simulator.h) ·
[the contract](docs/design/0004-durability.md) · and a real `kill -9` of a real process on a real
filesystem, 200 rounds in CI: [`tools/shale_crashtest.cc`](tools/shale_crashtest.cc)

**2. A whole run, compaction timing included, is a function of one integer.** All I/O goes through an
injected `Env` and all background work through an injected `Scheduler`; the decisions of the engine are
made by a pure core that cannot touch the world. A bug that seed 2240 found yesterday is found by
seed 2240 tomorrow, and a test asserts that one seed gives the same trace digest twice and on every
toolchain that CI builds with. [`tests/sim/determinism_test.cc`](tests/sim/determinism_test.cc) ·
[`tests/sim/regression_seeds.txt`](tests/sim/regression_seeds.txt) ·
[ADR 0002](docs/design/0002-pure-core-and-injected-env.md)

**3. The test suite can go red.** Six deliberate flaws (skip the log sync, skip the directory sync, skip
the MANIFEST sync, drop tombstones too early, ignore snapshots in compaction, delete files a reader
still holds) are compiled in one at a time, and a CI job fails unless the simulator fails with an
invariant for each. [`scripts/run-flaws.sh`](scripts/run-flaws.sh) ·
[the flaws and what caught them](docs/testing.md#injected-flaws)

**4. The locking is checked by the compiler, the races by a tool, the decoders by a fuzzer.** Clang's
thread-safety analysis runs with `-Werror` (and a test shows that it rejects an unguarded access),
ThreadSanitizer runs the test suite three times (the single-threaded simulator once), AddressSanitizer
and UndefinedBehaviorSanitizer run all of it, and six libFuzzer targets feed garbage to the log reader, the
block and table readers, the version edit decoder, the recovery of the table set from `CURRENT` and a
MANIFEST, and the whole database. [`tests/compile_fail/`](tests/compile_fail/) ·
[`tests/stress/concurrency_test.cc`](tests/stress/concurrency_test.cc) · [`fuzz/`](fuzz/) ·
[ADR 0006](docs/design/0006-concurrency-model.md)

And the numbers in this README were measured, with the machine and the command beside them.

## Try it

`shalectl` is the command-line tool. It is built with the project:

```
cmake --preset release && cmake --build --preset release
```

The transcript below is the output of these commands as they were run (with `build/release/tools` on the
`PATH`), not written by hand, and [a test](tests/tools/readme_transcript_test.sh) runs them again and
compares:

<!-- transcript:try-it -->
```
$ shalectl --db=/tmp/shale-demo --create put user:1 alice
$ shalectl --db=/tmp/shale-demo put user:2 bob
$ shalectl --db=/tmp/shale-demo get user:1
alice
$ shalectl --db=/tmp/shale-demo scan
user:1	alice
user:2	bob
$ shalectl --db=/tmp/shale-demo delete user:2
$ ls /tmp/shale-demo
000005.sst
000008.sst
000012.log
CURRENT
LOCK
LOG
LOG.old
MANIFEST-000013
$ shalectl dump-wal /tmp/shale-demo/000012.log
batch seq=3 count=1
  del user:2
$ shalectl --db=/tmp/shale-demo scan
user:1	alice
$ shalectl --db=/tmp/shale-demo get user:2
shalectl: not found
$ echo $?
1
$ shalectl --db=/tmp/shale-demo flush
$ shalectl --db=/tmp/shale-demo verify
ok
$ shalectl dump-sst /tmp/shale-demo/000015.sst
user:2 seq=3 type=del value=
$ shalectl dump-manifest /tmp/shale-demo/MANIFEST-000020
VersionEdit {
  log_number: 19
  next_file_number: 21
  last_sequence: 3
  add L0 #5 size=129 smallest='user:1'@1:value largest='user:1'@1:value
  add L0 #8 size=127 smallest='user:2'@2:value largest='user:2'@2:value
  add L0 #15 size=124 smallest='user:2'@3:deletion largest='user:2'@3:deletion
}
$ shalectl --db=/tmp/shale-demo stats
shale stats
Level 0: 3 files, 380 bytes
Level 1: 0 files, 0 bytes
Level 2: 0 files, 0 bytes
Level 3: 0 files, 0 bytes
Level 4: 0 files, 0 bytes
Level 5: 0 files, 0 bytes
Level 6: 0 files, 0 bytes
Counters: flushes=0 compactions=0 trivial_moves=0 tombstones_dropped=0 shadowed_dropped=0 user_bytes_written=0 wal_bytes_written=0 flush_bytes_written=0 compaction_bytes_read=0 compaction_bytes_written=0 write_groups=0 wal_syncs=0 write_slowdowns=0 write_stops=0 bg_errors=0
```

Every command except `verify` and the dumps opens the database, and opening turns the log it finds into a
table. That is why the first commands each left a `.sst` behind, and why the log of the last write, the
`delete`, was still there to be dumped. The sequence is shaped so that it never reaches four level-0 tables,
which is where a compaction would start in the background while `stats` prints, and the transcript would
differ from one run to the next; the test that runs it again says so.

`verify` reads every live table with its checksums on, and the three dump commands print what is inside
a table, a write-ahead log or a MANIFEST. What to do when they say something is wrong is in the
[operations guide](docs/operations.md).

## Use it as a library

The library installs as a CMake package:

```
cmake --preset release && cmake --build --preset release
cmake --install build/release --prefix ~/shale
```

and a project that uses it needs these three lines:

<!-- example:quickstart_cmake -->
```cmake
find_package(shale 0.1 REQUIRED)
add_executable(quickstart quickstart.cc)
target_link_libraries(quickstart PRIVATE shale::shale)
```

configured with `-DCMAKE_PREFIX_PATH=~/shale`. This is [`examples/quickstart.cc`](examples/quickstart.cc),
fifteen lines, which CI builds against an installed copy of the library and runs (the README's copy of it is
held to the file by [a test](tests/docs/readme_test.cc)):

<!-- example:quickstart -->
```cpp
#include <iostream>
#include <memory>
#include <string>

#include "shale/db.h"

int main() {
  shale::Options options;
  options.create_if_missing = true;
  std::unique_ptr<shale::DB> db;
  shale::Status s = shale::DB::Open(options, "/tmp/shale-quickstart", &db);
  std::string value;
  if (s.ok()) s = db->Put(shale::WriteOptions(), "greeting", "hello");
  if (s.ok()) s = db->Get(shale::ReadOptions(), "greeting", &value);
  std::cout << (s.ok() ? value : s.ToString()) << '\n';
  return s.ok() ? 0 : 1;
}
```

Every operation that can fail returns a `shale::Status` (an iterator reports its failure through
`status()`), and `Status` is `[[nodiscard]]`: nothing throws, and a dropped error is a warning, which the
project's own build, like the examples', turns into an error.
[`examples/basic.cc`](examples/basic.cc) is a longer tour with batches, an iterator and a snapshot.
Every public item in [`include/shale/`](include/shale/) has a Doxygen comment, and the documentation
build that CI runs fails without it.

## How it works

```
 write                                                  read
   │                                                      │
   ▼                                                      ▼
 queue of writers ─► the leader forms a group        memtable ─► immutable memtable
   │                                                      │             │
   ▼                                                      ▼             ▼
 write-ahead log   (fsync if sync = true)             Version ──► L0 files, newest first
   │                                                      │       L1 .. L6: one file each
   ▼                                                      ▼
 memtable (skip list)                                 table cache ─► Bloom filter ─► index
   │ full                                                                  │
   ▼                                                                       ▼
 immutable memtable ──flush──► L0 table                              data block (CRC checked)
                                  │
              one background thread: compaction merges L(n) into L(n+1)
              MANIFEST: the log of the edits that make one set of tables the next
              CURRENT:  names the live MANIFEST, with a checksum
```

A write goes to the log and the memtable; a full memtable is written out as an immutable sorted table;
compaction merges tables down through seven levels. A table is written once and made whole before
anything names it; the two files that grow, the log and the MANIFEST, only ever have a checksummed
record appended, and `CURRENT` is replaced whole by a rename, never changed in place. A crash that cuts
the last record of the log or the MANIFEST is told from damage when the database opens. That is what
keeps the crash argument short. The steps of every path are in
[docs/architecture.md](docs/architecture.md), and every byte of every file is in
[docs/format.md](docs/format.md).

## What it promises

<!-- table:contract -->
| | The promise |
| --- | --- |
| **D1** | With `sync = true` (**the default**), a `Write` that returns OK is present after a crash of the process and after a loss of power (assuming the device honours `fsync`; macOS uses `F_FULLFSYNC`). |
| **D2** | With `sync = false`, an OK write survives a crash of the process; after a power loss the state is the state after some prefix of the acknowledged writes that includes everything up to the last synced write. No holes. |
| **D3** | A `WriteBatch` is entirely visible or entirely invisible after any crash. |
| **D4** | A read returns the right value or an error. Never a wrong value, even after a bit flips on disk (with `verify_checksums`, the default). |
| **D5** | After a background I/O error every write returns that error until the database is reopened. Reads keep working. |
| **D6** | Three traces of a crash are tolerated whatever `paranoid_checks` says. In the logs and the MANIFEST: a record cut off by the end of a file, and zeros from a record boundary to the end of the file, which a filesystem leaves when it kept a file's length and not its data (the reader checks every byte to the end, and the `LOG` names the file). In the logs: a gap in the sequence numbers at the start of a log, which is what a power loss leaves when it takes the unsynced end of one log and keeps unsynced writes of the next (that log and the later ones are set aside as `.dropped` files, never deleted except by `DestroyDB()`). None drops a write that was acknowledged as synced. Any other defect fails `Open` (`paranoid_checks = true`, the default): a garbage tail, zeros followed by any other byte, and zeros inside a record that the file holds in full among them (a record that the file ends inside is cut off, the first trace). |
| **D7** | One `DB` per directory at a time, enforced with `flock`. |
<!-- /table:contract -->

How each is checked, and the order of syncs that makes it true, is in
[ADR 0004](docs/design/0004-durability.md). `sync = true` is the default on purpose, the opposite of
LevelDB and RocksDB: the default that cannot lose a confirmed write.

## Measured

<!-- benchmarks:begin (copied from docs/benchmarks.md: replace everything up to the end marker) -->
shale and LevelDB on one machine, through the same workloads and the same code (`shale_bench`). **A ratio
under 1 means shale is slower.** Each figure is the median of three runs, with the lowest and the highest in
brackets. Every read was checked against a record of what was written. The figures of shale are those of
the final build, after four changes that a first measurement led to
([docs/benchmarks.md](docs/benchmarks.md) has the figures before them).

In short, measured on 2026-10-18 with both engines in the same session:

- **Writes.** On `fillseq` and `fillrandom` shale is level with LevelDB: 0.99 and 0.98 of its ops/s in
  three interleaved rounds of the two engines, against 0.87 and 0.90 before the changes. `overwrite` is
  0.84 (0.81 before). The single runs of the full commands put shale above LevelDB on all three in the first
  pass and below it in the second; the write rows of this machine move by up to 16% from one run to the next,
  which is why the interleaved rounds are the figure to use. Synced writes are level (1.02, 1.03).
- **Reads.** LevelDB is faster at reading what is there: `readrandom` 0.70 and 0.74, `readseq` 0.28 and 0.46,
  `seekrandom` 0.28 and 0.68 (first pass, and compacted before the reads). shale is faster at keys that
  are not there: `readmissing` 1.21 and 2.49.
- **The changes.** Four inefficiencies that the first measurement found were fixed. A synced write still
  survives a power loss with every write acknowledged before it, and a power loss still leaves a prefix of
  the writes. Since a memtable switch no longer syncs, recovery now also sets aside a log that begins after
  a gap that a power loss left, whatever `paranoid_checks` says
  ([durability](docs/design/0004-durability.md), D6). Inlining `Status::ok()` and the key comparison
  made `readseq` 9% faster in the compacted pass, and the first-pass reads 7% to 59% faster in runs
  that are less steady; reading blocks into buffers that are not zero-filled, 2% on the point reads
  and seeks; writing the MANIFEST without the database mutex held, nothing measurable on its own;
  taking the syncs of a memtable switch off the writer's path, 4% to 13% on the write rows in the
  interleaved rounds. Each, with its figures, is in [docs/benchmarks.md](docs/benchmarks.md).

**Machine.** Apple M2, 8 cores (4 performance, 4 efficiency), 16 GiB, macOS 26.6.2 (build 25G83), `Darwin 25.6.0
arm64`; shale a Release build with Apple clang 21.0.0; LevelDB 1.23, the Homebrew bottle `leveldb 1.23_2`;
1,000,000 keys of 16 bytes and values of 100 bytes (`fillsync`: 10,000 puts).

**Commands.** `$DB` is an empty directory on the internal SSD.

```
cmake --preset release -DSHALE_BUILD_BENCH=ON -DCMAKE_PREFIX_PATH=$(brew --prefix leveldb)
cmake --build --preset release

caffeinate -ims build/release/tools/shale_bench --engine=shale \
  --benchmarks=fillseq,fillrandom,fillsync,overwrite,readrandom,readmissing,readseq,seekrandom \
  --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-10-m2.json
caffeinate -ims build/release/tools/shale_bench --engine=leveldb \
  --benchmarks=fillseq,fillrandom,fillsync,overwrite,readrandom,readmissing,readseq,seekrandom \
  --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-10-m2-leveldb.json
```

The commands as given:

| benchmark | shale ops/s | LevelDB ops/s | shale / LevelDB |
| --- | ---: | ---: | ---: |
| fillseq | 441,200 (435,705 to 441,654) | 408,109 (403,560 to 413,307) | 1.08 |
| fillrandom | 290,445 (288,418 to 291,838) | 247,073 (243,598 to 253,134) | 1.18 |
| fillsync | 345 (344 to 346) | 337 (336 to 337) | 1.02 |
| overwrite | 239,958 (235,260 to 240,642) | 237,847 (234,719 to 240,619) | 1.01 |
| readrandom | 304,512 (297,775 to 304,775) | 431,943 (369,074 to 434,236) | 0.70 |
| readmissing | 1,294,124 (1,293,920 to 1,297,000) | 1,065,736 (1,065,329 to 1,067,809) | 1.21 |
| readseq | 3,478,488 (3,477,337 to 3,486,352) | 12,453,332 (12,437,695 to 12,484,537) | 0.28 |
| seekrandom | 103,190 (103,153 to 103,327) | 366,458 (364,424 to 366,875) | 0.28 |

The same, with `--compact_before_reads` (the database is compacted once, before the reads):

| benchmark | shale ops/s | LevelDB ops/s | shale / LevelDB |
| --- | ---: | ---: | ---: |
| fillseq | 377,485 (372,644 to 380,806) | 413,846 (404,282 to 415,407) | 0.91 |
| fillrandom | 242,763 (242,581 to 246,257) | 249,546 (248,928 to 252,725) | 0.97 |
| fillsync | 345 (342 to 346) | 337 (336 to 344) | 1.03 |
| overwrite | 212,334 (209,492 to 213,469) | 245,928 (240,790 to 250,904) | 0.86 |
| readrandom | 375,821 (375,674 to 376,086) | 505,382 (504,340 to 505,730) | 0.74 |
| readmissing | 4,111,598 (4,108,292 to 4,117,099) | 1,651,735 (1,648,003 to 1,659,988) | 2.49 |
| readseq | 6,062,353 (6,058,940 to 6,066,268) | 13,299,420 (13,227,572 to 13,319,319) | 0.46 |
| seekrandom | 287,504 (287,476 to 287,849) | 424,286 (423,382 to 425,101) | 0.68 |

The write rows of this machine move by up to 16% from one run to the next, so the figure to use for them is the
one of three rounds that ran the builds one after the other, each run after a check for a quiet machine
(ops/s, the median of the three rounds' medians; "before" is the build with the first two changes, which do not
touch writing, and without the MANIFEST and switch changes; "with both" is the final build):

| benchmark | shale before | with the MANIFEST change | with both | LevelDB | before / LevelDB | after / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| fillseq | 388,887 | 390,531 | 441,871 | 447,959 | 0.87 | 0.99 |
| fillrandom | 262,190 | 266,595 | 287,276 | 292,827 | 0.90 | 0.98 |
| overwrite | 233,344 | 232,679 | 242,683 | 289,526 | 0.81 | 0.84 |

What each row is, how the two engines were matched, where shale is slower and why, and what these numbers do not
say are in [docs/benchmarks.md](docs/benchmarks.md), with the latency and write-amplification tables.
<!-- benchmarks:end -->

## Bugs found while building it

The simulator and reviews found real bugs in the engine while it was being built, and the crash test in CI
found one after the first release. Each one is pinned by a test that fails without its fix.

| Found by | The bug | Fixed in | Pinned by |
| --- | --- | --- | --- |
| the simulator, **seed 2240** | After a failed directory sync, the engine decided from a *misread* of `CURRENT` that it need not keep the new MANIFEST, deleted it, and the database could not be opened: `CURRENT` named a file that was gone. | [`cacf403`][cacf403] | seed 2240 in [`regression_seeds.txt`](tests/sim/regression_seeds.txt); a test in [`version_set_test.cc`](tests/db/version_set_test.cc) that fails without the fix |
| the simulator, **seeds 185 and 191** | A bit flipped in the length of a log record in the last block made a whole record look like the torn tail a crash leaves. The reader stopped without a word and dropped the rest of the file: synced writes in a WAL, and the edits of flushes and compactions in a MANIFEST. About one run in 300. | [`89915a1`][89915a1] | seeds 185 and 191; [`log_test.cc`](tests/io/log_test.cc) (all 16 bits of a length, 400 random flips, none silent) |
| the review of the fix for seed 2240 | `CURRENT` had no checksum, so a flipped digit could select an *older* MANIFEST that was still in the directory, and the database would open an older version of itself silently. | [`d8c7e7b`][d8c7e7b] | [`version_set_test.cc`](tests/db/version_set_test.cc): all 256 single-bit flips of a `CURRENT` are refused; [`db_recovery_test.cc`](tests/db/db_recovery_test.cc) |
| the simulator, comparing toolchains | The arena padded to `alignof(std::max_align_t)`, 8 on one compiler and 16 on another, so the same seed filled a memtable one write apart: a failure under one toolchain could pass under the other. | [`b082176`][b082176] | [`arena_test.cc`](tests/util/arena_test.cc); the pinned digests of [`determinism_test.cc`](tests/sim/determinism_test.cc) |
| review of the open path | A lenient open took a transient read error for a defect in the log, succeeded, and then deleted that log and every later one. Now a read error fails `Open`, and what a lenient open drops is renamed to `.dropped`, never deleted except by `DestroyDB()`. | [`39793ad`][39793ad] | [`db_recovery_test.cc`](tests/db/db_recovery_test.cc) (`LenientOpenSetsAsideTheLogWithTheDefectAndEveryLaterLog`, `AFailureAnywhereInALenientOpenLosesNeitherDataNorDroppedLogs`) |
| review of the table reader | A read with `verify_checksums` off put the block it read into the cache that verifying readers share, and a later verifying read was served a block nothing had checked. | [`216e7fc`][216e7fc] | [`table_test.cc`](tests/io/table_test.cc) (`UnverifiedReadDoesNotLeaveAnUnverifiedBlockInTheCache`) |
| the real crash test, in CI on macOS 14, after 0.1.0 | A kill of the writer left its MANIFEST ending in 438 zero bytes, the edit it was appending, which the operating system had allotted and not written. The reader took the zeros for a damaged record and the database would not open. The simulator's filesystem left only clean prefixes, so it could not have found this. | [`88ef8f7`][88ef8f7] | the MANIFEST from CI in [`tests/db/regression`](tests/db/regression/manifest-zero-tail); seeds 0 and 48, now that a simulated power loss can leave zeros; [`log_test.cc`](tests/io/log_test.cc) |

The commits are those of the milestone pull requests, which were squash-merged; the links open them on
GitHub.

The fuzz targets and the stress tests did not find an engine bug in the runs their commits record, and the
real crash test found the one in the last row; the stress tests were shown to fail on engines that were
broken on purpose in three ways, and the crash test fails on a database that it damages itself. That is
evidence about those runs and not a proof, and the next section says what is outside them.

## What the test suite does not cover

- **A real power loss.** The simulator models "bytes that were not synced are lost, or come back as
  zeros", and a process kill leaves the page cache behind. Neither models a drive that acknowledges a
  flush it has not done.
- **How a controller or a filesystem reorders writes**, beyond what the simulator's model of ordered
  directory changes and prefix-only loss says. What it also leaves out: files with garbage tails after a
  power loss (which, since a memtable switch syncs nothing, can be in either of the last two logs), zeros
  that start inside the unsynced part of a file, durable-at-once `CreateDir`, and bytes that change at
  rest.
- **Runs longer or bigger than CI's**: 50 seeds of 2000 steps, 200 killed writers, 60 seconds a fuzz target.
  Tens of gigabytes and months of uptime have not been run.
- **Other filesystems and platforms**: ext4 and APFS only, x86-64 Linux and arm64 macOS only.

The complete list, with what local runs can and cannot cover, is in
[docs/testing.md](docs/testing.md#what-the-tests-do-not-cover). Line coverage of `src/`, measured by CI on
Linux with GCC 14, is 95.4%; CI fails below 90%.

## What it does not do

No compression, reverse iteration, custom comparators, transactions, column families, network server or
replication, and one background thread. Each omission has a reason in
[ADR 0007](docs/design/0007-scope.md), and what is built is built with its failure paths.

## Documentation

| | |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | the layers, and every path (write, read, flush, compaction, recovery, deletion) step by step |
| [docs/format.md](docs/format.md) | every byte on disk, held to the code by a test |
| [docs/operations.md](docs/operations.md) | what each error means and what to do at 3 a.m.; tuning; backup |
| [docs/testing.md](docs/testing.md) | the layers of tests, how to reproduce a seed, what is not covered |
| [docs/benchmarks.md](docs/benchmarks.md) | how the numbers above were measured |
| [docs/design/](docs/design/) | seven design records: context, decision, costs, and what was rejected |
| [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md), [CHANGELOG.md](CHANGELOG.md), [RELEASING.md](RELEASING.md), [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) | the project |

## Building and testing

CMake 3.25 or newer, Ninja, and GCC 13, Clang 17 or Apple Clang 15 or newer.

```
cmake --preset dev && cmake --build --preset dev
ctest --preset dev
```

[MIT licensed](LICENSE).

[216e7fc]: https://github.com/jason-te-sde/shale/commit/216e7fca4ca3e22326ec97f062b00308df9a4475
[39793ad]: https://github.com/jason-te-sde/shale/commit/39793ada1cbde96f02a4011bd4abddf4bce2eb96
[88ef8f7]: https://github.com/jason-te-sde/shale/commit/88ef8f759091f2b1b40d0c2368b7018420bc687b
[89915a1]: https://github.com/jason-te-sde/shale/commit/89915a11fe4fc0b9c394960b096fef976f928463
[b082176]: https://github.com/jason-te-sde/shale/commit/b0821763439d9306d7427686350ae8995f54d7d7
[cacf403]: https://github.com/jason-te-sde/shale/commit/cacf403bbc7d37e67e02db58e74334de391a5ef9
[d8c7e7b]: https://github.com/jason-te-sde/shale/commit/d8c7e7bd50067c4de9f259b5fc9facdda5ca1848
