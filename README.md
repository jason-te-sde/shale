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
shale 0.1.2 next to v0.1.1 and LevelDB on one machine, through the same workloads and the same code (`shale_bench`), all
built and run in one session on 2026-11-23. The ratio columns are 0.1.2 / v0.1.1 and 0.1.2 / LevelDB: above 1 means 0.1.2
is faster. Each figure is the median of three runs, with the lowest and the highest in brackets. Every read was checked
against a record of what was written, and every shale build read the same files.

In short:

- **Faster reads than v0.1.1, on every read row.** In three interleaved rounds of the builds, with all three rounds of
  0.1.2 above all three of v0.1.1's: `readseq` is 1.97 times as fast (1.98 after a compaction), `seekrandom` 1.24 times
  (1.25), `readmissing` 1.11 times (1.16) and `readrandom` 1.04 times (1.04). Readahead took the `pread` calls of
  `readseq` from 31,786 to 875, and a cached `Get` allocates once where it allocated 6 times.
- **Faster compaction.** Compaction reads its input through the new table iterator. Compacting the same files, with the
  same two compactions and the same bytes, took 1.51 s in 0.1.2 against 2.59 s in v0.1.1. After `overwrite`, 0.1.2 has
  made all its compactions when the last write returns, where v0.1.1 needs another 0.7 to 0.9 s.
- **Ahead of LevelDB on `readmissing`** (2.06 in the first pass, 3.20 after a compaction, with all three rounds of shale
  above all three of LevelDB's), and **level with LevelDB on `fillseq` and `fillrandom`** in the write rounds (0.97 and
  0.98, and 0.97 and 1.02 counting the wait for the background).
- **What changed.** One table iterator that makes every check itself, readahead for a table read in order, and fewer
  allocations for a read. No file the engine writes has changed.

