# Benchmarks

What shale does on one machine, next to what LevelDB does on the same machine through the same
workloads, and what four changes made after a first measurement did to it. Every measured figure in
this file is in a JSON file in [`benchmarks/`](benchmarks/), which `shale_bench` wrote. The counts and
shares in the explanations are in [`benchmarks/2026-10-m2-evidence.txt`](benchmarks/2026-10-m2-evidence.txt),
which also has the machine record and the load of every run, and says how each was taken.

In short, measured on 2026-10-18 with both engines in the same session:

- **Writes.** On `fillseq` and `fillrandom` shale is level with LevelDB: 0.99 and 0.98 of its ops/s in
  three interleaved rounds of the two engines, against 0.87 and 0.90 before the changes. `overwrite` is
  0.84 (0.81 before). The single runs of the full commands put shale above LevelDB on all three in the first
  pass and below it in the second; the write rows of this machine move by up to 16% from one run to the next,
  which is why the interleaved rounds are the figure to use. Synced writes are level (1.02, 1.03).
- **Reads.** LevelDB is faster at reading what is there: `readrandom` 0.70 and 0.74, `readseq` 0.28 and 0.46,
  `seekrandom` 0.28 and 0.68 (first pass, and compacted before the reads). shale is faster at keys that
  are not there: `readmissing` 1.21 and 2.49.
- **The changes.** Four inefficiencies that the first measurement found were fixed, without weakening any
  durability guarantee. Inlining `Status::ok()` and the key comparison made `readseq` 9% faster in the compacted
  pass, and the first-pass reads 7% to 59% faster in runs that are less steady; reading blocks into buffers that are
  not zero-filled, 2% on the point reads and seeks; writing the MANIFEST without the database mutex held, nothing
  measurable on its own; taking the syncs of a memtable switch off the writer's path, 4% to 13% on the write rows in the
  interleaved rounds.
  Each is below, with its figures.

## What was measured

**Machine.** Apple M2, 8 cores (4 performance, 4 efficiency), 16 GiB, macOS 26.6.2 (build 25G83),
`uname -srm` is `Darwin 25.6.0 arm64`. The database was on the internal SSD, an APFS volume that was
94% full. On AC power, battery charged. The machine was not idle: the window server and a web
browser's renderer were running throughout.

**shale.** A Release build with Apple clang 21.0.0 (clang-2100.3.34.2). The flags, from the
compile commands CMake wrote, are `-O3 -DNDEBUG -std=c++20 -arch arm64 -Wall -Wextra -Wpedantic
-Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Werror
-Wthread-safety`. There is no link-time optimization. `shale_bench` and the LevelDB adapter were
built with the same flags. A Debug or sanitizer build was not measured. The builds of the commits
before the last one, for the figures of each change, were made the same way from a clean export of
each commit (with `-DSHALE_BUILD_TESTS=OFF`, which builds the same libraries).

**LevelDB.** Version 1.23: the Homebrew bottle `leveldb 1.23_2` for arm64, used as the shared
library `libleveldb.1.23.0.dylib`, with `snappy 1.3.1` beside it. It was not built here, so its
compiler flags are not known. Homebrew's formula asks CMake for a Release build, and Homebrew's build
environment, as installed here, defaults to `-Os` (`HOMEBREW_OPTIMIZATION_LEVEL`). Which of the two decided the
optimization level of the bottle cannot be told from the bottle. A LevelDB built with shale's flags
might be faster than the one measured.

**Workloads.** `shale_bench`, which runs both engines through the same code: the same keys
(`%016llu`), the same values (100 bytes), the same operations in the same order, timed by the same
loop, and every read checked against a record of what was written. 1,000,000 keys; `fillsync` does
10,000 puts. Each benchmark runs three times, and each figure is the **median of the three runs, with the lowest and the
highest run in brackets**. Each write run starts from an empty database. The database is about 116 MB of keys
and values, so reads are served from the operating system's page cache and not from the disk.

**Commands.** The build, and then each engine writing its own JSON file. `$DB` is an empty directory on the
internal SSD.

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

