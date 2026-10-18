# Benchmarks

What shale does on one machine, next to what LevelDB does on the same machine through the same
workloads. The measured figures in this file are in the JSON files in [`benchmarks/`](benchmarks/),
which `shale_bench` wrote. The counts and shares in the explanations of the slower rows are in
[`benchmarks/2026-10-m2-evidence.txt`](benchmarks/2026-10-m2-evidence.txt), which also has the machine record and
the load of every run, and says how each was taken.

In short: on this machine LevelDB is faster than shale on six of the eight workloads: shale's ops/s are
0.91 to 0.22 of LevelDB's on those six in the first pass, and 0.89 to 0.42 in the second, where the database was
compacted before it was read. They are level on synced writes, and shale is faster at looking up keys that are not
there. The reasons are
given below, row by row. Where the two engines could not be given the same settings, the difference is listed.

## What was measured

**Machine.** Apple M2, 8 cores (4 performance, 4 efficiency), 16 GiB, macOS 26.6.2 (build 25G83),
`uname -srm` is `Darwin 25.6.0 arm64`. The database was on the internal SSD, an APFS volume that was
94% full. On AC power, battery charged. The machine was not idle: the window server and a web
browser's renderer were running throughout, and `top`, looked at before the runs, showed about 80% of
the CPU idle.

**shale.** A Release build with Apple clang 21.0.0 (clang-2100.3.34.2). The flags, from the
compile commands CMake wrote, are `-O3 -DNDEBUG -std=c++20 -arch arm64 -Wall -Wextra -Wpedantic
-Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Werror
-Wthread-safety`. There is no link-time optimization. `shale_bench` and the LevelDB adapter were
built with the same flags. A Debug or sanitizer build was not measured.

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

| run | started (2026-10-18) | 1-minute load average at the start | 5-minute | took |
| --- | --- | ---: | ---: | ---: |
| shale | 03:44:08 | 1.85 | 2.28 | 3 min 6 s |
| LevelDB | 03:49:27 | 1.95 | 2.41 | 2 min 26 s |
| shale, compacted before the reads | 04:13:40 | 1.89 | 2.63 | 2 min 42 s |
| LevelDB, compacted before the reads | 04:21:38 | 1.93 | 2.48 | 2 min 29 s |

Before each run the machine was checked: a 1-minute load average under 2.0, and none of `ninja`,
`clang`, `cmake`, `ctest`, `ld64`, `shale_` or `fuzz_` running. The 5-minute averages show that it was quiet
by that rule and was not idle: builds, tests and profiling had run just before, and the window server and
the browser were there all along.

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
  The counts are with the read rows below.
- **Checksums are computed differently.** shale uses the processor's CRC32C instructions. This LevelDB
  computes them in software, and the profiles show it: 24% to 47% of the samples of the thread that reads.
- **`paranoid_checks`** is true for shale and false, LevelDB's default, for LevelDB. In LevelDB 1.23 it
  also decides whether a compaction verifies the checksums of the blocks it reads (`MakeInputIterator`
  in `version_set.cc`), and shale's compaction always does. So shale does more work in compaction. It is
  not much: in the profile of `overwrite`, `crc32c` is 2.8% of the samples of shale's background thread
  and 3.3% of LevelDB's, and LevelDB's would roughly double if it checked what it reads as well as what it writes.
- **Compaction and flushing are each engine's own.**

## Results: the commands as given

The reads follow `overwrite` directly, and the compaction that `overwrite` left is still to be done
while they run.

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