The rows where LevelDB is faster, the rows that did not improve, how each change moved each row, a diagnostic run with
the whole database in the block cache, the first session of 2026-11-22 that this one supersedes, and what these numbers
do not say are in [docs/benchmarks.md](docs/benchmarks.md#2026-11-where-shale-is-still-slower-and-why) and the sections
around it. The figures of 2026-10 that the file keeps were made on another version of macOS, and are not comparable with
these.

**Machine.** Apple M2, 8 cores (4 performance, 4 efficiency), 16 GiB, macOS 27.2.1 (build 26B131), `Darwin 27.2.0 arm64`;
shale a Release build with Apple clang 21.0.0; LevelDB 1.23, the Homebrew bottle `leveldb 1.23_2`; 1,000,000 keys of 16
bytes and values of 100 bytes (`fillsync`: 10,000 puts). The builds are clean exports of v0.1.1 (`9fefedf`) and of
`c7af18b`, the library of 0.1.2, each with the benchmark tool of `ae1e1d4`.

**Commands.** `$WORK/<build>` is the export of a build. For the writes `$DB` is an empty directory on the internal SSD; for
the reads it is an APFS clone of one database that 0.1.2 wrote with `fillrandom,overwrite` and opened once (LevelDB reads
its own, made the same way). The compacted pass adds `--compact_before_reads`. LevelDB is the 0.1.2 binary with
`--engine=leveldb`. After every write run, `shale_bench` waits until the background is idle and reports the ops/s both when
the last write returned and counting the wait ("settled").

```
cmake -S $WORK/<build> -B $WORK/<build>/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHALE_BUILD_BENCH=ON \
  -DSHALE_BUILD_TESTS=OFF -DCMAKE_PREFIX_PATH=$(brew --prefix leveldb)
cmake --build $WORK/<build>/build --target shale_bench

caffeinate -ims $WORK/<build>/build/tools/shale_bench --engine=shale \
  --benchmarks=fillseq,fillrandom,fillsync,overwrite \
  --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-<build>-writes.json
caffeinate -ims $WORK/<build>/build/tools/shale_bench --engine=shale --existing_db=overwrite \
  --benchmarks=readrandom,readmissing,readseq,seekrandom \
  --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-<build>.json
```

The commands as given, one run of each (`<build>` is `v011` and `final`). Writes, on an empty database, when the last
write returned and settled:

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| fillseq | 360,706 (358,996 to 360,796) | 313,473 (311,509 to 316,063) | 338,960 (326,363 to 342,453) | 0.87 | 0.92 |
| fillseq, settled | 360,013 (358,215 to 360,173) | 312,780 (310,864 to 315,469) | 338,363 (325,502 to 341,631) | 0.87 | 0.92 |
| fillrandom | 250,363 (241,797 to 251,102) | 209,385 (209,207 to 213,567) | 178,775 (163,880 to 210,993) | 0.84 | 1.17 |
| fillrandom, settled | 249,965 (241,391 to 250,694) | 209,084 (208,916 to 213,229) | 172,092 (157,229 to 201,129) | 0.84 | 1.21 |
| fillsync | 346 (345 to 348) | 346 (345 to 347) | 385 (382 to 398) | 1.00 | 0.90 |
| fillsync, settled | 346 (345 to 348) | 346 (345 to 347) | 385 (382 to 398) | 1.00 | 0.90 |
| overwrite | 193,397 (192,082 to 196,570) | 162,654 (161,840 to 164,878) | 223,227 (217,277 to 224,608) | 0.84 | 0.73 |
| overwrite, settled | 166,806 (166,611 to 169,109) | 162,442 (161,639 to 164,669) | 216,430 (210,142 to 216,966) | 0.97 | 0.75 |

Reads, first pass (each build on a clone of its engine's first-pass database):

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| readrandom | 394,177 (387,538 to 395,913) | 389,563 (379,900 to 392,913) | 546,741 (468,450 to 549,673) | 0.99 | 0.71 |
| readmissing | 2,994,761 (2,936,521 to 3,013,150) | 3,259,578 (3,242,481 to 3,263,005) | 1,654,776 (1,629,968 to 1,674,796) | 1.09 | 1.97 |
| readseq | 4,628,391 (4,611,174 to 4,700,788) | 9,180,429 (9,170,915 to 9,202,391) | 13,588,903 (13,579,814 to 13,589,834) | 1.98 | 0.68 |
| seekrandom | 193,669 (193,620 to 194,718) | 233,593 (229,226 to 235,515) | 451,754 (448,176 to 453,186) | 1.21 | 0.52 |

The same, with `--compact_before_reads`: the clone is compacted once before `readrandom`, which is not timed.

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| readrandom | 407,290 (400,183 to 409,967) | 416,153 (414,665 to 423,433) | 539,362 (538,764 to 541,124) | 1.02 | 0.77 |
| readmissing | 4,423,588 (4,408,362 to 4,427,446) | 5,106,411 (5,080,784 to 5,113,216) | 1,633,419 (1,595,653 to 1,646,097) | 1.15 | 3.13 |
| readseq | 6,170,841 (6,168,285 to 6,187,852) | 12,076,219 (12,075,526 to 12,130,782) | 13,555,937 (13,544,836 to 13,558,464) | 1.96 | 0.89 |
| seekrandom | 310,317 (309,694 to 312,382) | 383,729 (377,366 to 385,825) | 449,339 (448,858 to 449,442) | 1.24 | 0.85 |

`fillsync` is a single run of each build, so its ratios are single-run ratios. The write rows of these single runs are not
the figure to use either: in this session the write rows of every engine came out at one of two levels, and the single
runs above fell at different levels (v0.1.1's `fillseq` and `fillrandom` at the higher, 0.1.2's at the lower).

The figure to use for a read row or a write row is the one of three rounds that ran the builds one after the other, in
three orders (the median of the three rounds' medians, with the lowest and highest round in brackets), with the rules for
what counts as a change and what counts as level, all in
[docs/benchmarks.md](docs/benchmarks.md#2026-11-the-read-rows-in-interleaved-rounds). The reads, in rounds, first pass:

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| readrandom | 382,992 (380,552 to 383,450) | 396,613 (395,401 to 403,688) | 534,442 (522,504 to 547,834) | 1.04 | 0.74 |
| readmissing | 2,926,379 (2,919,035 to 2,941,092) | 3,250,969 (3,248,129 to 3,258,688) | 1,575,842 (1,573,047 to 1,582,442) | 1.11 | 2.06 |
| readseq | 4,647,963 (4,647,315 to 4,668,204) | 9,173,810 (8,996,831 to 9,217,036) | 13,571,276 (13,557,055 to 13,574,968) | 1.97 | 0.68 |
| seekrandom | 189,823 (188,783 to 191,651) | 235,317 (233,828 to 239,032) | 450,171 (449,685 to 453,221) | 1.24 | 0.52 |

Compacted before the reads:

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| readrandom | 407,256 (406,118 to 413,343) | 423,632 (413,987 to 425,260) | 537,358 (537,279 to 540,423) | 1.04 | 0.79 |
| readmissing | 4,414,066 (4,413,326 to 4,541,999) | 5,100,292 (4,960,617 to 5,125,091) | 1,596,117 (1,569,261 to 1,626,000) | 1.16 | 3.20 |
| readseq | 6,116,191 (6,077,071 to 6,138,162) | 12,096,543 (12,083,442 to 12,157,581) | 13,576,089 (13,573,985 to 13,580,790) | 1.98 | 0.89 |
| seekrandom | 307,174 (306,945 to 317,919) | 383,746 (372,749 to 388,040) | 451,574 (450,732 to 454,165) | 1.25 | 0.85 |

The writes, in rounds, when the last write returned and settled:

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| fillseq | 354,822 (313,406 to 356,954) | 356,580 (312,403 to 359,072) | 368,327 (335,306 to 375,897) | 1.00 | 0.97 |
| fillseq, settled | 353,971 (312,797 to 356,043) | 355,718 (311,742 to 358,249) | 367,499 (334,578 to 375,137) | 1.00 | 0.97 |
| fillrandom | 215,313 (214,456 to 255,315) | 254,335 (216,133 to 255,022) | 260,094 (222,644 to 263,093) | 1.18 | 0.98 |
| fillrandom, settled | 214,989 (214,114 to 254,923) | 253,919 (215,815 to 254,587) | 248,153 (212,728 to 251,000) | 1.18 | 1.02 |
| overwrite | 193,321 (193,101 to 232,344) | 210,310 (170,588 to 211,349) | 260,332 (217,565 to 261,279) | 1.09 | 0.81 |
| overwrite, settled | 167,027 (165,769 to 199,659) | 210,008 (170,371 to 211,027) | 252,279 (211,169 to 253,045) | 1.26 | 0.83 |

The compaction of the whole key range before the compacted reads, of the same files in every build, timed by
`shale_bench` (four runs of each build; E is the first of the three changes, the table iterator, E+F adds readahead, and
E+F+G is 0.1.2; [docs/benchmarks.md](docs/benchmarks.md#2026-11-e-made-compaction-faster) has the counters after
`overwrite`):

| | v0.1.1 | E | E+F | E+F+G (0.1.2) |
| --- | ---: | ---: | ---: | ---: |
| compaction of the whole range, s | 2.593 (2.554 to 2.671) | 1.530 (1.515 to 1.564) | 1.574 (1.539 to 1.587) | 1.509 (1.473 to 1.528) |

What each row is, how the engines were matched, and the latency and write-amplification figures are in
[docs/benchmarks.md](docs/benchmarks.md).
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
