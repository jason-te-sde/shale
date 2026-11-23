# Benchmarks

Two measurements of shale next to LevelDB on one machine, through the same workloads and the same code (`shale_bench`).
The second, of 2026-11, is of the read path of 0.1.2 against v0.1.1 and LevelDB; the first, of 2026-10, is kept below as
it was written. The two were made on different versions of the operating system, so their figures are not comparable.

## 2026-11: the read path (0.1.2)

A second measurement, made after three changes to how reads work (below), with the builds before and after run in the same
session as LevelDB. Every figure in this section is in a JSON file in [`benchmarks/`](benchmarks/), which `shale_bench` wrote.
The counts and shares in the explanations are in [`benchmarks/2026-11-m2-evidence.txt`](benchmarks/2026-11-m2-evidence.txt).

### 2026-11: in short

Measured on 2026-11-22, in one session, with the five builds run one after the other in rounds (v0.1.1, the three
steps that made reads cheaper, and LevelDB), so every comparison below is between builds made and run the same
day on the same machine. In this document "0.1.2" is the build of `main` at the commit that merged the protocol
(`59e795e`), whose library is that of the last of the three steps; "E", "E+F" and "E+F+G" are the three steps. A ratio
is the first figure over the second, so a ratio above 1 means that the first is faster. Each figure is a median, with
the lowest and the highest run, or round, in brackets.

- **Reads after a compaction (`--compact_before_reads`).** 0.1.2 is faster than v0.1.1 on `readseq`
  (1.94), `seekrandom` (1.23) and `readmissing`
  (1.13), each by the rule of section 6 of the protocol (see "What is reported"
  below); on `readrandom` (1.01) the difference is not distinguishable from
  run-to-run variation.
- **Reads under the commands as given (the first pass).** `readseq` is faster (1.32).
  `readrandom` (0.94), `readmissing` (0.83) and
  `seekrandom` (0.59) are slower than in v0.1.1, in all three rounds. The change
  that did it is the first of the three, and with it the database that the first pass reads is a different one: the
  compactions of `overwrite` ran differently (section "Where each change moved the figures").
- **Against LevelDB.** LevelDB is faster on `readseq` (0.45 first pass,
  0.87 compacted), `seekrandom` (0.26,
  0.84) and `readrandom` (0.70,
  0.77). shale is faster on `readmissing` (1.16,
  3.07). On none of these rows are the rounds of the two engines interleaved,
  so no row is level with LevelDB by the rule.
- **Writes, in rounds.** 0.1.2 is slower than v0.1.1 on `fillseq` (0.88), `fillrandom`
  (0.82) and `overwrite` (0.72), by the rule. Against LevelDB the
  ratios are 0.92, 0.95 (0.949, just outside the 0.95 to 1.05
  band) and 0.75. The three steps did not change what is written; the counters of the
  database show that the compactions of `overwrite` differ from v0.1.1's from the first step on, and why was not
  examined.
- **Synced writes** (`fillsync`, one run of each build, so a single-run ratio): 348 ops/s against 339 for v0.1.1 and
  336 for LevelDB.
- **What the steps did.** A cached `Get` allocates 1 time in 0.1.2 against 6 in v0.1.1, and a seek whose blocks are
  cached 9 times against 39 (libc++, 16-byte keys). Readahead cut the `pread` calls of `readseq` from about 31,000 to
  932 in the first pass and from about 27,700 to 765 after a compaction.
- **Diagnostic, not a main result.** With a 1 GiB block cache for shale (and LevelDB as configured, 8 MiB), 0.1.2
  reads `readrandom` at 1.69 of LevelDB's rate after a compaction and
  1.28 in the first pass; `seekrandom` 1.72 and
  0.51. This changes a condition of the comparison.

### 2026-11: what changed

Three changes to how a table is read, each merged to `main` as one commit and measured as one step.

**E, one table iterator (`194417f`).** A read through a table used a two-level iterator over a separate
iterator for each data block, wrapped in a named iterator to say which file was damaged. A table is now read through
one iterator (`src/io/table_iterator.cc`) that walks the index and the data blocks itself, makes every check the old
path made (the block checksum whenever `verify_checksums` is set, the type byte always, keys strictly increasing, no
key past the last key its index entry names, a seek never landing before its target), and names the file in every
error. The checks were pinned by tests before the change, and none was loosened.

**F, readahead (`2f95cb6`).** A table iterator that has moved to the next data block twice in a row after a seek
reads ahead: one read of the block and the 8 KiB after it, then twice as many each time, up to 256 KiB. Each block
taken out of that buffer is copied and checked on its own, and a block that fails its check is read again by itself.
A seek, and `Get()`, read one block. There is no option for it, and no file the engine writes has changed. A table
read in order costs at most 256 KiB plus one block of buffer.

**G, fewer allocations (`c7af18b`).** A data block is allocated together with its share count and its bytes in one
allocation (`Block::MakeShared`); `Table::Get` and the iterators that a read builds live on the stack; the keys of a
block iterator and a lookup key are kept in place when they are short; and the database iterator holds the pin on the
version itself instead of in wrapper objects. `read_allocations_test` counts the allocations of a read through the
public API, so that the count is held by a test. The figures are in "Allocations per operation".

None of the three changes the public API, the disk format or a check. `shale_bench --cache_size` (`10e9eb7`) was
added for the diagnostic rows; it changes the tool and not the library.

### 2026-11: what was measured

