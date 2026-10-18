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
prefix of the rest (so a file is cut at a seeded byte), rolls back the directory changes that were never
synced, kills the process, fails calls, and flips bits in what is read. After **every** step it compares
the database with a reference model, and after every reopen it requires the recovered state to be the
state after some prefix of the acknowledged writes, at least as long as the last synced one, with no
half-applied batch.
[`tests/sim/sim_test.cc`](tests/sim/sim_test.cc) · [`tests/testkit/simulator.h`](tests/testkit/simulator.h) ·
[the contract](docs/design/0004-durability.md) · and a real `kill -9` of a real process on a real
filesystem, 200 rounds in CI: [`tools/shale_crashtest.cc`](tools/shale_crashtest.cc)

**2. A whole run, compaction timing included, is a function of one integer.** All I/O goes through an
injected `Env` and all background work through an injected `Scheduler`; the decisions of the engine are
made by a pure core that cannot touch the world. A bug that seed 2240 found yesterday is found by
seed 2240 tomorrow, and a test asserts that one seed gives the same trace digest twice and on every
toolchain. [`tests/sim/determinism_test.cc`](tests/sim/determinism_test.cc) ·
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
and UndefinedBehaviorSanitizer run all of it, and six libFuzzer targets feed garbage to every decoder, to the recovery of the table set and
to the whole database. [`tests/compile_fail/`](tests/compile_fail/) ·
[`tests/stress/concurrency_test.cc`](tests/stress/concurrency_test.cc) · [`fuzz/`](fuzz/) ·
[ADR 0006](docs/design/0006-concurrency-model.md)

And the numbers in this README were measured, with the machine and the command beside them.

## Try it

`shalectl` is the command-line tool. It is built with the project:

```
cmake --preset release && cmake --build --preset release
```

The transcript below is the output of these commands as they were run (with `build/release/tools` on the
`PATH`), not written by hand:

```
$ shalectl --db=/tmp/shale-demo --create put user:1 alice
$ shalectl --db=/tmp/shale-demo put user:2 bob
$ shalectl --db=/tmp/shale-demo get user:1
alice
$ shalectl --db=/tmp/shale-demo delete user:2
$ shalectl --db=/tmp/shale-demo scan
user:1	alice
$ shalectl --db=/tmp/shale-demo get user:2
shalectl: not found
$ echo $?
1
$ shalectl --db=/tmp/shale-demo flush
$ shalectl --db=/tmp/shale-demo put user:3 carol
$ ls /tmp/shale-demo
000005.sst
000008.sst
000013.sst
000019.log
CURRENT
LOCK
LOG
LOG.old
MANIFEST-000020
$ shalectl dump-wal /tmp/shale-demo/000019.log
batch seq=4 count=1
  put user:3 carol
$ shalectl --db=/tmp/shale-demo verify
ok
$ shalectl dump-sst /tmp/shale-demo/000013.sst
user:2 seq=3 type=del value=
$ shalectl dump-manifest /tmp/shale-demo/MANIFEST-000020
VersionEdit {
  log_number: 19
  next_file_number: 21
  last_sequence: 3
  add L0 #5 size=129 smallest='user:1'@1:value largest='user:1'@1:value
  add L0 #8 size=127 smallest='user:2'@2:value largest='user:2'@2:value
  add L0 #13 size=124 smallest='user:2'@3:deletion largest='user:2'@3:deletion
}
$ shalectl --db=/tmp/shale-demo stats
shale stats
Level 0: 4 files, 509 bytes
Level 1: 0 files, 0 bytes
Level 2: 0 files, 0 bytes
Level 3: 0 files, 0 bytes
Level 4: 0 files, 0 bytes
Level 5: 0 files, 0 bytes
Level 6: 0 files, 0 bytes
Counters: flushes=1 compactions=0 trivial_moves=0 tombstones_dropped=0 shadowed_dropped=0 user_bytes_written=0 wal_bytes_written=0 flush_bytes_written=129 compaction_bytes_read=0 compaction_bytes_written=0 write_groups=0 wal_syncs=0 write_slowdowns=0 write_stops=0 bg_errors=0
```