and the same two commands with `--compact_before_reads` added, writing `2026-10-m2-compacted.json` and
`2026-10-m2-leveldb-compacted.json`. `fillsync` comes before `overwrite`, which is the tool's own order: a read
benchmark reads what the write before it left, and after `fillsync` that is a database of 10,000
keys, not the 1,000,000 that `overwrite` leaves.

Before each run the machine was checked: a 1-minute load average under 2.0, and none of `ninja`,
`clang`, `cmake`, `ctest`, `ld64`, `shale_` or `fuzz_` running. The check waited, for up to two hours,
for both to hold. The 5-minute averages show that the machine was quiet by that rule and was not idle.

| run | started (2026-10-18) | 1-minute load average at the start | 5-minute | took |
| --- | --- | ---: | ---: | ---: |
| shale | 07:17:50 | 1.83 | 2.54 | 2 min 47 s |
| shale, compacted before the reads | 07:58:03 | 1.91 | 2.39 | 2 min 35 s |
| LevelDB | 08:16:13 | 1.96 | 2.45 | 2 min 31 s |
| LevelDB, compacted before the reads | 08:28:31 | 1.82 | 2.49 | 2 min 28 s |

(For the last run the check saw 1.82 and the `uptime` that followed it a moment later 2.16.)

The measurement before the changes, from earlier the same day, is kept in the files that end in
`-before.json`: `2026-10-m2-before.json`, `2026-10-m2-compacted-before.json`, `2026-10-m2-leveldb-before.json` and
`2026-10-m2-leveldb-compacted-before.json`. Its machine record and loads are in section 0 of the evidence file.

## How the engines were matched

| | shale | LevelDB |
| --- | --- | --- |
| write buffer | 4 MiB | 4 MiB |
| block size | 4096 | 4096 |
| Bloom filter | 10 bits a key | 10 bits a key |
| block cache | 8 MiB | 8 MiB, new at every open, as shale's is |
| open files | 1000 | 1000 |
| compression | none | none |
| checksums verified on every read | yes | yes (`verify_checksums`) |
| table size, restart interval, level 0 triggers (4, 8, 12), level sizes (10 MiB, times 10) | the defaults | LevelDB's defaults, which are the same numbers (read from its 1.23 sources) |
| a synced write | `F_FULLFSYNC` | `F_FULLFSYNC` |
| write amplification | shown | `n/a` |

What is not the same, and was left as each engine has it:

- **A synced write is the same on both.** shale's sync on macOS is `fcntl(F_FULLFSYNC)`, which asks
  the drive to flush its cache. So is LevelDB's in this build. With a library that counts calls, 200
  synced puts made 209 `F_FULLFSYNC` calls and no `fsync` on shale, and 206 and no `fsync` on LevelDB (a few
  more than the puts, which the database makes for its own files).
- **Tables are read differently.** shale reads a block with `pread`. LevelDB maps its table files into
  memory, which is its default on 64-bit systems (for up to 1000 files), and reads blocks out of the mapping.
  shale's scope leaves out performance features that do not change its correctness argument, and reading
  through a mapping is one.
- **Checksums are computed differently.** shale uses the processor's CRC32C instructions. This LevelDB
  computes them in software, and the profiles show it: 24% to 47% of the samples of the thread that reads.
- **`paranoid_checks`** is true for shale and false, LevelDB's default, for LevelDB. In LevelDB 1.23 it
  also decides whether a compaction verifies the checksums of the blocks it reads (`MakeInputIterator`
  in `version_set.cc`), and shale's compaction always does. So shale does more work in compaction. It is
  not much: in the profile of `overwrite`, `crc32c` was 2.8% of the samples of shale's background thread
  and 3.3% of LevelDB's.
- **A table is synced, and then its directory.** shale syncs the directory after each table it writes, so
  that the table's name survives a power loss before anything names it; LevelDB syncs the table and not the
  directory. That is one more `F_FULLFSYNC` for every table of a flush or a compaction.
- **Compaction and flushing are otherwise each engine's own.**

## Results: the commands as given

The reads follow `overwrite` directly, and the compaction that `overwrite` left is still to be done
while they run.

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