**Machine.** Apple M2, 8 cores (4 performance, 4 efficiency), 16 GiB, macOS 27.2.1 (build 26B131), `uname -srm` is
`Darwin 27.2.0 arm64`. The database was on the internal SSD, an APFS volume that was 74% full. On AC power. The
machine was not idle: the window server and a web browser were running during the session, and the quiet check
below waited for the load to fall each time it was above its limit.

**What differs from the machine of 2026-10.** The operating system (macOS 26.6.2, build 25G83, then; `Darwin
25.6.0`), the system C++ library that `shale_bench` loads (libc++ 2200.27.0, then 2000.63.0) and libSystem (1359, then
1356). The compiler (Apple clang 21.0.0, clang-2100.3.34.2), cmake (4.4.3), LevelDB (`leveldb 1.23_2`) and snappy
(`snappy 1.3.1`) are the same. The disk was 74% full, then 94%. **The figures of 2026-11 are therefore not comparable
with those of 2026-10 below.** The comparison that this section reports is the one made inside the 2026-11 session:
v0.1.1 against the steps and LevelDB, built and run on the same day, in interleaved rounds. Where a figure of 2026-10 is
mentioned it is named as such, on that other system.

**Builds.** Four clean exports of `v0.1.1` (`9fefedf`), `194417f` (E), `2f95cb6` (E+F) and `59e795e` (0.1.2, whose
`src` and `include` equal those of `c7af18b`, the last step), each built with
`cmake -S $WORK/<name> -B $WORK/<name>/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHALE_BUILD_BENCH=ON
-DSHALE_BUILD_TESTS=OFF -DCMAKE_PREFIX_PATH=$(brew --prefix leveldb)` and `cmake --build $WORK/<name>/build --target
shale_bench`, with the same compiler and flags as in 2026-10 (the flags are those of the `release` preset). The harness
(`tools/`) is the same in all four, apart from `--cache_size`, whose default is the 8 MiB of the main table.
LevelDB is the binary of the 0.1.2 build run with `--engine=leveldb`. Every condition of the main table is that of
2026-10 (see "How the engines were matched" below): 8 MiB block cache, checksums verified on every read, no compression.

**Workloads.** As in 2026-10: 1,000,000 keys, 100-byte values, `fillsync` 10,000 puts, three runs of each
benchmark, each figure the median of the three runs, every read checked against a record of what was written.

**Commands.** `$DB` is an empty directory on the internal SSD, made empty before every run. Every run went through
`measure.sh` (in the evidence file), which waited for the quiet check, ran the command under `caffeinate -ims` and wrote
the record. The protocol that was committed before the first run is
[`benchmarks/2026-11-m2-protocol.md`](benchmarks/2026-11-m2-protocol.md).

```
caffeinate -ims $WORK/<build>/build/tools/shale_bench --engine=shale \
  --benchmarks=fillseq,fillrandom,fillsync,overwrite,readrandom,readmissing,readseq,seekrandom \
  --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-<build>.json
```

for `<build>` in `v011`, `e`, `ef`, `final` and, with `--engine=leveldb` and `final`'s binary, `leveldb`; then the same
five with `--compact_before_reads`, writing `...-<build>-compacted.json`. Then three rounds, each round running the five
builds in one order (`v011 e ef final leveldb`, then `leveldb final ef e v011`, then `ef v011 leveldb e final`), the first
pass of every build and then the compacted pass of every build, with
`--benchmarks=overwrite,readrandom,readmissing,readseq,seekrandom --num=1000000 --repeat=3`, writing
`2026-11-m2-reads/<build>-<first|compacted>-<round>.json`; three rounds of
`--benchmarks=fillseq,fillrandom,overwrite` in the same three orders, writing `2026-11-m2-writes/<build>-<round>.json`; and
three rounds of the diagnostic (0.1.2 with `--cache_size=1073741824`, and LevelDB, in both passes, with the reads' benchmark
list), writing `2026-11-m2-diagnostic/`. In all, 67 runs of `shale_bench` and 16 runs of the probe (and 4 more probe runs made afterwards, on E and E+F, which the
protocol does not list: they are named as such in the evidence file).

**Quiet check and loads.** Before every run: a 1-minute load average under 2.0, none of the processes the protocol
names running, AC power. The check waited 3,952 s in all over the session's 83 runs, the longest wait
829 s (the machine was in use). The loads, starts and exit statuses of every run are in section 0 of
[`benchmarks/2026-11-m2-evidence.txt`](benchmarks/2026-11-m2-evidence.txt). Every run exited with status 0, and none was
run again. The session took from 18:50 to 21:14 on 2026-11-22.

### 2026-11: results, the commands as given

The reads follow `overwrite` directly, and the compaction that `overwrite` left is still to be done while they run.
One run of each build (the median of its three repeats, with the lowest and highest repeat). The figure to quote for a
read row is in the rounds, below; the write rows have rounds too.

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| fillseq | 353,592 (351,506 to 361,211) | 364,009 (357,450 to 368,952) | 372,806 (356,444 to 377,477) | 1.03 | 0.98 |
| fillrandom | 250,686 (203,065 to 256,023) | 255,153 (236,936 to 260,382) | 251,158 (213,446 to 257,578) | 1.02 | 1.02 |
| fillsync | 339 (339 to 341) | 348 (347 to 348) | 336 (336 to 337) | 1.03 | 1.03 |
| overwrite | 227,493 (225,379 to 229,440) | 212,776 (211,357 to 214,231) | 262,602 (261,650 to 264,170) | 0.94 | 0.81 |
| readrandom | 335,910 (322,018 to 336,597) | 311,709 (310,336 to 312,012) | 449,074 (393,755 to 453,608) | 0.93 | 0.69 |
| readmissing | 1,561,970 (1,557,656 to 1,567,684) | 1,237,600 (1,235,168 to 1,238,079) | 1,081,730 (1,081,608 to 1,090,322) | 0.79 | 1.14 |
| readseq | 4,308,290 (4,271,166 to 4,313,903) | 5,642,695 (5,586,235 to 5,644,453) | 12,757,807 (12,741,302 to 12,764,111) | 1.31 | 0.44 |
| seekrandom | 170,310 (170,249 to 170,769) | 99,196 (98,762 to 99,346) | 386,148 (385,988 to 386,291) | 0.58 | 0.26 |