| benchmark | engine | MB/s | p50 us | p99 us | p99.9 us | write amplification |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| fillseq | shale | 42.14 | 1.71 | 4.33 | 8.21 | 2.17 |
| fillseq | LevelDB | 49.61 | 1.67 | 4.58 | 9.83 | n/a |
| fillrandom | shale | 28.69 | 2.00 | 5.04 | 71.29 | 4.96 |
| fillrandom | LevelDB | 31.52 | 2.04 | 6.17 | 59.54 | n/a |
| fillsync | shale | 0.04 | 2,996.25 | 3,404.67 | 9,442.54 | 1.19 |
| fillsync | LevelDB | 0.04 | 2,998.33 | 3,673.75 | 9,468.38 | n/a |
| overwrite | shale | 21.03 | 2.08 | 5.42 | 821.21 | 5.82 |
| overwrite | LevelDB | 30.73 | 2.04 | 6.21 | 63.33 | n/a |
| readrandom | shale | 31.08 | 3.42 | 6.50 | 10.21 | - |
| readrandom | LevelDB | 48.02 | 2.17 | 3.04 | 3.88 | - |
| readmissing | shale | 0.00 | 0.75 | 3.79 | 5.50 | - |
| readmissing | LevelDB | 0.00 | 0.88 | 1.50 | 2.79 | - |
| readseq | shale | 311.86 | - | - | - | - |
| readseq | LevelDB | 1,374.24 | - | - | - | - |
| seekrandom | shale | 8.78 | 12.54 | 20.79 | 28.12 | - |
| seekrandom | LevelDB | 40.44 | 2.62 | 3.83 | 6.62 | - |


Write amplification is the engine's own figure, `shale.write-amplification`: the bytes of the log, the
flushes and the compactions over the key and value bytes that the callers wrote, since the database
was opened (for `overwrite`, since it was opened again after its load). LevelDB's is `n/a`.

The runs of `fillrandom` on shale were 264,247, 153,575 and 259,365 ops/s: the second took 6.51 s against
3.78 s and 3.86 s, for the same operations. No build, test or benchmark of ours was running, and the cause is not
known. It does not move the median. LevelDB's runs of `readrandom` were 365,583 to 434,339.

## Results: compacted before the reads

The same, with `--compact_before_reads`: the database is compacted once, between `overwrite` and
`readrandom`, which is not timed. That took 4.83 s on shale and 2.23 s on LevelDB (`compact_seconds` in the JSON).
In a probe that made the same compaction, both engines ended with all their data in one level (55 files on
shale, 56 on LevelDB). The write rows are a second run of the same commands as in the first pass.

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

| benchmark | engine | MB/s | p50 us | p99 us | p99.9 us | write amplification |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| fillseq | shale | 36.73 | 1.71 | 4.33 | 7.75 | 2.17 |
| fillseq | LevelDB | 45.58 | 1.67 | 4.50 | 9.96 | n/a |
| fillrandom | shale | 24.06 | 2.12 | 5.46 | 87.75 | 4.90 |
| fillrandom | LevelDB | 27.05 | 2.08 | 6.42 | 60.88 | n/a |
| fillsync | shale | 0.04 | 2,996.50 | 3,492.71 | 8,661.46 | 1.19 |
| fillsync | LevelDB | 0.04 | 2,998.08 | 3,524.46 | 9,421.17 | n/a |
| overwrite | shale | 20.81 | 2.12 | 5.29 | 574.92 | 5.82 |
| overwrite | LevelDB | 27.08 | 2.08 | 6.29 | 66.12 | n/a |
| readrandom | shale | 40.22 | 2.67 | 3.75 | 6.21 | - |
| readrandom | LevelDB | 56.28 | 1.83 | 2.54 | 3.12 | - |
| readmissing | shale | 0.00 | 0.21 | 0.33 | 3.33 | - |
| readmissing | LevelDB | 0.00 | 0.54 | 1.12 | 2.50 | - |
| readseq | shale | 614.34 | - | - | - | - |
| readseq | LevelDB | 1,467.53 | - | - | - | - |
| seekrandom | shale | 30.61 | 3.54 | 5.54 | 8.50 | - |
| seekrandom | LevelDB | 46.90 | 2.25 | 3.42 | 4.67 | - |


Between the passes the write rows moved by 1% to 16% on both engines. The medians were, for shale,
`fillseq` 380,882 and then 332,033, `fillrandom` 259,365 and 217,485, `overwrite` 190,098 and 188,068; for
LevelDB 448,481 and 411,985, 284,893 and 244,493, and 277,799 and 244,763. Both were lower in the second
pass, so the figures are less steady than the ratios between the engines: `fillseq` 0.85 and then 0.81,
`fillrandom` 0.91 and 0.89, `overwrite` 0.68 and 0.77.