| benchmark | engine | MB/s | p50 us | p99 us | p99.9 us | write amplification |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| fillseq | shale | 48.81 | 1.62 | 4.21 | 8.00 | 2.17 |
| fillseq | LevelDB | 45.15 | 1.67 | 4.58 | 10.62 | n/a |
| fillrandom | shale | 32.13 | 1.92 | 4.92 | 157.04 | 4.90 |
| fillrandom | LevelDB | 27.33 | 2.08 | 6.33 | 58.21 | n/a |
| fillsync | shale | 0.04 | 2,996.21 | 3,540.50 | 8,007.12 | 1.19 |
| fillsync | LevelDB | 0.04 | 2,998.08 | 3,445.79 | 9,420.46 | n/a |
| overwrite | shale | 26.55 | 1.96 | 5.04 | 465.46 | 5.64 |
| overwrite | LevelDB | 26.31 | 2.17 | 6.33 | 93.88 | n/a |
| readrandom | shale | 33.69 | 3.21 | 5.88 | 8.29 | - |
| readrandom | LevelDB | 47.78 | 2.21 | 3.08 | 3.79 | - |
| readmissing | shale | 0.00 | 0.67 | 3.46 | 4.83 | - |
| readmissing | LevelDB | 0.00 | 0.88 | 1.54 | 3.12 | - |
| readseq | shale | 384.81 | - | - | - | - |
| readseq | LevelDB | 1,377.67 | - | - | - | - |
| seekrandom | shale | 11.42 | 9.62 | 15.83 | 19.58 | - |
| seekrandom | LevelDB | 40.54 | 2.62 | 3.83 | 7.54 | - |

Write amplification is the engine's own figure, `shale.write-amplification`: the bytes of the log, the
flushes and the compactions over the key and value bytes that the callers wrote, since the database
was opened (for `overwrite`, since it was opened again after its load). LevelDB's is `n/a`.

## Results: compacted before the reads

The same, with `--compact_before_reads`: the database is compacted once, between `overwrite` and
`readrandom`, which is not timed. That took 4.23 s on shale and 2.13 s on LevelDB (`compact_seconds` in the JSON).
The write rows are a second run of the same commands as in the first pass.

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

| benchmark | engine | MB/s | p50 us | p99 us | p99.9 us | write amplification |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| fillseq | shale | 41.76 | 1.67 | 4.29 | 7.96 | 2.17 |
| fillseq | LevelDB | 45.78 | 1.67 | 4.54 | 10.29 | n/a |
| fillrandom | shale | 26.86 | 2.04 | 5.29 | 149.42 | 4.90 |
| fillrandom | LevelDB | 27.61 | 2.08 | 6.21 | 65.04 | n/a |
| fillsync | shale | 0.04 | 2,996.62 | 3,458.88 | 9,414.17 | 1.19 |
| fillsync | LevelDB | 0.04 | 2,997.92 | 3,475.96 | 9,073.58 | n/a |
| overwrite | shale | 23.49 | 2.04 | 5.21 | 451.46 | 5.77 |
| overwrite | LevelDB | 27.21 | 2.08 | 6.25 | 75.21 | n/a |
| readrandom | shale | 41.58 | 2.58 | 3.54 | 6.00 | - |
| readrandom | LevelDB | 55.91 | 1.83 | 2.58 | 3.29 | - |
| readmissing | shale | 0.00 | 0.21 | 0.33 | 3.21 | - |
| readmissing | LevelDB | 0.00 | 0.54 | 1.12 | 2.54 | - |
| readseq | shale | 670.66 | - | - | - | - |
| readseq | LevelDB | 1,471.26 | - | - | - | - |
| seekrandom | shale | 31.81 | 3.42 | 5.38 | 8.42 | - |
| seekrandom | LevelDB | 46.94 | 2.25 | 3.42 | 4.71 | - |

## The write rows, in interleaved rounds