The same, with `--compact_before_reads`: the database is compacted once, between `overwrite` and `readrandom`, which is not
timed.

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| fillseq | 365,033 (364,026 to 366,166) | 354,400 (354,062 to 354,430) | 371,536 (365,014 to 376,294) | 0.97 | 0.95 |
| fillrandom | 257,077 (250,349 to 258,904) | 251,713 (245,578 to 259,095) | 264,359 (257,184 to 264,676) | 0.98 | 0.95 |
| fillsync | 347 (347 to 347) | 346 (346 to 347) | 338 (337 to 338) | 1.00 | 1.02 |
| overwrite | 233,306 (228,958 to 235,646) | 213,097 (211,641 to 214,790) | 218,716 (218,708 to 223,255) | 0.91 | 0.97 |
| readrandom | 385,798 (375,496 to 387,503) | 426,068 (420,045 to 426,503) | 537,590 (535,234 to 539,030) | 1.10 | 0.79 |
| readmissing | 4,232,505 (4,175,567 to 4,259,019) | 4,939,866 (4,938,384 to 4,944,253) | 1,598,519 (1,577,105 to 1,617,481) | 1.17 | 3.09 |
| readseq | 6,063,439 (5,974,389 to 6,084,389) | 11,987,150 (11,839,866 to 12,001,236) | 13,557,959 (13,551,352 to 13,570,408) | 1.98 | 0.88 |
| seekrandom | 297,692 (295,511 to 299,952) | 383,716 (375,822 to 384,326) | 450,217 (449,974 to 450,501) | 1.29 | 0.85 |

`fillsync` is a single run of each build, so its ratios are single-run ratios and are not a finding. The write rows of
these single runs are not the figure to use either: the rounds below are.