The read rows depend much more on compaction in shale than in LevelDB. After compaction `readseq` is
2.0 times as fast on shale and 1.07 times on LevelDB, `seekrandom` 3.5 times and 1.16, `readmissing` 3.7 times and
1.56, `readrandom` 1.29 and 1.17. In the first pass shale has a tree with files in level 0 and several levels
holding data, and compactions are running while it reads: in the probe's counters, 15 more compactions
during `readrandom`.

## Where shale is slower, and why

The figures after a row name are shale's ops/s over LevelDB's, in the first pass and in the second. "The
thread" is the one that calls the engine, in a profile made with `sample` of a separate run of the same
workload (the evidence file says how). Its shares are shares of that thread's samples, by the function at
the top of the stack. They show where the time went. They do not measure what changing something would save.

### fillseq (0.85, 0.81), fillrandom (0.91, 0.89), overwrite (0.68, 0.77)

On both engines the thread spends most of its time in one `write(2)` of the log record for each put:
59%, 66% and 64% of shale's samples in the three workloads, 80%, 82% and 79% of LevelDB's. What shale has on
top of that, and LevelDB's thread does not:

- **A sync of the old log and of the directory at each memtable switch**, in the thread that is writing:
  11%, 11% and 9% of its samples. `SwitchMemTable()` syncs the old log before the first write goes into the
  new one, and the directory once the new log exists, with the database mutex held. The comment there gives the
  reason: otherwise a later write that is synced, in the new log, could survive a power loss while earlier
  writes that were not synced, in the old log, were lost, and what may be lost is only a suffix of the writes.
  It happens 35 times in a run of 1,000,000 puts (`flushes=35`). LevelDB's thread shows no sync at a memtable switch.
- **Waiting for the database mutex**: 11%, 5% and 4% of the samples. The background thread holds the mutex
  while it appends the change of version to the MANIFEST and syncs it (`LogAndApply()`), and the writer waits in
  `CommitGroup()`. In `fillseq` the background thread has 11% of its own samples in `LogAndApply()`, as much as
  the writer's wait. LevelDB's thread waits for 0.1% to 0.4%.
- **Sleeping, in `overwrite`**: 7% of the samples. Level 0 reached 8 files and writes were slowed with a sleep of
  1 ms, 372 times in the probe's counters (`write_slowdowns`). shale's background thread was idle for 5% of the
  window and LevelDB's for 17%. In the probe the same workload wrote 500 MiB of tables in shale (109 MiB flushed,
  391 MiB compacted) and 429 MiB in LevelDB (107 MiB and 322 MiB, by its own statistics): about a sixth more.

The first two are 22% of the thread's samples in `fillseq` and 16% in `fillrandom`; with the sleeps, all three are 20% in
`overwrite`. shale's gap in those rows, as a share of its time per put, is 15% and 19% in `fillseq`, 9% and 11% in
`fillrandom`, and 32% and 23% in `overwrite`. So what the profile shows covers `fillseq` and `fillrandom` in full, and
`overwrite` in part: about two thirds of the gap in the first pass and most of it in the second. The rest is not explained
by anything in the profile. In `fillrandom` the ranges of the runs of the two engines overlap in the second pass
(shale 208,302 to 221,172, LevelDB 219,171 to 252,626), so that difference is the least certain of the three.

The syncs are a decision of shale's about what a power loss may take, and this file does not weigh it against their cost.

### readrandom (0.65, 0.71)