The write rows of this machine are not steady from one run to the next: the final build of shale ran `fillseq`
at 441,200, 368,975 and 431,804 ops/s in three runs of the first pass (`2026-10-m2.json` and the two files in
[`2026-10-m2-repeats/`](benchmarks/2026-10-m2-repeats/) named `final`), and LevelDB at 448,481 in the earlier
measurement and at 408,109 in this one. A difference between two single runs of the full commands, made
minutes apart, can be that. So the write benchmarks were also run on their own, in three rounds, each round
running four builds one after the other, each run after the same check for a quiet machine: shale before the
MANIFEST and switch changes (the build with the first two changes, which do not touch writing), with the
MANIFEST change, with both, and LevelDB. The JSON of every run is in
[`benchmarks/2026-10-m2-writes/`](benchmarks/2026-10-m2-writes/), and the table gives the median of the three
rounds' medians, ops/s.

| benchmark | shale before | with the MANIFEST change | with both | LevelDB | before / LevelDB | after / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| fillseq | 388,887 | 390,531 | 441,871 | 447,959 | 0.87 | 0.99 |
| fillrandom | 262,190 | 266,595 | 287,276 | 292,827 | 0.90 | 0.98 |
| overwrite | 233,344 | 232,679 | 242,683 | 289,526 | 0.81 | 0.84 |

The rounds were: `fillseq` 340,883, 388,887 and 396,965 for shale before; 365,588, 392,934 and 390,531 with the
MANIFEST change; 439,781, 441,871 and 444,625 with both; 456,173, 447,959 and 403,325 for LevelDB. The slow runs
of shale before (round 1) and of LevelDB (round 3) are the same unsteadiness as above, and the medians leave them out.

## What profiling found, and what changed

The first measurement profiled the thread that calls the engine in each row where shale was slower (section
4 and 5 of the evidence file). Four of the costs it found were defects or plain inefficiency rather than design,
and each was changed in a commit of its own. The tables below give each change's rows, before and after, as
ops/s, from these files:

| step | first pass | compacted before the reads |
| --- | --- | --- |
| before, the earlier measurement | `2026-10-m2-before.json` | `2026-10-m2-compacted-before.json` |
| before, the same build run again in this session | `2026-10-m2-before-rerun.json` | `2026-10-m2-compacted-before-rerun.json` |
| A: inline `Status::ok()` and the key comparison | `2026-10-m2-a.json` | `2026-10-m2-a-compacted.json` |
| A and B: block buffers not zero-filled | `2026-10-m2-ab.json` | `2026-10-m2-ab-compacted.json` |
| A, B and C: the MANIFEST synced without the mutex | `2026-10-m2-abc.json` | `2026-10-m2-abc-compacted.json` |
| A to D: the switch's syncs off the writer's path | `2026-10-m2.json` | `2026-10-m2-compacted.json` |

Each change is measured against the step before it, run in the same session. The earlier measurement is there
because the changes were chosen from it; the rerun of the same build, made in this session just before the
others, is the one to compare with.

| benchmark | before (earlier) | before (rerun) | A | A, B | A, B, C | A to D |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| fillseq | 380,882 | 380,946 | 392,143 | 387,658 | 378,752 | 441,200 |
| fillrandom | 259,365 | 265,818 | 258,973 | 264,550 | 261,223 | 290,445 |
| fillsync | 345 | 345 | 337 | 343 | 345 | 345 |
| overwrite | 190,098 | 218,290 | 227,053 | 225,842 | 226,470 | 239,958 |
| readrandom | 280,939 | 291,726 | 312,137 | 320,286 | 316,676 | 304,512 |
| readmissing | 1,125,959 | 1,235,898 | 1,534,402 | 1,524,909 | 1,521,744 | 1,294,124 |
| readseq | 2,819,038 | 3,112,028 | 4,243,946 | 4,223,371 | 4,221,056 | 3,478,488 |
| seekrandom | 79,349 | 99,412 | 158,461 | 161,597 | 160,061 | 103,190 |

Compacted before the reads:

| benchmark | before (earlier) | before (rerun) | A | A, B | A, B, C | A to D |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| fillseq | 332,033 | 385,915 | 335,596 | 339,064 | 343,744 | 377,485 |
| fillrandom | 217,485 | 268,410 | 220,090 | 217,419 | 218,375 | 242,763 |
| fillsync | 345 | 345 | 346 | 348 | 342 | 345 |
| overwrite | 188,068 | 185,571 | 189,232 | 196,360 | 195,441 | 212,334 |
| readrandom | 363,602 | 367,031 | 367,466 | 375,442 | 372,874 | 375,821 |
| readmissing | 4,183,217 | 4,152,400 | 4,194,939 | 4,152,866 | 4,194,180 | 4,111,598 |
| readseq | 5,553,271 | 5,600,749 | 6,086,501 | 6,016,030 | 6,023,295 | 6,062,353 |
| seekrandom | 276,667 | 281,476 | 281,285 | 287,421 | 284,646 | 287,504 |