Every command opens the database, and opening turns the log it finds into a table. That is why the first
commands each left a `.sst` behind, and why the log of the last `put` was still there to be dumped.

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

Every call returns a `shale::Status`, which is `[[nodiscard]]`: nothing throws, and a dropped error is a
compile error. [`examples/basic.cc`](examples/basic.cc) is a longer tour with batches, an iterator and a
snapshot. Every public item in [`include/shale/`](include/shale/) has a Doxygen comment, and the build fails
without it.

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
compaction merges tables down through seven levels. Every file except the log is written once and made
whole before anything names it, which is what keeps the crash argument short. The steps of every path
are in [docs/architecture.md](docs/architecture.md), and every byte of every file is in
[docs/format.md](docs/format.md).

## What it promises

<!-- durability contract -->
| | The promise |
| --- | --- |
| **D1** | With `sync = true` (**the default**), a `Write` that returns OK is present after a crash of the process and after a loss of power (assuming the device honours `fsync`; macOS uses `F_FULLFSYNC`). |
| **D2** | With `sync = false`, an OK write survives a crash of the process; after a power loss the state is the state after some prefix of the acknowledged writes that includes everything up to the last synced write. No holes. |
| **D3** | A `WriteBatch` is entirely visible or entirely invisible after any crash. |
| **D4** | A read returns the right value or an error. Never a wrong value, even after a bit flips on disk. |
| **D5** | After a background I/O error every write returns that error until the database is reopened. Reads keep working. |
| **D6** | The only trace of a crash silently tolerated in a log is a record cut off by the end of the file. Any other defect fails `Open` (`paranoid_checks = true`, the default). |
| **D7** | One `DB` per directory at a time, enforced with `flock`. |

How each is checked, and the order of syncs that makes it true, is in
[ADR 0004](docs/design/0004-durability.md). `sync = true` is the default on purpose, the opposite of
LevelDB and RocksDB: the default that cannot lose a confirmed write.

## Measured

<!-- benchmarks:begin (copied from docs/benchmarks.md: replace everything up to the end marker) -->
shale and LevelDB on one machine, through the same workloads and the same code (`shale_bench`). **A ratio
under 1 means shale is slower.** Each figure is the median of three runs, with the lowest and the highest in
brackets. Every read was checked against a record of what was written.

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
| fillseq | 380,882 (375,414 to 382,323) | 448,481 (448,045 to 448,695) | 0.85 |
| fillrandom | 259,365 (153,575 to 264,247) | 284,893 (282,502 to 287,338) | 0.91 |
| fillsync | 345 (344 to 345) | 335 (332 to 336) | 1.03 |
| overwrite | 190,098 (189,162 to 210,303) | 277,799 (271,681 to 279,394) | 0.68 |
| readrandom | 280,939 (277,569 to 281,703) | 434,082 (365,583 to 434,339) | 0.65 |
| readmissing | 1,125,959 (1,125,691 to 1,134,390) | 1,072,933 (1,069,143 to 1,073,601) | 1.05 |
| readseq | 2,819,038 (2,682,553 to 2,828,695) | 12,422,399 (12,419,712 to 12,436,747) | 0.23 |
| seekrandom | 79,349 (79,338 to 79,524) | 365,534 (363,088 to 366,201) | 0.22 |

The same, with `--compact_before_reads` (the database is compacted once, before the reads):

| benchmark | shale ops/s | LevelDB ops/s | shale / LevelDB |
| --- | ---: | ---: | ---: |
| fillseq | 332,033 (326,376 to 337,998) | 411,985 (409,310 to 412,266) | 0.81 |
| fillrandom | 217,485 (208,302 to 221,172) | 244,493 (219,171 to 252,626) | 0.89 |
| fillsync | 345 (344 to 346) | 335 (329 to 336) | 1.03 |
| overwrite | 188,068 (186,516 to 190,241) | 244,763 (241,648 to 267,016) | 0.77 |
| readrandom | 363,602 (363,516 to 364,085) | 508,784 (507,812 to 510,388) | 0.71 |
| readmissing | 4,183,217 (4,169,409 to 4,196,415) | 1,672,947 (1,670,665 to 1,678,121) | 2.50 |
| readseq | 5,553,271 (5,514,200 to 5,558,331) | 13,265,656 (13,213,924 to 13,290,634) | 0.42 |
| seekrandom | 276,667 (275,732 to 276,686) | 423,984 (422,747 to 424,570) | 0.65 |