shale made 958,642 `pread` calls for 1,000,000 gets in the first state and 926,670 in the second, of
4.1 KB each: nearly every get reads a block, since 8 MiB of block cache holds only a small part of a 116 MB
database. LevelDB made none, and reads out of its mapping. In the thread, `pread` is 26% of shale's samples in the
first state and 34% in the second, against gaps of 35% and 29% of shale's time per get. Allocating, freeing and zero-filling
come to 15% and 18% (LevelDB's 11% and 9%); the zero-fill of the buffer that `ReadBlock()` reads into is 5% of
it in the second state. LevelDB's thread spends 38% and 47% of its samples in its software CRC32C, where shale spends 13% and
15% in hardware, so the two costs of reading a block partly cancel and the syscall is what is left over. In the first
pass the compactions that `overwrite` left were still running during the reads, on both engines: in one probe
run the database wrote 63 MB with 83 `F_FULLFSYNC` calls during `readrandom` on shale and 314 MB with 278 on LevelDB.

### readseq (0.23, 0.42)

shale made 33,051 `pread` calls for the 1,000,000 entries in the first state and 27,710 in the second (30 and
36 entries to a block). LevelDB made none. `pread` is 8% and 9% of shale's samples, the checksum 6% and 7%, allocating and
zero-filling 6% and 8%. Another 12.5% and 11.2% is a single function: `Status::ok()`, which is `return code_ == Code::kOk;`,
defined in `status.cc`, and which the iterator layers call as a real call, since nothing is optimized across
files in this build. The rest of the thread, 60% and 57%, is the iterator code itself: a merging iterator over a chain of
two-level and block iterators, each layer answering `Valid()`, `key()` and `Next()` through a virtual call. In the
second state LevelDB takes 0.42 of shale's time per entry and spends 56% of that in the corresponding code, which per entry
is 2.4 times less than shale spends there.

### seekrandom (0.22, 0.65)

A seek on shale made 2.26 `pread` calls in the first state and 0.95 in the second (LevelDB none). That is
consistent with the tree: in the first state it has files in level 0 and several levels holding data, and a seek has to
be positioned in each of them, and in the compacted state there is one level. `pread` is 22% and 23% of the thread's
samples. Allocating, freeing and zero-filling is 24% and 27% (LevelDB 12% and 18%), of which the zero-fill of the block
buffer in `ReadBlock()` is 5%. The checksum is 10% and 12% (LevelDB 47% and 41%). Seeks are 4.6 times slower than
LevelDB's in the first pass and 1.5 times in the second.

### What these point to

Read together, the profiles point to: the `pread` per block and the buffer it reads into, on every read row; the layers
of iterator calls, and `Status::ok()` in particular, on the scans; and the sync of the old log and the directory, and the
manifest sync, under the mutex, on the writes. Each is a place to look, and none is a measurement of what fixing it
would give. Nothing here changes the engine.

## Where shale is not slower

- **fillsync** (1.03, 1.03). 345 ops/s against 335 in both passes, and the ranges of the runs do not overlap.
  Both are bound by the flush that every put waits for, one `F_FULLFSYNC` each: the p50 of about 3.0 ms is the same on
  both, and so is the p99.9 of about 9.4 ms. A difference of 3% is shown here, and is not explained.
- **readmissing** (1.05, 2.50). On both engines a key that is not there is answered by the Bloom filter, and a block is
  read only when the filter says maybe: shale made 16,990 `pread` calls for 1,000,000 lookups in the first state and 5,453
  in the second (1.7% and 0.55%, which is of the order that a Bloom filter of 10 bits a key lets through), LevelDB none. In the
  compacted state there is one level to look in, and shale's p50 is 0.21 us against LevelDB's 0.54 us. Why LevelDB's lookup costs
  more than shale's there was not examined.

## What these numbers do not say

- They are one machine on one day, in two passes. The ratios that the two passes agree on are firmer than any one figure.
- The machine was not idle. The 1-minute load average was under 2.0 at the start of every run and the 5-minute
  average was not. A browser renderer and the window server used part of a core all the time.
- The disk was 94% full, and the write rows moved by up to 16% between the passes.
- The database fits in the page cache. A read that goes to the disk is a different benchmark.
- LevelDB is the Homebrew bottle, whose optimization level is not known, and whose tables are read through a mapping:
  its default, and not what shale does. Both are part of the read gaps, so a LevelDB built like shale would not
  necessarily be the same competitor.
- The profiles are of separate runs, on the same machine, which was not held quiet. They say where the time goes in a
  run and not what a change would save.
- Every read in every run was checked against a record of what was written, and none was wrong. That is a check on the
  answers, not on the speed.

## Reproducing

Build `shale_bench` as above, with LevelDB installed (`brew install leveldb`). The JSON of each run has the machine
(`uname -srm`, the processor, the cores, the clock), the build (compiler, version, build type) and every run's figures,
and no host name. `shale_bench --help` says what each figure is.