**The first pass's read rows have two levels.** Five runs of the full commands in this session, of two builds, read in
the first pass at one of two levels, run by run: `readseq` at about 4.2 million entries a second or at 3.2 to 3.5
million, `seekrandom` at about 158,000 or at 82,000 to 103,000, `readmissing` at about 1.5 million or at 1.2 to 1.3
million. The build with A, B and C was at the higher level both times (`2026-10-m2-abc.json`,
[`2026-10-m2-repeats/abc-1.json`](benchmarks/2026-10-m2-repeats/)). The final build was at the lower level in the run of
the tables (`2026-10-m2.json`) and in one of the two runs made after it (`2026-10-m2-repeats/final-2.json`), and at the
higher level in the other (`2026-10-m2-repeats/final-1.json`). Within a run, the three repetitions of a read benchmark
agree to about 1%; the compacted pass has no such levels, and in it the final build reads as fast as the build before D.
So the fall of the first-pass read rows from the A, B, C column to the last one is the lower level and not D, which
touches nothing that a read does. What decides the level was not found: replaying the same sequence of workloads with
the probe of the evidence file, five times over the two builds, gave the higher level every time, with the same tree
under the reads (section 7 of the evidence file). The two runs of the build before the changes read `readseq` at 2.8
and 3.1 million in the first pass; whether that is its lower level or its higher cannot be told from two runs, so A's
first-pass figures below may include the difference between the levels, and its compacted figures are the ones to rely
on.

### A. `Status::ok()` and the key comparison, inline

**Found.** In `readseq`, 12.5% of the thread (11.2% compacted) was in `Status::ok()` itself, which is
`return code_ == Code::kOk;`, defined in `status.cc`: every `Valid()` of the table iterators asks its own status
first, and with no link-time optimization each was a real call. `CompareInternalKey()`, called for every entry by
the table iterators' order checks, was 3.1%.

**Changed.** `Status`'s accessors are defined in its class in `include/shale/status.h`, and `ExtractUserKey()`,
`BytewiseCompare()` and `CompareInternalKey()` in `util/dbformat.h`, inline.

**Measured.** Against the rerun of the build before it: `readseq` 3,112,028 to 4,243,946 (+36%) in the first pass and
5,600,749 to 6,086,501 (+9%) compacted; `seekrandom` 99,412 to 158,461 (+59%) and 281,476 to 281,285 (no change);
`readmissing` 1,235,898 to 1,534,402 (+24%) and 4,152,400 to 4,194,939 (+1%); `readrandom` 291,726 to 312,137 (+7%) and
367,031 to 367,466 (no change). The first pass, with level 0 and several levels to look in, makes many more
comparisons per operation than the compacted tree, which is where the inlined comparison would show most; but the
first pass's read rows have two levels (above), and how much of its gain is A is not certain. The compacted gain in
`readseq`, 9%, is. `Status::ok()` does
not appear in the profiles of the final build (section 7 of the evidence file). The block and merging iterators
still call the comparison through a function pointer, and there it is still a call: about 4% of `readrandom` and
`seekrandom`.

### B. Block buffers that are not zero-filled

**Found.** Every miss of the block cache reads a block into a buffer, which `ReadBlock()` made by resizing a
`std::string`, so it was filled with zeros first and then overwritten by the read: about 5% of the thread in
`readrandom` and `seekrandom` after compaction.

**Changed.** A block's bytes are in a buffer made with `std::make_unique_for_overwrite`, which the read fills
directly. Nothing reads a byte the file did not supply: a short read is an error before the checksum is looked at.