**The two levels of 2026-10.** In 2026-10 the first-pass read rows of one build came out at one of two levels from run to
run. The protocol fixed a criterion before the runs: the repeats of every run of a row within 2%, and the medians of the
four first-pass runs of a build (the run above and the three rounds) falling into two groups, every run of the higher one
more than 10% above every run of the lower. By it no build shows two levels here: the largest gap between neighbouring
runs of any build and read row is 7.3% (LevelDB's `readmissing`), and the next largest is
3.5%. The differences in the first pass in this session are between builds, not between runs of one build.

### 2026-11: the read rows in interleaved rounds

Three rounds, each of the five builds, in the three orders of the protocol. The figure is the median of the three rounds'
medians, with the lowest and highest round in brackets, and is the figure to quote. The rules are those of "What is
reported" at the end of the protocol: a change **moved** a row only if all three of its round medians lie on one side of all
three of the build before it and the medians differ by at least 3%; shale is **level with** LevelDB only if the rounds
interleave and the ratio is within 0.95 to 1.05.

The commands as given (first pass):

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| readrandom | 339,730 (336,779 to 343,593) | 319,693 (318,462 to 323,789) | 458,438 (454,874 to 458,762) | 0.94 | 0.70 |
| readmissing | 1,465,520 (1,459,442 to 1,509,805) | 1,219,835 (1,213,390 to 1,221,941) | 1,053,136 (981,823 to 1,078,242) | 0.83 | 1.16 |
| readseq | 4,322,745 (4,287,624 to 4,330,466) | 5,696,584 (5,680,023 to 5,698,458) | 12,792,345 (12,698,473 to 12,805,115) | 1.32 | 0.45 |
| seekrandom | 173,310 (172,280 to 174,115) | 101,870 (101,592 to 103,560) | 387,072 (387,021 to 388,032) | 0.59 | 0.26 |

| benchmark | 0.1.2 against v0.1.1 | 0.1.2 against LevelDB |
| --- | --- | --- |
| readrandom | moved (slower by 6%) | 0.70; the rounds do not interleave |
| readmissing | moved (slower by 17%) | 1.16; the rounds do not interleave |
| readseq | moved (faster by 32%) | 0.45; the rounds do not interleave |
| seekrandom | moved (slower by 41%) | 0.26; the rounds do not interleave |

Compacted before the reads:

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| readrandom | 405,792 (401,169 to 405,854) | 411,782 (407,310 to 415,907) | 537,160 (536,586 to 538,149) | 1.01 | 0.77 |
| readmissing | 4,387,081 (4,291,835 to 4,400,609) | 4,942,452 (4,930,658 to 4,957,097) | 1,609,096 (1,596,734 to 1,614,120) | 1.13 | 3.07 |
| readseq | 6,128,582 (5,874,496 to 6,133,715) | 11,887,444 (11,858,135 to 11,917,034) | 13,587,072 (13,537,051 to 13,596,524) | 1.94 | 0.87 |
| seekrandom | 307,251 (304,001 to 308,187) | 378,730 (375,938 to 381,391) | 452,616 (451,579 to 453,742) | 1.23 | 0.84 |

| benchmark | 0.1.2 against v0.1.1 | 0.1.2 against LevelDB |
| --- | --- | --- |
| readrandom | not distinguishable from run-to-run variation (+1.5%) | 0.77; the rounds do not interleave |
| readmissing | moved (faster by 13%) | 3.07; the rounds do not interleave |
| readseq | moved (faster by 94%) | 0.87; the rounds do not interleave |
| seekrandom | moved (faster by 23%) | 0.84; the rounds do not interleave |

On `readrandom` after a compaction the three rounds of 0.1.2 and of v0.1.1 overlap
(407,310 to 415,907 against
401,169 to 405,854), and the
medians differ by 1.5%.

### 2026-11: the write rows in interleaved rounds

The same three rounds for `fillseq`, `fillrandom` and `overwrite`, which write to an empty database and have no second pass.

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| fillseq | 359,621 (354,797 to 359,731) | 316,189 (312,693 to 353,037) | 343,462 (337,586 to 371,720) | 0.88 | 0.92 |
| fillrandom | 256,391 (252,986 to 256,876) | 209,105 (207,623 to 252,703) | 220,282 (219,567 to 262,722) | 0.82 | 0.95 |
| overwrite | 232,553 (232,291 to 235,339) | 168,346 (165,897 to 210,399) | 223,255 (223,109 to 260,116) | 0.72 | 0.75 |

| benchmark | 0.1.2 against v0.1.1 | 0.1.2 against LevelDB |
| --- | --- | --- |
| fillseq | moved (slower by 12%) | 0.921; the rounds interleave, and the ratio is outside the 0.95 to 1.05 band |
| fillrandom | moved (slower by 18%) | 0.949; the rounds interleave, and the ratio is outside the 0.95 to 1.05 band |
| overwrite | moved (slower by 28%) | 0.75; the rounds do not interleave |

Three of the five builds each have one round of the write rows that is higher than the other two, and it is round 3 for
all three: E+F, 0.1.2 and LevelDB (`overwrite`: 0.1.2 165,897, 168,346, 210,399 and LevelDB
223,109, 223,255, 260,116 in rounds 1, 2 and 3; v0.1.1
232,291, 235,339, 232,553). E has a low round too (round 1 on `fillrandom` and `overwrite`, round 2 on
`fillseq`), and v0.1.1's three rounds agree within 1.5% on every write row. Why the high
rounds are high was not examined. The medians leave the high round out, and the brackets show it. The verdict "moved
(slower)" on `fillrandom` rests on a margin of 0.1%: the highest round of 0.1.2 (252,703) is
below the lowest of v0.1.1 (252,986) by 284 ops/s,
and the rule as fixed counts that.

### 2026-11: where each change moved the figures

The medians of the rounds for v0.1.1, then after each of the three steps, with the rule of the protocol applied to each
step against the build before it.

The commands as given (first pass):

| benchmark | v0.1.1 | E | E+F | E+F+G (0.1.2) | v0.1.1 to E | E to E+F | E+F to E+F+G |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| readrandom | 339,730 | 302,854 | 292,738 | 319,693 | 0.89, moved (slower by 11%) | 0.97, not distinguishable from run-to-run variation | 1.09, moved (faster by 9%) |
| readmissing | 1,465,520 | 1,157,273 | 1,156,291 | 1,219,835 | 0.79, moved (slower by 21%) | 1.00, not distinguishable from run-to-run variation | 1.05, moved (faster by 5%) |
| readseq | 4,322,745 | 4,982,190 | 5,287,529 | 5,696,584 | 1.15, moved (faster by 15%) | 1.06, moved (faster by 6%) | 1.08, moved (faster by 8%) |
| seekrandom | 173,310 | 94,325 | 92,921 | 101,870 | 0.54, moved (slower by 46%) | 0.99, not distinguishable from run-to-run variation | 1.10, moved (faster by 10%) |

Compacted before the reads:

| benchmark | v0.1.1 | E | E+F | E+F+G (0.1.2) | v0.1.1 to E | E to E+F | E+F to E+F+G |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| readrandom | 405,792 | 380,471 | 374,031 | 411,782 | 0.94, moved (slower by 6%) | 0.98, not distinguishable from run-to-run variation | 1.10, moved (faster by 10%) |
| readmissing | 4,387,081 | 4,384,820 | 4,383,361 | 4,942,452 | 1.00, not distinguishable from run-to-run variation | 1.00, not distinguishable from run-to-run variation | 1.13, moved (faster by 13%) |
| readseq | 6,128,582 | 9,330,926 | 10,570,755 | 11,887,444 | 1.52, moved (faster by 52%) | 1.13, moved (faster by 13%) | 1.12, moved (faster by 12%) |
| seekrandom | 307,251 | 336,563 | 333,406 | 378,730 | 1.10, moved (faster by 10%) | 0.99, not distinguishable from run-to-run variation | 1.14, moved (faster by 14%) |

Writes, in rounds:

| benchmark | v0.1.1 | E | E+F | E+F+G (0.1.2) | v0.1.1 to E | E to E+F | E+F to E+F+G |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| fillseq | 359,621 | 354,547 | 313,198 | 316,189 | 0.99, not distinguishable from run-to-run variation | 0.88, not distinguishable from run-to-run variation | 1.01, not distinguishable from run-to-run variation |
| fillrandom | 256,391 | 253,616 | 213,131 | 209,105 | 0.99, not distinguishable from run-to-run variation | 0.84, not distinguishable from run-to-run variation | 0.98, not distinguishable from run-to-run variation |
| overwrite | 232,553 | 209,639 | 168,486 | 168,346 | 0.90, moved (slower by 10%) | 0.80, not distinguishable from run-to-run variation | 1.00, not distinguishable from run-to-run variation |

**What the system calls and the counters say.** One run of the probe of 2026-10 (it calls `shale_bench`'s own workload
functions, and counts the `pread` calls of each benchmark with a library loaded into it) on each build, after
`fillrandom` and `overwrite` of 1,000,000 keys. `pread` calls, with the bytes read:

| state | benchmark | v0.1.1 `pread` calls (bytes) | E | E+F | 0.1.2 |
| --- | --- | ---: | ---: | ---: | ---: |
| first pass | readrandom | 947,761 (3,891 MB) | 954,943 (3,922 MB) | 954,937 (3,922 MB) | 954,947 (3,922 MB) |
| first pass | readmissing | 9,012 (37 MB) | 25,390 (105 MB) | 25,385 (105 MB) | 25,380 (105 MB) |
| first pass | readseq | 31,285 (128 MB) | 33,904 (139 MB) | 932 (142 MB) | 932 (142 MB) |
| first pass | seekrandom | 1,415,887 (5,814 MB) | 3,139,624 (12,940 MB) | 3,139,619 (12,940 MB) | 3,139,474 (12,940 MB) |
| compacted | readrandom | 926,878 (3,802 MB) | 926,814 (3,802 MB) | 926,814 (3,802 MB) | 926,814 (3,802 MB) |
| compacted | readmissing | 5,458 (22 MB) | 5,428 (22 MB) | 5,428 (22 MB) | 5,428 (22 MB) |
| compacted | readseq | 27,716 (114 MB) | 27,711 (114 MB) | 765 (116 MB) | 765 (116 MB) |
| compacted | seekrandom | 953,091 (3,910 MB) | 953,027 (3,909 MB) | 953,027 (3,909 MB) | 953,027 (3,909 MB) |

And what the database itself counted during `overwrite` (the same probe runs):

| | v0.1.1 | E | E+F | 0.1.2 |
| --- | ---: | ---: | ---: | ---: |
| compactions | 38 | 50 | 50 | 50 |
| compaction bytes read | 536,381,130 | 693,788,325 | 693,788,325 | 693,788,325 |
| compaction bytes written | 458,447,945 | 605,590,520 | 605,590,520 | 605,590,520 |
| write slowdowns | 91 | 0 | 0 | 0 |
| write amplification of `overwrite` | 6.13 | 7.39 | 7.39 | 7.39 |
| files per level at the end of `overwrite` (levels 0 to 3) | 5/7/52/4 | 3/5/50/8 | 3/5/50/8 | 3/5/50/8 |

- **E.** The first step is the one that made the first pass slower on three rows. The tree that the first pass reads
  is different with E: the database made 50 compactions against 38, read and wrote 29% to 32% more bytes in them, never
  slowed a write down (91 slowdowns in v0.1.1), and ended with files in different levels (3/5/50/8 against 5/7/52/4 in
  levels 0 to 3). E, E+F and E+F+G left the same counters to the byte, run after run, and v0.1.1's differ from run to run
  in the bytes. A `seekrandom` in that tree made 3,139,474 `pread` calls against
  1,415,887 in v0.1.1 (the same count for E, E+F and E+F+G to within 150 calls), and read
  2.2 times the bytes; the first-pass `readmissing` made
  25,380 against 9,012. That the rows are slower in the first pass,
  and that the tree and the calls differ with E, are measured; why E makes the compactions run differently was not examined.
  After a compaction the trees are the same (the calls of `seekrandom` differ by 64 in 953,000), and E is not slower on
  `seekrandom` there (1.10) or `readmissing`; it is slower on `readrandom`
  after a compaction (0.94) for a reason that was not examined.
- **F.** Readahead cut the `pread` calls of `readseq` from 33,904 (E) to 932 in the
  first pass and from 27,711 to 765 after a compaction, while reading 2% more
  bytes. `readseq` moved (1.06 and 1.13);
  the point reads and the seeks did not, which is what the design says (a seek and a `Get()` read one block). In the
  probe's profile, `pread` is 11.7% of the calling thread's samples in
  `readseq` in v0.1.1 and 5.5% in 0.1.2 (first pass),
  14.5% and 8.6%
  compacted.
- **G.** Fewer allocations moved `readrandom`, `readmissing` and `seekrandom` after a compaction by
  1.10, 1.13 and
  1.14, and `readseq` by
  1.12, against E+F. The allocation counts per operation are in the next
  section. In the probe's profile, "malloc, free, memset" is
  19.6% of the calling thread's samples in `seekrandom` in v0.1.1 and
  9.2% in 0.1.2 after a compaction
  (16.9% and 7.6%
  in the first pass), and 10.8% and
  5.7% in `readrandom`.
- **Writes.** The write rows do not use the table iterator, except that a compaction reads its inputs through it, and what
  they write is unchanged. 0.1.2's write rows are below v0.1.1's, and the counters above are different from E on. By the
  rule, only `overwrite` moved at a single step (E, 0.90); the other drops are
  between steps and are not distinguishable from run-to-run variation one step at a time. A causal link between the
  counters and the write rows was not examined.

The profile shares of the calling thread, in the probe's runs of 2026-11 (one run each; `sample` for 4 seconds of a loop of
the benchmark, the thread that calls the engine):