What each row is, how the two engines were matched, where shale is slower and why, and what these numbers do not
say are in [docs/benchmarks.md](docs/benchmarks.md), with the latency and write-amplification tables.
<!-- benchmarks:end -->

## Bugs found while building it

The simulator and reviews found real bugs in the engine while it was being built, and each one is pinned
by a test that fails without its fix.

| Found by | The bug | Fixed in | Pinned by |
| --- | --- | --- | --- |
| the simulator, **seed 2240** | After a failed directory sync, the engine decided from a *misread* of `CURRENT` that it need not keep the new MANIFEST, deleted it, and the database could not be opened: `CURRENT` named a file that was gone. | `cacf403` | seed 2240 in [`regression_seeds.txt`](tests/sim/regression_seeds.txt); a test in [`version_set_test.cc`](tests/db/version_set_test.cc) that fails without the fix |
| the simulator, **seeds 185 and 191** | A bit flipped in the length of a log record in the last block made a whole record look like the torn tail a crash leaves. The reader stopped without a word and dropped the rest of the file: synced writes in a WAL, and the edits of flushes and compactions in a MANIFEST. About one run in 300. | `89915a1` | seeds 185 and 191; [`log_test.cc`](tests/io/log_test.cc) (all 16 bits of a length, 400 random flips, none silent) |
| the review of the fix for seed 2240 | `CURRENT` had no checksum, so a flipped digit could select an *older* MANIFEST that was still in the directory, and the database would open an older version of itself silently. | `d8c7e7b` | [`version_set_test.cc`](tests/db/version_set_test.cc): all 256 single-bit flips of a `CURRENT` are refused; [`db_recovery_test.cc`](tests/db/db_recovery_test.cc) |
| the simulator, comparing toolchains | The arena padded to `alignof(std::max_align_t)`, 8 on one compiler and 16 on another, so the same seed filled a memtable one write apart: a failure under one toolchain could pass under the other. | `b082176` | [`arena_test.cc`](tests/util/arena_test.cc); the pinned digests of [`determinism_test.cc`](tests/sim/determinism_test.cc) |
| review of the open path | A lenient open took a transient read error for a defect in the log, succeeded, and then deleted that log and every later one. Now a read error fails `Open`, and what a lenient open drops is renamed to `.dropped`, never deleted. | `39793ad` | [`db_recovery_test.cc`](tests/db/db_recovery_test.cc) (`LenientOpenSetsAsideTheLogWithTheDefectAndEveryLaterLog`, `AFailureAnywhereInALenientOpenLosesNeitherDataNorDroppedLogs`) |
| review of the table reader | A read with `verify_checksums` off put the block it read into the cache that verifying readers share, and a later verifying read was served a block nothing had checked. | `216e7fc` | [`table_test.cc`](tests/io/table_test.cc) (`UnverifiedReadDoesNotLeaveAnUnverifiedBlockInTheCache`) |

The fuzz targets, the stress tests and the real crash test did not find an engine bug in the runs their
commits record; the stress tests were shown to fail on engines that were broken on purpose in three ways,
and the crash test fails on a database that it damages itself. That is evidence about those runs and not a
proof, and the next section says what is outside them.

## What the test suite does not cover

- **A real power loss.** The simulator models "bytes that were not synced are lost", and a process kill
  leaves the page cache behind. Neither models a drive that acknowledges a flush it has not done.
- **How a controller or a filesystem reorders writes**, beyond what the simulator's model of ordered
  directory changes and prefix-only loss says. What it also leaves out: files with zero-filled or garbage
  tails after a power loss, durable-at-once `CreateDir`, and bytes that change at rest.
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