**Measured.** Against A: `readrandom` 312,137 to 320,286 (+2.6%) and 367,466 to 375,442 (+2.2%); `seekrandom` 158,461 to
161,597 (+2.0%) and 281,285 to 287,421 (+2.2%). The other rows moved by 1% or less, either way.

### C. The MANIFEST written and synced without the database mutex

**Found.** In `fillseq` the writing thread waited for the database mutex for 11% of its samples (5% in `fillrandom`,
4% in `overwrite`): the background thread held it while it appended an edit to the MANIFEST and synced it.

**Changed.** The edit is prepared with the mutex held, the mutex is given up for the append and the sync, and taken
again to install the version, as LevelDB does it. There is one writer of the MANIFEST at a time, the background task,
and the version set refuses a second one rather than relying on that.

**Measured.** Nothing, on its own. Against A and B, the write rows in the full commands moved by -2.3% to +1.4%, and in
the interleaved rounds by -0.3% to +1.7%: within what the same build does from run to run. The profile says why. In
`fillseq`, profiled in this session without and with the change, the writer's wait for the mutex fell from 7.1% of its
samples to 0.2%, and its `F_FULLFSYNC` at the memtable switch rose from 8.0% to 15.8% (section 7 of the evidence file):
the sync of the old log at the switch now ran while the background thread synced the MANIFEST, and the drive serves one
cache flush at a time. The wait moved from the mutex to the disk. With D there is no sync at the switch left to wait in.

### D. The syncs of a memtable switch off the writer's path

**Found.** At every memtable switch the writing thread synced the old log and the database directory, with the mutex
held: 11%, 11% and 9% of its samples in `fillseq`, `fillrandom` and `overwrite`, 35 switches in a run of 1,000,000 puts.

**Changed.** The switch syncs nothing. What the two syncs promised is kept another way:

- A write acknowledged as synced survives a power loss together with every write acknowledged before it. The first
  synced write after a switch syncs the old log, then the directory, then its own log, and only then returns; the flush
  of the old memtable makes both earlier syncs unnecessary, since its table is durable before its edit is.
- A power loss leaves a prefix of the acknowledged writes and never a hole. Without the sync at the switch it can take
  the unsynced end of the old log and keep unsynced writes of the new one. Writes are numbered one after another, so
  recovery checks that the first write of each log it replays follows the last write of the tables and of the logs
  before it, and stops at a log that does not, which it sets aside, renamed and kept. For the first log to be checked
  too, an edit of the MANIFEST now records the newest write that the tables hold.

The deterministic simulator checks both after every crash it makes, and a deliberate break of either rule, in a scratch
copy, makes it fail (the commit says which invariant, at which seed).

**Measured.** Against A, B and C: `fillseq` 378,752 to 441,200 (+16%) in the first pass and 343,744 to 377,485 (+10%)
compacted; `fillrandom` 261,223 to 290,445 (+11%) and 218,375 to 242,763 (+11%); `overwrite` 226,470 to 239,958 (+6%) and
195,441 to 212,334 (+9%). In the interleaved rounds: 390,531 to 441,871 (+13%), 266,595 to 287,276 (+8%) and 232,679
to 242,683 (+4%); these are the figures to rely on, since one run of the same build can move by as much as the first
pass's 16%. `fillsync` is the same, 345: when every write is synced the old log has nothing unsynced, and there
was no sync of it to save. In the profile of `fillseq` the writer now spends 81% of its samples in `write(2)` of the
log, against 75% without C and D and 68% with C alone, and 0.1% in the switch, against 8% and 16%.

## Where shale is still slower, and why

The figures after a row name are shale's ops/s over LevelDB's in the first pass and compacted, in this session, and
for the scans also with the final build's run of the first pass that read at the higher level
(`2026-10-m2-repeats/final-1.json`). "The
thread" is the one that calls the engine, in a profile made with `sample` of a separate run of the same workload on the
final build (section 7 of the evidence file). Its shares are shares of that thread's samples. They show where the time
went, and not what a change would save.

### overwrite (1.01, 0.86; 0.84 in the interleaved rounds)