| state | benchmark | build | samples | engine and harness code | `pread` | crc32c | malloc, free, memset | memmove, memcmp |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| first pass | readrandom | v0.1.1 | 2,791 | 39.5% | 30.7% | 16.0% | 6.9% | 5.9% |
| first pass | readrandom | 0.1.2 | 2,807 | 42.8% | 29.6% | 14.1% | 5.7% | 7.2% |
| first pass | readseq | v0.1.1 | 2,946 | 69.0% | 11.7% | 6.4% | 3.3% | 9.6% |
| first pass | readseq | 0.1.2 | 2,935 | 62.2% | 5.5% | 10.6% | 2.6% | 19.0% |
| first pass | seekrandom | v0.1.1 | 2,802 | 42.3% | 21.9% | 10.8% | 16.9% | 7.0% |
| first pass | seekrandom | 0.1.2 | 2,834 | 40.1% | 27.8% | 15.1% | 7.6% | 8.2% |
| compacted | readrandom | v0.1.1 | 2,757 | 32.0% | 33.6% | 17.9% | 10.8% | 4.9% |
| compacted | readrandom | 0.1.2 | 2,780 | 34.0% | 34.6% | 19.2% | 5.7% | 6.4% |
| compacted | readseq | v0.1.1 | 2,925 | 65.7% | 14.5% | 8.0% | 4.4% | 7.3% |
| compacted | readseq | 0.1.2 | 2,390 | 50.8% | 8.6% | 16.6% | 5.1% | 18.5% |
| compacted | seekrandom | v0.1.1 | 2,795 | 34.2% | 25.9% | 13.7% | 19.6% | 5.2% |
| compacted | seekrandom | 0.1.2 | 2,772 | 31.1% | 34.7% | 17.7% | 9.2% | 5.8% |

### 2026-11: diagnostic, the database in the block cache

**This changes a condition of the comparison and is not a main result.** The database is 116 MB of keys and values; with
`shale_bench --cache_size=1073741824` shale's block cache holds all of it, while LevelDB's cache stays as the adapter
configures it (8 MiB), so the two engines are not matched here. The JSON files are in `2026-11-m2-diagnostic/`, and the tool marks each as
diagnostic. Three rounds of 0.1.2 and LevelDB (order: 0.1.2 first in rounds 1 and 3, LevelDB first in round 2), the
benchmark list of the read rounds, median of the three rounds' medians with the lowest and highest in brackets.

| benchmark | state | 0.1.2, 1 GiB cache | LevelDB, as configured (8 MiB) | 0.1.2 (1 GiB) / LevelDB | 0.1.2, 8 MiB (read rounds) | 1 GiB over 8 MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| readrandom (diagnostic) | first pass | 586,719 (577,855 to 587,894) | 457,671 (453,352 to 458,170) | 1.28 | 319,693 | 1.84 |
| seekrandom (diagnostic) | first pass | 199,963 (195,884 to 200,696) | 389,279 (386,132 to 389,343) | 0.51 | 101,870 | 1.96 |
| readrandom (diagnostic) | compacted | 909,523 (897,615 to 915,175) | 539,273 (538,776 to 540,024) | 1.69 | 411,782 | 2.21 |
| seekrandom (diagnostic) | compacted | 769,580 (749,378 to 771,498) | 448,689 (446,808 to 452,601) | 1.72 | 378,730 | 2.03 |

What a large cache does to shale in this run: it roughly doubles the throughput of `readrandom` and `seekrandom` (the last column), and with it 0.1.2 is above LevelDB as configured
on `readrandom` in both states and on `seekrandom` after a compaction, and below it on `seekrandom` in the first pass.
No probe was taken with the large cache, so what the calls and the time of these rows are is not examined. The comparison
says what the cache is worth and not that the engines are matched.

### 2026-11: allocations per operation