The background thread cannot keep up. In the profile it was busy for 95% of the window, against 87% for LevelDB's,
and the writer slept for 5.5% of its samples, slowed for a millisecond each time level 0 reached 8 files (303 times in
the probe's counters), where LevelDB's writer did not sleep. Two things make shale's compaction the larger job: it wrote
507 MiB of tables for the overwrites against LevelDB's 429 MiB (109 MiB flushed and 398 MiB compacted, against 107 MiB and
322 MiB, by each engine's statistics), and it made 779 `F_FULLFSYNC` calls in the load and the overwrites against
LevelDB's 624, since it syncs the directory after every table. `F_FULLFSYNC` was 53% of shale's background thread.
The first pass's 1.01 is a single run; the interleaved rounds and the second pass agree on 0.84 to 0.86.

### readrandom (0.70, 0.74)

Nearly every get reads a block with `pread`: 961,868 calls for 1,000,000 gets in the first state, since 8 MiB of block
cache holds a small part of a 116 MB database. LevelDB makes none, and reads out of its mapping. `pread` is 30% of the
thread in the first state and 36% compacted, which is as large as the gap in the first state (30% of shale's time per
get) and larger than it compacted (26%). Checksums are 13% and 17% of shale's thread, in hardware, and allocation 11%
and 12%; in the earlier profile LevelDB spent 38% and 47% of its thread on checksums, in software.

### readseq (0.28, 0.46; 0.33 at the higher level of the first pass)

31,928 `pread` calls for 1,000,000 entries (LevelDB none), 11% and 15% of the thread. The rest is the iterator layers:
64% to 69% of the thread is engine code, most of it a merging iterator over a chain of iterators that each answer
`Valid()`, `key()` and `Next()` through a virtual call. For each table shale has four layers that LevelDB's two-level
iterator does not: two that check that what the table returns is in order and within the block its index names
(`OrderedIterator`, `BoundedBlockIterator`), one that names the file in an error (`NamedTableIterator`), and one that
hands the index key to the block (`KeyedIndexIterator`). Those checks are a choice of shale's, which reports a table that
is out of order as an error rather than return what it holds, and their cost here is not separated from the calls.

### seekrandom (0.28, 0.68; 0.43 at the higher level of the first pass)

A seek reads 1.5 blocks on average with `pread` in the probe's first state (1,476,058 calls for 1,000,000 seeks), 24%
and 28% of the thread; allocation and freeing are 20% and 22% (a seek builds a merging iterator and its children each
time; LevelDB's were 12% and 18% in the earlier profile), checksums 11% and 12%. In the first state a seek is positioned in three levels
that hold data, and in one after compaction (section 7.4 of the evidence file), which is why the first pass is the
slower.

## Where shale is not slower

- **fillsync** (1.02, 1.03). 345 ops/s against 337. Both are bound by the flush that every put waits for, one
  `F_FULLFSYNC` each: the p50 of about 3.0 ms is the same on both.
- **readmissing** (1.21, 2.49). On both engines a key that is not there is answered by the Bloom filter, and a block is
  read only when the filter says maybe. Why LevelDB's lookup costs more than shale's in the compacted state was not
  examined.
- **fillseq and fillrandom** are level (0.99 and 0.98 in the interleaved rounds); the single runs put shale 8% and 18%
  above LevelDB in the first pass and 9% and 3% below in the second.

## What these numbers do not say

- They are one machine on one day. The ratios that the passes and the rounds agree on are firmer than any one figure.
- The machine was not idle: the 1-minute load average was under 2.0 at the start of every run and the 5-minute
  average was not. A browser renderer and the window server used part of a core all the time.
- The disk was 94% full, and the write rows moved by up to 16% between runs of the same build.
- The database fits in the page cache. A read that goes to the disk is a different benchmark.
- LevelDB is the Homebrew bottle, whose optimization level is not known, and whose tables are read through a mapping.
- The profiles are of separate runs, on the same machine, which was not held quiet. They say where the time goes in a
  run and not what a change would save.
- Every read in every run was checked against a record of what was written, and none was wrong.

## Reproducing

Build `shale_bench` as above, with LevelDB installed (`brew install leveldb`). The JSON of each run has the machine
(`uname -srm`, the processor, the cores, the clock), the build (compiler, version, build type) and every run's figures,
and no host name. `shale_bench --help` says what each figure is.