`read_allocations_test` counts the `operator new` calls of a thread during one operation, through the public API, with
100-byte values and 16-byte keys (the benchmark's), and with 7-byte keys. libc++ with Apple clang (the standard library of
this session's builds), counted on v0.1.1, on E+F (`2f95cb6`) and on E+F+G (`c7af18b`) from clean exports with the same
test file, in the change that made them (#24):

| per call, libc++, 16-byte keys | v0.1.1 | E+F | E+F+G (0.1.2) |
| --- | ---: | ---: | ---: |
| `Get`, block cached | 6 | 8 | 1 |
| `Get`, block read from the file | 8 | 10 | 2 |
| a data block read | 2 | 2 | 1 |
| `Get`, block read and cached | 11 | 13 | 5 |
| a seek, blocks cached | 39 | 21 | 9 |
| a seek, blocks read | 43 | 25 | 11 |
| 10 `Next` inside a block | 0 | 0 | 0 |

With 7-byte keys (which fit the string's inline buffer) v0.1.1 is 3, 5, 2, 8, 27 and 31, E+F is 3, 5, 2, 8, 13 and 17, and
E+F+G is 1, 2, 1, 5, 8 and 10. With libstdc++ the counts of E+F+G with 16-byte keys are 1, 2, 1, 5, 9 and 11 (Clang 17 and 18);
v0.1.1 and E+F were not counted with libstdc++. These counts do not depend on the speed of the machine. They are counts of
calls and say nothing yet about time: G moved the post-compaction point reads and seeks by 10% to 14% (above), where `Get`
with its block read from the file went from 10 allocations to 2.

### 2026-11: where shale is still slower, and why

Rows where 0.1.2 is below LevelDB in the rounds (ratio of the medians; the rounds of the two engines do not
interleave unless said):

- **readseq** (0.45 first pass, 0.87 compacted). The calling thread
  of the probe spends 50.8% of its samples in the engine's and
  the harness's own code, 18.5% in `memmove` and `memcmp`,
  16.6% in checksums and 8.6%
  in `pread` (0.1.2, compacted); the first pass has 5.5% in `pread`. LevelDB
  reads through memory-mapped table files and made no `pread` in 2026-10 (not counted again). What is left of the gap was
  not examined.
- **seekrandom** (0.26, 0.84). After a compaction a seek
  makes one `pread` (953,027 calls for 1,000,000 seeks) and `pread` is
  34.7% of the thread, checksums
  17.7%. In the first pass the seeks made
  3,139,474 calls, which is the E effect above.
- **readrandom** (0.70, 0.77). `pread` is
  29.6% and 34.6% of
  the thread in the probe (first pass, compacted), checksums 14.1% and
  19.2%. LevelDB made no `pread` in 2026-10 (not counted again). With the whole database in the block
  cache (the diagnostic above) shale is 1.28 and 1.69 of LevelDB.
- **Writes.** `overwrite` 0.75, `fillseq` 0.92 and `fillrandom`
  0.95 of LevelDB in the rounds (the rounds of `fillseq` and `fillrandom` interleave with
  LevelDB's, those of `overwrite` do not). What differs from v0.1.1 is under "Where each change moved the figures". The
  profile of the writing thread was not taken again in 2026-11, so no share is cited for the write rows.

### 2026-11: what did not improve

- **The first pass of `readrandom`, `readmissing` and `seekrandom` got slower.** 0.94,
  0.83 and 0.59 of v0.1.1, in all three rounds each. The first
  step is where it happened; the calls and counters above show a different tree and more bytes read for each seek. Why was
  not examined.
- **The write rows got slower.** `fillseq` 0.88, `fillrandom` 0.82, `overwrite`
  0.72 of v0.1.1, by the rule; one step at a time only `overwrite` at the first step moved (the table above).
- **`readrandom` after a compaction is not distinguishable from v0.1.1** (1.01): the first step
  made it slower (0.94) and the last gave that back
  (1.10).
- **Readahead (F) did not move any point read or seek**, in either state; it was not meant to.
- **`fillsync` was not put through rounds.** One run of each build: 339, 345 (E), 345 (E+F) and 348 ops/s; a single run
  is not evidence of a movement.
- **LevelDB is not caught on any row it leads**, and no row is level with it under the rule.

### 2026-11: what these numbers do not say

- They are one machine on one day, after an operating system update. They do not carry over to the figures of 2026-10
  below, which were measured on macOS 26.6.2 with another libc++.
- Three rounds is few. For two builds that do not differ, the three rounds of one lie entirely on one side of the three of
  the other 1 time in 10, which is why a change needs a difference of 3% as well to be called moved.
- The machine was not idle during the session: the quiet check waited for the load to fall each time it was over 2.0,
  and a browser and the window server ran throughout.
- The database fits in the page cache; a read that goes to the disk is a different benchmark. The diagnostic rows put the
  database in shale's block cache too, and match nothing.
- The explanation of each row rests on one probe run per build and state (the syscalls and counters) and one profile of
  4 seconds per row. The probe is of the same workloads, not of the rows' own runs. Where no probe was taken the text says
  "not examined".
- What made the first step change the compactions of `overwrite` was not found, and so the slower first-pass reads and
  the slower write rows are reported as measured, with the counters that go with them, and not explained.
- LevelDB is the Homebrew bottle, whose optimization level is not known, and whose tables are read through a mapping.
- Every read in every run was checked against a record of what was written, and none was wrong.

### 2026-11: reproducing

The protocol, [`benchmarks/2026-11-m2-protocol.md`](benchmarks/2026-11-m2-protocol.md), has the builds, the quiet check, the
order of the runs, the commands and the scripts (`measure.sh`, `table.py`). The evidence file has the sources of the probes and
of the script that applies the reporting rules. The JSON files are in `benchmarks/2026-11-m2-*.json`,
`benchmarks/2026-11-m2-reads/`, `benchmarks/2026-11-m2-writes/` and `benchmarks/2026-11-m2-diagnostic/`.

## 2026-10: the first measurement (history)

This is the measurement of 2026-10-18 as it was written, on macOS 26.6.2, with its headings one level down. Its figures are
not comparable with those above, which were made on macOS 27.2.1.

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
- **The changes.** Four inefficiencies that the first measurement found were fixed. A synced write still
  survives a power loss with every write acknowledged before it, and a power loss still leaves a prefix of
  the writes. Since a memtable switch no longer syncs, recovery now also sets aside a log that begins after
  a gap that a power loss left, whatever `paranoid_checks` says
  ([durability](design/0004-durability.md), D6). Inlining `Status::ok()` and the key comparison made `readseq`
  9% faster in the compacted pass, and the first-pass reads 7% to 59% faster in runs that are less steady;
  reading blocks into buffers that are not zero-filled, 2% on the point reads and seeks; writing the MANIFEST
  without the database mutex held, nothing measurable on its own; taking the syncs of a memtable switch off
  the writer's path, 4% to 13% on the write rows in the interleaved rounds.
  Each is below, with its figures.

### What was measured

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

### How the engines were matched

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

### Results: the commands as given

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

### Results: compacted before the reads

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

### The write rows, in interleaved rounds

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

### What profiling found, and what changed

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

#### A. `Status::ok()` and the key comparison, inline

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

#### B. Block buffers that are not zero-filled

**Found.** Every miss of the block cache reads a block into a buffer, which `ReadBlock()` made by resizing a
`std::string`, so it was filled with zeros first and then overwritten by the read: about 5% of the thread in
`readrandom` and `seekrandom` after compaction.

**Changed.** A block's bytes are in a buffer made with `std::make_unique_for_overwrite`, which the read fills
directly. Nothing reads a byte the file did not supply: a short read is an error before the checksum is looked at.

**Measured.** Against A: `readrandom` 312,137 to 320,286 (+2.6%) and 367,466 to 375,442 (+2.2%); `seekrandom` 158,461 to
161,597 (+2.0%) and 281,285 to 287,421 (+2.2%). The other rows moved by 1% or less, either way.

#### C. The MANIFEST written and synced without the database mutex

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

#### D. The syncs of a memtable switch off the writer's path

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

### Where shale is still slower, and why

The figures after a row name are shale's ops/s over LevelDB's in the first pass and compacted, in this session, and
for the scans also with the final build's run of the first pass that read at the higher level
(`2026-10-m2-repeats/final-1.json`). "The
thread" is the one that calls the engine, in a profile made with `sample` of a separate run of the same workload on the
final build (section 7 of the evidence file). Its shares are shares of that thread's samples. They show where the time
went, and not what a change would save.

#### overwrite (1.01, 0.86; 0.84 in the interleaved rounds)

The background thread cannot keep up. In the profile it was busy for 95% of the window, against 87% for LevelDB's,
and the writer slept for 5.5% of its samples, slowed for a millisecond each time level 0 reached 8 files (303 times in
the probe's counters), where LevelDB's writer did not sleep. Two things make shale's compaction the larger job: it wrote
507 MiB of tables for the overwrites against LevelDB's 429 MiB (109 MiB flushed and 398 MiB compacted, against 107 MiB and
322 MiB, by each engine's statistics), and it made 779 `F_FULLFSYNC` calls in the load and the overwrites against
LevelDB's 624, since it syncs the directory after every table. `F_FULLFSYNC` was 53% of shale's background thread.
The first pass's 1.01 is a single run; the interleaved rounds and the second pass agree on 0.84 to 0.86.

#### readrandom (0.70, 0.74)

Nearly every get reads a block with `pread`: 961,868 calls for 1,000,000 gets in the first state, since 8 MiB of block
cache holds a small part of a 116 MB database. LevelDB makes none, and reads out of its mapping. `pread` is 30% of the
thread in the first state and 36% compacted, which is as large as the gap in the first state (30% of shale's time per
get) and larger than it compacted (26%). Checksums are 13% and 17% of shale's thread, in hardware, and allocation 11%
and 12%; in the earlier profile LevelDB spent 38% and 47% of its thread on checksums, in software.

#### readseq (0.28, 0.46; 0.33 at the higher level of the first pass)

31,928 `pread` calls for 1,000,000 entries (LevelDB none), 11% and 15% of the thread. The rest is the iterator layers:
64% to 69% of the thread is engine code, most of it a merging iterator over a chain of iterators that each answer
`Valid()`, `key()` and `Next()` through a virtual call. For each table shale has four layers that LevelDB's two-level
iterator does not: two that check that what the table returns is in order and within the block its index names
(`OrderedIterator`, `BoundedBlockIterator`), one that names the file in an error (`NamedTableIterator`), and one that
hands the index key to the block (`KeyedIndexIterator`). Those checks are a choice of shale's, which reports a table that
is out of order as an error rather than return what it holds, and their cost here is not separated from the calls.

#### seekrandom (0.28, 0.68; 0.43 at the higher level of the first pass)

A seek reads 1.5 blocks on average with `pread` in the probe's first state (1,476,058 calls for 1,000,000 seeks), 24%
and 28% of the thread; allocation and freeing are 20% and 22% (a seek builds a merging iterator and its children each
time; LevelDB's were 12% and 18% in the earlier profile), checksums 11% and 12%. In the first state a seek is positioned in three levels
that hold data, and in one after compaction (section 7.4 of the evidence file), which is why the first pass is the
slower.

### Where shale is not slower

- **fillsync** (1.02, 1.03). 345 ops/s against 337. Both are bound by the flush that every put waits for, one
  `F_FULLFSYNC` each: the p50 of about 3.0 ms is the same on both.
- **readmissing** (1.21, 2.49). On both engines a key that is not there is answered by the Bloom filter, and a block is
  read only when the filter says maybe. Why LevelDB's lookup costs more than shale's in the compacted state was not
  examined.
- **fillseq and fillrandom** are level (0.99 and 0.98 in the interleaved rounds); the single runs put shale 8% and 18%
  above LevelDB in the first pass and 9% and 3% below in the second.

### What these numbers do not say

- They are one machine on one day. The ratios that the passes and the rounds agree on are firmer than any one figure.
- The machine was not idle: the 1-minute load average was under 2.0 at the start of every run and the 5-minute
  average was not. A browser renderer and the window server used part of a core all the time.
- The disk was 94% full, and the write rows moved by up to 16% between runs of the same build.
- The database fits in the page cache. A read that goes to the disk is a different benchmark.
- LevelDB is the Homebrew bottle, whose optimization level is not known, and whose tables are read through a mapping.
- The profiles are of separate runs, on the same machine, which was not held quiet. They say where the time goes in a
  run and not what a change would save.
- Every read in every run was checked against a record of what was written, and none was wrong.

### Reproducing

Build `shale_bench` as above, with LevelDB installed (`brew install leveldb`). The JSON of each run has the machine
(`uname -srm`, the processor, the cores, the clock), the build (compiler, version, build type) and every run's figures,
and no host name. `shale_bench --help` says what each figure is.
