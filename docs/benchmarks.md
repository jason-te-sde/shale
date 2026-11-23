# Benchmarks

Two measurements of shale next to LevelDB on one machine, through the same workloads and the same code (`shale_bench`).
The second, of 2026-11, is of the read path of 0.1.2 against v0.1.1 and LevelDB; the first, of 2026-10, is kept below as
it was written. The two were made on different versions of the operating system, so their figures are not comparable.

## 2026-11: the read path (0.1.2)

A second measurement, made after three changes to how reads work (below), with the builds before and after run in the same
session as LevelDB. Every figure in this section is in a JSON file in [`benchmarks/`](benchmarks/), which `shale_bench` wrote.
The counts and shares in the explanations, and the record of every run, are in
[`benchmarks/2026-11-m2-evidence.txt`](benchmarks/2026-11-m2-evidence.txt).

### 2026-11: in short

Measured on 2026-11-23, in one session, with the five builds run one after the other in rounds (v0.1.1, the three
steps that made reads cheaper, and LevelDB), so every comparison below is between builds made and run the same
day on the same machine. In this document "0.1.2" is the build of the last of the three steps; "E", "E+F" and "E+F+G" are
the three steps. A ratio is the first figure over the second: in the columns 0.1.2 / v0.1.1 and 0.1.2 / LevelDB, above 1
means that 0.1.2 is faster. Each figure is a median, with the lowest and the highest run, or round, in brackets. Every
shale build read the same files, and LevelDB read a database made the same way.

- **Reads, against v0.1.1.** 0.1.2 is faster on every read row, in both passes, by the rule of the protocol (all
  three rounds of 0.1.2 above all three of v0.1.1's, and the medians at least 3% apart): `readseq` 1.97 (1.98 after a
  compaction), `seekrandom` 1.24 (1.25), `readmissing` 1.11 (1.16) and `readrandom` 1.04 (1.04).
- **Compaction, against v0.1.1.** Compaction reads its input through the table iterator of the first step. A compaction
  of the whole key range of the same files, which made the same two compactions and read and wrote the same bytes in every
  build, took 2.59 s in v0.1.1 and 1.51 s in 0.1.2 (1.53 s in E). After `overwrite`, 0.1.2 has made all 50 of its
  compactions when the last write returns; v0.1.1 has made 37 or 38 and needs 0.7 to 0.9 s more for the rest.
- **Against LevelDB.** shale is faster on `readmissing` (2.06 in the first pass, 3.20 after a compaction), and level with
  LevelDB on `fillseq` and `fillrandom` in the write rounds (0.97 and 0.98 when the last write returns, 0.97 and 1.02
  once the background is idle). LevelDB is faster on `readseq` (0.68 in the first pass, 0.89 after a compaction),
  `seekrandom` (0.52, 0.85), `readrandom` (0.74, 0.79) and `overwrite` (0.81 when the last write returns, 0.83 settled).
  On none of the read rows do the rounds of the two engines interleave.
- **Writes, against v0.1.1.** No write row moved, either figure: the rounds overlap. The write rows of every engine came
  out at one of two levels during the session (section "The write rows in interleaved rounds").
- **What the steps did.** Readahead cut the `pread` calls of `readseq` from 31,786 to 875 in the first pass and from
  27,711 to 765 after a compaction. A cached `Get` allocates 1 time in 0.1.2 against 6 in v0.1.1, and a seek whose
  blocks are cached 9 times against 39 (libc++, 16-byte keys).
- **Diagnostic, not a main result.** With a 1 GiB block cache for shale (and LevelDB as configured, 8 MiB), 0.1.2 reads
  `readrandom` at 1.57 of LevelDB's rate in the first pass and 1.71 after a compaction; `seekrandom` 1.02 and 1.69. This
  changes a condition of the comparison.
- **A first session of 2026-11-22 is superseded.** It timed the writes while v0.1.1 still owed compaction work and gave
  each build's reads a different tree. It is described in "The first session, superseded" and is not a result.

### 2026-11: what changed

Three changes to how a table is read, each merged to `main` as one commit and measured as one step.

**E, one table iterator (`194417f`).** A read through a table used a two-level iterator over a separate
iterator for each data block, wrapped in a named iterator to say which file was damaged. A table is now read through
one iterator (`src/io/table_iterator.cc`) that walks the index and the data blocks itself, makes every check the old
path made (the block checksum whenever `verify_checksums` is set, the type byte always, keys strictly increasing, no
key past the last key its index entry names, a seek never landing before its target), and names the file in every
error. The checks were pinned by tests before the change, and none was loosened. Compaction reads every input table
through this iterator too (`src/db/compaction_job.cc`).

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

None of the three changes the public API, the disk format or a check. `shale_bench` gained `--cache_size` (`10e9eb7`) for
the diagnostic rows, and, for this measurement (`ae1e1d4`), `--existing_db` and the wait for the background described below;
these change the tool and not the library.

### 2026-11: what was measured

**Machine.** Apple M2, 8 cores (4 performance, 4 efficiency), 16 GiB, macOS 27.2.1 (build 26B131), `uname -srm` is
`Darwin 27.2.0 arm64`. The databases were on the internal SSD, an APFS volume that was 75% full. On AC power. The
machine was not idle: the desktop applications that were open ran during the session, and the quiet check below waited
for the load to fall each time it was above its limit.

**What differs from the machine of 2026-10.** The operating system (macOS 26.6.2, build 25G83, then; `Darwin
25.6.0`), the system C++ library that `shale_bench` loads (libc++ 2200.27.0, then 2000.63.0) and libSystem (1359, then
1356). The compiler (Apple clang 21.0.0, clang-2100.3.34.2), cmake (4.4.3), LevelDB (`leveldb 1.23_2`) and snappy
(`snappy 1.3.1`) are the same. The disk was 75% full, then 94%. **The figures of 2026-11 are therefore not comparable
with those of 2026-10 below.** The comparison that this section reports is the one made inside the 2026-11 session:
v0.1.1 against the steps and LevelDB, built and run on the same day, in interleaved rounds.

**Builds.** Every engine version was built with the same benchmark tool, the `tools/` of `ae1e1d4` (the commit that
merged the protocol's revision), over the engine (everything else) of its own commit: v0.1.1 (`9fefedf`), E (`194417f`),
E+F (`2f95cb6`) and 0.1.2 (`c7af18b`, whose `src`, `include` and build files are those of `ae1e1d4`). Each is a clean
export, built with `cmake -S $WORK/<name> -B $WORK/<name>/build -G Ninja -DCMAKE_BUILD_TYPE=Release
-DSHALE_BUILD_BENCH=ON -DSHALE_BUILD_TESTS=OFF -DCMAKE_PREFIX_PATH=$(brew --prefix leveldb)` and `cmake --build
$WORK/<name>/build --target shale_bench`, the flags of the `release` preset, as in 2026-10. LevelDB is the binary of the
0.1.2 build run with `--engine=leveldb`. Every condition of the main table is that of 2026-10 (see "How the engines were
matched" below): 8 MiB block cache, checksums verified on every read, no compression.

**Workloads.** As in 2026-10: 1,000,000 keys, 100-byte values, `fillsync` 10,000 puts, three runs of each
benchmark, each figure the median of the three runs, every read checked against a record of what was written.

**Writes are timed twice.** After every run of a write benchmark, `shale_bench` waits until the engine's background is
idle: no flush or compaction queued or running, and no level at a compaction score of 1 or more (for LevelDB, the same
rule from its public interfaces). It looks every 10 ms, for at most 300 s. A write row gives its ops/s when the last write
returns, as before and as LevelDB's `db_bench` does, and its ops/s counting the wait ("settled"), with the engine's counters
at both moments. It also waits before every benchmark, so that no benchmark runs while the work of another is being done.

**The first-pass databases.** Every first-pass read of every shale build reads the same files: a database written once by
0.1.2's binary (`fillrandom,overwrite` at 1,000,000 keys, waited for), then opened once, which wrote what its log held into a
table and compacted it (8 compactions), and waited for again. Its tree is 0/4/49/9 (files in levels 0 to 3; the levels
below are empty). Before every read run, the run's database is made an APFS clone of it (`cp -c -R`), and every file of the
clone is read once, so that its blocks are in the operating system's cache as a database that was just written has them.
LevelDB reads its own database, made the same way; it left 0/7/35/67. The compacted pass is the same command with
`--compact_before_reads`, on its own clone. At the end of the session every file of both databases had the checksum it
was recorded with.

**Commands.** Every run went through `measure.sh`, which made the database, waited for the quiet check, ran the command
under `caffeinate -ims` and wrote the record. The protocol that was committed before the first run is
[`benchmarks/2026-11-m2-protocol.md`](benchmarks/2026-11-m2-protocol.md); section 9 is the revision that this session
followed. `<reads>` is `readrandom,readmissing,readseq,seekrandom`; for LevelDB the first argument is `--engine=leveldb`.

```
# The writes, on an empty database:
caffeinate -ims $WORK/<build>/build/tools/shale_bench --engine=shale \
  --benchmarks=fillseq,fillrandom,fillsync,overwrite \
  --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-<build>-writes.json
# The reads, on a clone of the first-pass database:
caffeinate -ims $WORK/<build>/build/tools/shale_bench --engine=shale --existing_db=overwrite \
  --benchmarks=<reads> --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-<build>.json
# The same, compacted before the reads:
caffeinate -ims $WORK/<build>/build/tools/shale_bench --engine=shale --existing_db=overwrite \
  --benchmarks=<reads> --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-<build>-compacted.json \
  --compact_before_reads
```

for `<build>` in `v011`, `e`, `ef`, `final` and, with `final`'s binary, `leveldb`. Then three rounds of the reads, each
round running the five builds in one order (`v011 e ef final leveldb`, then `leveldb final ef e v011`, then `ef v011
leveldb e final`), the first pass of every build and then the compacted pass of every build, writing
`2026-11-m2-reads/<build>-<first|compacted>-<round>.json`; three rounds of
`--benchmarks=fillseq,fillrandom,overwrite` on an empty database in the same three orders, writing
`2026-11-m2-writes/<build>-<round>.json`; three rounds of the diagnostic (0.1.2 with `--cache_size=1073741824`, and
LevelDB, in both passes, with the reads' benchmark list), writing `2026-11-m2-diagnostic/`; and the probes. The two
first-pass databases were made first, and their JSON files are in `2026-11-m2-masters/`. In all, 76 runs of
`shale_bench` and 16 runs of the probe.

**Quiet check and loads.** Before every run: a 1-minute load average under 2.0, none of the processes the protocol
names running, AC power. The load at the start of a run was 1.55 to 1.99, and the check waited 1,619 s in all over the
session's 92 runs, the longest wait 242 s. The loads, starts and exit statuses of every run are in section 0 of
[`benchmarks/2026-11-m2-evidence.txt`](benchmarks/2026-11-m2-evidence.txt). Every run exited with status 0, none was
run again, and none wrote to its standard error. The session took from 04:20 to 05:29 on 2026-11-23.

### 2026-11: results, the commands as given

One run of each build (the median of its three repeats, with the lowest and highest repeat). The figure to quote for a
read row or a write row is in the rounds, below.

Writes, on an empty database, when the last write returned and settled:

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

`fillsync` is a single run of each build, so its ratios are single-run ratios and are not a finding. The write rows of
these single runs are not the figure to use either: in this session the write rows of every engine came out at one of two
levels, and the single runs above fell at different levels (v0.1.1's `fillseq` and `fillrandom` at the higher, 0.1.2's at
the lower; see the write rounds below).

**The trees.** Every first-pass read row of every shale build, in these runs and in the rounds, began on the tree of the
first-pass database, 0/4/49/9, and left it as it was; every compacted row began on 0/0/0/55 and left it. LevelDB's tree
changes during its first read: in every first pass its `readrandom` began on 0/7/35/67 and left 0/0/0/65, 0/0/0/66 or
0/0/0/67. LevelDB compacts a file that reads seek through too often, during the reads that do it, by design (the
protocol's section 9.4 notes it); its `readmissing`, `readseq` and `seekrandom` read the tree its `readrandom` left. Its
compacted rows read 0/0/0/55. The first repetition of LevelDB's first-pass `readrandom`, during which its tree changes,
is the lowest of the three in each of its four first-pass runs (437,612 to 468,450 against 522,504 to 549,673 for the
other two).

**The two levels of 2026-10.** In 2026-10 the first-pass read rows of one build came out at one of two levels from run to
run. The protocol fixed a criterion before the runs: the repeats of every run of a row within 2%, and the medians of the
four first-pass runs of a build (the run above and the three rounds) falling into two groups, every run of the higher one
more than 10% above every run of the lower. By it no build shows two levels on a read row here: the largest gap between
neighbouring runs of any build and read row is 4.6% (LevelDB's `readmissing`).

### 2026-11: the read rows in interleaved rounds

Three rounds, each of the five builds, in the three orders of the protocol. The figure is the median of the three rounds'
medians, with the lowest and highest round in brackets, and is the figure to quote. The rules are those of "What is
reported" in the protocol: a change **moved** a row only if all three of its round medians lie on one side of all
three of the build before it and the medians differ by at least 3%; shale is **level with** LevelDB only if the rounds
interleave and the ratio is within 0.95 to 1.05.

First pass:

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| readrandom | 382,992 (380,552 to 383,450) | 396,613 (395,401 to 403,688) | 534,442 (522,504 to 547,834) | 1.04 | 0.74 |
| readmissing | 2,926,379 (2,919,035 to 2,941,092) | 3,250,969 (3,248,129 to 3,258,688) | 1,575,842 (1,573,047 to 1,582,442) | 1.11 | 2.06 |
| readseq | 4,647,963 (4,647,315 to 4,668,204) | 9,173,810 (8,996,831 to 9,217,036) | 13,571,276 (13,557,055 to 13,574,968) | 1.97 | 0.68 |
| seekrandom | 189,823 (188,783 to 191,651) | 235,317 (233,828 to 239,032) | 450,171 (449,685 to 453,221) | 1.24 | 0.52 |

| benchmark | 0.1.2 against v0.1.1 | 0.1.2 against LevelDB |
| --- | --- | --- |
| readrandom | moved (faster by 3.6%) | 0.74; the rounds do not interleave (all three of 0.1.2's below LevelDB's) |
| readmissing | moved (faster by 11.1%) | 2.06; the rounds do not interleave (all three of 0.1.2's above LevelDB's) |
| readseq | moved (faster by 97.4%) | 0.68; the rounds do not interleave (all three of 0.1.2's below LevelDB's) |
| seekrandom | moved (faster by 24.0%) | 0.52; the rounds do not interleave (all three of 0.1.2's below LevelDB's) |

Compacted before the reads:

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| readrandom | 407,256 (406,118 to 413,343) | 423,632 (413,987 to 425,260) | 537,358 (537,279 to 540,423) | 1.04 | 0.79 |
| readmissing | 4,414,066 (4,413,326 to 4,541,999) | 5,100,292 (4,960,617 to 5,125,091) | 1,596,117 (1,569,261 to 1,626,000) | 1.16 | 3.20 |
| readseq | 6,116,191 (6,077,071 to 6,138,162) | 12,096,543 (12,083,442 to 12,157,581) | 13,576,089 (13,573,985 to 13,580,790) | 1.98 | 0.89 |
| seekrandom | 307,174 (306,945 to 317,919) | 383,746 (372,749 to 388,040) | 451,574 (450,732 to 454,165) | 1.25 | 0.85 |

| benchmark | 0.1.2 against v0.1.1 | 0.1.2 against LevelDB |
| --- | --- | --- |
| readrandom | moved (faster by 4.0%) | 0.79; the rounds do not interleave (all three of 0.1.2's below LevelDB's) |
| readmissing | moved (faster by 15.5%) | 3.20; the rounds do not interleave (all three of 0.1.2's above LevelDB's) |
| readseq | moved (faster by 97.8%) | 0.89; the rounds do not interleave (all three of 0.1.2's below LevelDB's) |
| seekrandom | moved (faster by 24.9%) | 0.85; the rounds do not interleave (all three of 0.1.2's below LevelDB's) |

On `readrandom` the margin is small in both passes: in the first pass the lowest round of 0.1.2 (395,401) is above the
highest of v0.1.1 (383,450) and the medians differ by 3.6%; after a compaction the lowest of 0.1.2 (413,987) is above
the highest of v0.1.1 (413,343) by 644 ops/s, and the medians differ by 4.0%. The rule as fixed counts both.

### 2026-11: the write rows in interleaved rounds

The same three rounds for `fillseq`, `fillrandom` and `overwrite`, which write to an empty database and have no second
pass. Each row is given twice: its ops/s when the last write returned, and its ops/s counting the wait for the background
("settled"). The rules are applied to each on its own.

| benchmark | v0.1.1 ops/s | 0.1.2 ops/s | LevelDB ops/s | 0.1.2 / v0.1.1 | 0.1.2 / LevelDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| fillseq | 354,822 (313,406 to 356,954) | 356,580 (312,403 to 359,072) | 368,327 (335,306 to 375,897) | 1.00 | 0.97 |
| fillseq, settled | 353,971 (312,797 to 356,043) | 355,718 (311,742 to 358,249) | 367,499 (334,578 to 375,137) | 1.00 | 0.97 |
| fillrandom | 215,313 (214,456 to 255,315) | 254,335 (216,133 to 255,022) | 260,094 (222,644 to 263,093) | 1.18 | 0.98 |
| fillrandom, settled | 214,989 (214,114 to 254,923) | 253,919 (215,815 to 254,587) | 248,153 (212,728 to 251,000) | 1.18 | 1.02 |
| overwrite | 193,321 (193,101 to 232,344) | 210,310 (170,588 to 211,349) | 260,332 (217,565 to 261,279) | 1.09 | 0.81 |
| overwrite, settled | 167,027 (165,769 to 199,659) | 210,008 (170,371 to 211,027) | 252,279 (211,169 to 253,045) | 1.26 | 0.83 |

| benchmark | 0.1.2 against v0.1.1 | 0.1.2 against LevelDB |
| --- | --- | --- |
| fillseq | not distinguishable from run-to-run variation (+0.5%) | 0.97; level with (the rounds interleave, and the ratio is within 0.95 to 1.05) |
| fillseq, settled | not distinguishable from run-to-run variation (+0.5%) | 0.97; level with (the rounds interleave, and the ratio is within 0.95 to 1.05) |
| fillrandom | not distinguishable from run-to-run variation (+18.1%) | 0.98; level with (the rounds interleave, and the ratio is within 0.95 to 1.05) |
| fillrandom, settled | not distinguishable from run-to-run variation (+18.1%) | 1.02; level with (the rounds interleave, and the ratio is within 0.95 to 1.05) |
| overwrite | not distinguishable from run-to-run variation (+8.8%) | 0.81; the rounds do not interleave (all three of 0.1.2's below LevelDB's) |
| overwrite, settled | not distinguishable from run-to-run variation (+25.7%) | 0.83; the rounds do not interleave (all three of 0.1.2's below LevelDB's) |

**Two levels.** Every engine's write rows came out at one of two levels, 10% to 25% apart, and the brackets above show
it. `overwrite`, when the last write returned, round by round (rounds 1, 2 and 3): v0.1.1 193,101, 232,344 and 193,321;
0.1.2 210,310, 211,349 and 170,588; LevelDB 261,279, 260,332 and 217,565. The repeats of one run mostly agree (within 3%
on `overwrite` in 16 of the 20 runs), but the level changed within runs as well as between them: between the benchmarks
of one run (v0.1.1's run of the commands as given has `fillseq` and `fillrandom` at the higher level and `overwrite` at
the lower), and between the repeats of one benchmark (E's `fillrandom` in round 1: 223,368, 244,825 and 257,095). The
runs at the lower level fell mostly in three stretches of the session, about 04:23 to 04:35, 05:02 to 05:04 and 05:16 to
05:21, for whichever engine was running then; the read rounds, which ran between the first two, show no such split
(section "The two levels of 2026-10" above). Why was not examined. The medians follow whichever level two of the three
rounds fell at, so the medians above compare different levels: v0.1.1's `fillrandom` and `overwrite` had two rounds at
the lower level, and 0.1.2's one. No rule of the protocol reads the levels apart; every run of each build, with its
repeats and the time it started, is in the evidence file (section 3).

`fillsync` was not put through rounds (one run of each build, above).

### 2026-11: E made compaction faster

Compaction reads every input table through `Table::NewIterator()` (`src/db/compaction_job.cc`), the iterator that E
rewrote. This session measured what that does in two ways.

**The same compaction, timed.** With `--compact_before_reads`, `shale_bench` compacts the whole key range of its clone
of the first-pass database (`DB::CompactRange()`) before the reads, and records how long it took. Every shale build
compacted the same files, made the same 2 compactions, and read and wrote the same bytes (198,570,807 and 181,529,295).
Over the four compacted runs of each build (the run of the commands as given and the three rounds):

| | v0.1.1 | E | E+F | E+F+G (0.1.2) |
| --- | ---: | ---: | ---: | ---: |
| compaction of the whole range, s | 2.593 (2.554 to 2.671) | 1.530 (1.515 to 1.564) | 1.574 (1.539 to 1.587) | 1.509 (1.473 to 1.528) |

v0.1.1 took 1.69 times as long as E. The readahead of F cut the `pread` calls of that compaction from 47,907 (v0.1.1) to
1,750 (0.1.2) for the same 200 MB (the probe, below); the time with E+F is not below E's. (LevelDB compacted its own
database, of other files, in 1.712 to 1.782 s; that is not a comparison of the same work.)

**After `overwrite`.** In every one of the 12 runs of `overwrite` of each shale build (the three repeats of the run of the
commands as given and of each of the three write rounds):

| | v0.1.1 | E, E+F and 0.1.2 |
| --- | --- | --- |
| compactions when the last write returned | 37 (38 in one run) | 50 |
| files per level then (levels 0 to 3) | 5/8/52/4 (6/6/52/4 in that run) | 3/5/50/8 |
| write slowdowns | 50 to 129 | 0 |
| write amplification then | 5.99 to 6.14 | 7.39 |
| wait for the background to be idle | 0.705 to 0.900 s | 0.007 to 0.009 s |
| compactions once idle | 50 | 50 |
| files per level once idle | 0/4/49/8 | 3/5/50/8 |
| write amplification once idle | 7.02 to 7.10 | 7.39 |

So v0.1.1 still owes 12 or 13 compactions when its last write returns, and once it has done them it has made as many as
E (50); its settled write amplification is 7.02 to 7.10 against E's 7.39. E's wait, 0.007 to 0.009 s, is of the order of
the wait after `fillseq` (0.005 to 0.007 s), which makes no compaction, and E's counters and tree did not change during
it: nothing was left to do. The `overwrite` row when the last write returns leaves out the 0.7 to 0.9 s of v0.1.1's
background work, and none of E's; the settled row counts it. In the rounds, neither `overwrite` row moved (above).

**Per compaction.** This session does not time compactions one by one. The investigation that led to the protocol's
revision did, outside the protocol and under a load of 1.8 to 3.0: with a scheduler that runs every background task as soon
as it is queued, so that both builds make the same 87 compactions in the same order and write byte-identical databases,
each compaction took 1.01 to 1.49 times as long in v0.1.1 as in E, with a median of 1.2. Slowing E's table reads by 4
microseconds each reproduced v0.1.1's counters byte for byte, and giving v0.1.1's compaction more time reproduced E's (the
evidence file, section 6).

### 2026-11: where each change moved the figures

The medians of the rounds for v0.1.1, then after each of the three steps, with the rule of the protocol applied to each
step against the build before it.

First pass:

| benchmark | v0.1.1 | E | E+F | E+F+G (0.1.2) | v0.1.1 to E | E to E+F | E+F to E+F+G |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| readrandom | 382,992 | 361,116 | 357,388 | 396,613 | 0.94, moved (slower by 5.7%) | 0.99, not distinguishable from run-to-run variation (-1.0%) | 1.11, moved (faster by 11.0%) |
| readmissing | 2,926,379 | 2,933,599 | 2,924,562 | 3,250,969 | 1.00, not distinguishable from run-to-run variation (+0.2%) | 1.00, not distinguishable from run-to-run variation (-0.3%) | 1.11, moved (faster by 11.2%) |
| readseq | 4,647,963 | 7,507,512 | 8,310,085 | 9,173,810 | 1.62, moved (faster by 61.5%) | 1.11, moved (faster by 10.7%) | 1.10, moved (faster by 10.4%) |
| seekrandom | 189,823 | 211,825 | 210,254 | 235,317 | 1.12, moved (faster by 11.6%) | 0.99, not distinguishable from run-to-run variation (-0.7%) | 1.12, moved (faster by 11.9%) |

Compacted before the reads:

| benchmark | v0.1.1 | E | E+F | E+F+G (0.1.2) | v0.1.1 to E | E to E+F | E+F to E+F+G |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| readrandom | 407,256 | 381,189 | 378,929 | 423,632 | 0.94, moved (slower by 6.4%) | 0.99, not distinguishable from run-to-run variation (-0.6%) | 1.12, moved (faster by 11.8%) |
| readmissing | 4,414,066 | 4,392,495 | 4,381,372 | 5,100,292 | 1.00, not distinguishable from run-to-run variation (-0.5%) | 1.00, not distinguishable from run-to-run variation (-0.3%) | 1.16, moved (faster by 16.4%) |
| readseq | 6,116,191 | 9,413,105 | 10,720,053 | 12,096,543 | 1.54, moved (faster by 53.9%) | 1.14, moved (faster by 13.9%) | 1.13, moved (faster by 12.8%) |
| seekrandom | 307,174 | 338,314 | 338,254 | 383,746 | 1.10, moved (faster by 10.1%) | 1.00, not distinguishable from run-to-run variation (+0.0%) | 1.13, moved (faster by 13.4%) |

Writes, in rounds, when the last write returned and settled:

| benchmark | v0.1.1 | E | E+F | E+F+G (0.1.2) | v0.1.1 to E | E to E+F | E+F to E+F+G |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| fillseq | 354,822 | 315,401 | 357,347 | 356,580 | 0.89, not distinguishable from run-to-run variation (-11.1%) | 1.13, not distinguishable from run-to-run variation (+13.3%) | 1.00, not distinguishable from run-to-run variation (-0.2%) |
| fillrandom | 215,313 | 244,825 | 256,178 | 254,335 | 1.14, not distinguishable from run-to-run variation (+13.7%) | 1.05, not distinguishable from run-to-run variation (+4.6%) | 0.99, not distinguishable from run-to-run variation (-0.7%) |
| overwrite | 193,321 | 210,596 | 210,256 | 210,310 | 1.09, not distinguishable from run-to-run variation (+8.9%) | 1.00, not distinguishable from run-to-run variation (-0.2%) | 1.00, not distinguishable from run-to-run variation (+0.0%) |
| fillseq, settled | 353,971 | 314,790 | 356,487 | 355,718 | 0.89, not distinguishable from run-to-run variation (-11.1%) | 1.13, not distinguishable from run-to-run variation (+13.2%) | 1.00, not distinguishable from run-to-run variation (-0.2%) |
| fillrandom, settled | 214,989 | 244,387 | 255,708 | 253,919 | 1.14, not distinguishable from run-to-run variation (+13.7%) | 1.05, not distinguishable from run-to-run variation (+4.6%) | 0.99, not distinguishable from run-to-run variation (-0.7%) |
| overwrite, settled | 167,027 | 210,269 | 209,930 | 210,008 | 1.26, not distinguishable from run-to-run variation (+25.9%) | 1.00, not distinguishable from run-to-run variation (-0.2%) | 1.00, not distinguishable from run-to-run variation (+0.0%) |

**What the system calls say.** One run of the counting probe on v0.1.1 and on 0.1.2, in each state, on clones of the same
first-pass database (it calls `shale_bench`'s own engine adapter and workload functions, and counts the `pread` calls of
each benchmark with a library loaded into it). `pread` calls, with the bytes read:

| state | benchmark | v0.1.1 `pread` calls (bytes) | 0.1.2 `pread` calls (bytes) |
| --- | --- | ---: | ---: |
| first pass | readrandom | 943,114 (3,872 MB) | 943,114 (3,872 MB) |
| first pass | readmissing | 9,708 (40 MB) | 9,708 (40 MB) |
| first pass | readseq | 31,786 (130 MB) | 875 (133 MB) |
| first pass | seekrandom | 1,477,001 (6,065 MB) | 1,477,001 (6,065 MB) |
| compacted | the compaction before the reads | 47,907 (201 MB) | 1,750 (205 MB) |
| compacted | readrandom | 926,744 (3,801 MB) | 926,744 (3,801 MB) |
| compacted | readmissing | 5,436 (22 MB) | 5,436 (22 MB) |
| compacted | readseq | 27,711 (114 MB) | 765 (116 MB) |
| compacted | seekrandom | 953,057 (3,909 MB) | 953,057 (3,909 MB) |

On the same files, the two builds make the same number of reads of the files for the point reads and the seeks, to the
call; what changed on those rows is the work done around each read.

- **E.** One table iterator moved `readseq` by 61.5% (first pass) and 53.9% (compacted) and `seekrandom` by 11.6% and
  10.1%. In v0.1.1's `readseq`, functions of the named and bounded iterators that wrapped each table and each block, which
  E removed, are at the top of the stack in 18.9% and 18.6% of the calling thread's samples (first pass, compacted); they do
  not exist in 0.1.2. E made `readrandom` slower, by 5.7% and 6.4%, for a reason that was not examined (the probes were
  taken of v0.1.1 and 0.1.2 only), and G more than gave it back.
- **F.** Readahead cut the `pread` calls of `readseq` from 31,786 (v0.1.1) to 875 (0.1.2) in the first pass and from
  27,711 to 765 after a compaction, while reading 2% more bytes. It moved `readseq` (10.7% and 13.9%) and nothing else,
  which is what the design says (a seek and a `Get()` read one block). In the probe's profile, `pread` is 13.1% of the
  calling thread's samples in `readseq` in v0.1.1 and 6.9% in 0.1.2 (first pass), 14.6% and 10.2% compacted.
- **G.** Fewer allocations moved every read row in both passes, by 10.4% to 16.4% against E+F. The allocation counts per
  operation are in the next section but one. In the probe's profile, "malloc, free, memset" is 20.1% of the calling thread's
  samples in `seekrandom` in v0.1.1 and 9.2% in 0.1.2 (first pass), 19.8% and 9.4% compacted, and 9.2% and 5.8%, 10.4% and
  5.1% in `readrandom`.
- **Writes.** No step moved a write row, either figure, by the rule; the two levels above keep the rounds of every pair of
  builds overlapping. What E did to compaction is in the section before this one.

The profile shares of the calling thread, in the probe's runs of 2026-11-23 (one run each; `sample` for 4 seconds of a
loop of the benchmark, the thread that calls the engine):

| state | benchmark | build | samples | engine and harness code | `pread` | crc32c | malloc, free, memset | memmove, memcmp |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| first pass | readrandom | v0.1.1 | 2,740 | 31.4% | 34.7% | 18.4% | 9.2% | 4.7% |
| first pass | readrandom | 0.1.2 | 2,754 | 35.1% | 34.5% | 17.0% | 5.8% | 6.3% |
| first pass | readseq | v0.1.1 | 2,917 | 66.8% | 13.1% | 6.6% | 3.5% | 9.8% |
| first pass | readseq | 0.1.2 | 2,903 | 55.5% | 6.9% | 13.6% | 4.2% | 19.8% |
| first pass | seekrandom | v0.1.1 | 2,748 | 35.0% | 24.0% | 12.6% | 20.1% | 5.8% |
| first pass | seekrandom | 0.1.2 | 2,795 | 32.7% | 31.3% | 17.5% | 9.2% | 6.7% |
| compacted | readrandom | v0.1.1 | 2,730 | 31.4% | 33.7% | 17.2% | 10.4% | 5.5% |
| compacted | readrandom | 0.1.2 | 2,788 | 33.7% | 33.8% | 19.5% | 5.1% | 6.7% |
| compacted | readseq | v0.1.1 | 2,931 | 66.3% | 14.6% | 7.4% | 3.9% | 7.3% |
| compacted | readseq | 0.1.2 | 2,331 | 51.4% | 10.2% | 15.5% | 4.5% | 17.8% |
| compacted | seekrandom | v0.1.1 | 2,730 | 34.5% | 24.5% | 12.7% | 19.8% | 5.5% |
| compacted | seekrandom | 0.1.2 | 2,766 | 31.5% | 32.3% | 16.3% | 9.4% | 6.3% |

### 2026-11: diagnostic, the database in the block cache

**This changes a condition of the comparison and is not a main result.** The database is 116 MB of keys and values; with
`shale_bench --cache_size=1073741824` shale's block cache holds all of it, while LevelDB's cache stays as the adapter
configures it (8 MiB), so the two engines are not matched here. The JSON files are in `2026-11-m2-diagnostic/`, and the
tool marks each as diagnostic. Three rounds of 0.1.2 and LevelDB (order: 0.1.2 first in rounds 1 and 3, LevelDB first in
round 2), the benchmark list of the read rounds on clones of the first-pass databases, median of the three rounds' medians
with the lowest and highest in brackets.

| benchmark | state | 0.1.2, 1 GiB cache | LevelDB, as configured (8 MiB) | 0.1.2 (1 GiB) / LevelDB | 0.1.2, 8 MiB (read rounds) | 1 GiB over 8 MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| readrandom (diagnostic) | first pass | 844,631 (835,994 to 845,734) | 536,382 (535,642 to 549,314) | 1.57 | 396,613 | 2.13 |
| seekrandom (diagnostic) | first pass | 457,786 (457,751 to 463,563) | 449,322 (447,455 to 450,696) | 1.02 | 235,317 | 1.95 |
| readrandom (diagnostic) | compacted | 920,516 (914,040 to 926,365) | 538,088 (535,505 to 539,096) | 1.71 | 423,632 | 2.17 |
| seekrandom (diagnostic) | compacted | 761,538 (755,078 to 772,535) | 451,019 (449,184 to 452,039) | 1.69 | 383,746 | 1.98 |

What a large cache does to shale in this run: it roughly doubles the throughput of `readrandom` and `seekrandom` (the last
column), and with it all three rounds of 0.1.2 are above all three of LevelDB as configured on every row (on first-pass
`seekrandom` by 1.9%). No probe was taken with the large cache, so what the calls and the time of these rows are is not
examined. The comparison says what the cache is worth and not that the engines are matched.

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
calls and say nothing on their own about time: G moved the read rows by 10.4% to 16.4% (above), where `Get` with its block
read from the file went from 10 allocations to 2.

### 2026-11: where shale is still slower, and why

Rows where 0.1.2 is below LevelDB in the rounds (ratio of the medians; the rounds of the two engines do not interleave):

- **readseq** (0.68 first pass, 0.89 compacted). The calling thread of the probe spends 51.4% of its samples in the
  engine's and the harness's own code, 17.8% in `memmove` and `memcmp`, 15.5% in checksums and 10.2% in `pread` (0.1.2,
  compacted); in the first pass 55.5%, 19.8%, 13.6% and 6.9%. LevelDB reads through memory-mapped table files and made no
  `pread` in 2026-10 (not counted again). In the first pass shale reads the three levels that hold files in 0/4/49/9,
  while LevelDB's `readseq` read the one level of the tree its `readrandom` had left. What is left of the gap was not examined.
- **seekrandom** (0.52, 0.85). After a compaction a seek makes about one `pread` (953,057 calls for 1,000,000 seeks) and
  `pread` is 32.3% of the thread, checksums 16.3%. In the first pass, on 0/4/49/9, the seeks made 1,477,001 calls, and
  `pread` is 31.3%, checksums 17.5%. LevelDB's first-pass `seekrandom` read the one level its `readrandom` had left.
- **readrandom** (0.74, 0.79). `pread` is 34.5% and 33.8% of the thread in the probe (first pass, compacted), checksums
  17.0% and 19.5%. LevelDB made no `pread` in 2026-10 (not counted again). With the whole database in the block cache (the
  diagnostic above) shale is 1.57 and 1.71 of LevelDB.
- **overwrite** (0.81 when the last write returned, 0.83 settled). The profile of the writing thread was not taken in
  2026-11, so no share is cited, and the gap was not examined.
- **fillsync** (one run of each: 346 ops/s against LevelDB's 385, a single-run ratio of 0.90). Not put through rounds.

### 2026-11: what did not improve

- **The write rows, against v0.1.1.** No write row moved, when the last write returned or settled: `fillseq` 1.00,
  `fillrandom` 1.18 and `overwrite` 1.09 (settled 1.00, 1.18 and 1.26), with the rounds overlapping on every row. The two
  levels of the session are in "The write rows in interleaved rounds".
- **`overwrite` when the last write returns.** In the runs at the same level, v0.1.1 is above 0.1.2: at the lower level
  193,101 and 193,321 (rounds 1 and 3) against 170,588 (round 3), and 193,397 against 162,654 in the commands as given; at
  the higher level 232,344 (round 2) against 210,310 and 211,349 (rounds 1 and 2). Settled, the same runs give 165,769,
  167,027 and 166,806 against 170,371 and 162,442 at the lower level, and 199,659 against 210,008 and 211,027 at the higher.
  No rule of the protocol reads the levels apart, so this is a description and not a finding. The counters of each are in
  "E made compaction faster".
- **The first step made `readrandom` slower**, by 5.7% (first pass) and 6.4% (compacted), for a reason that was not
  examined; G gave it back, and 0.1.2 is above v0.1.1 on the row by 3.6% and 4.0%.
- **`readmissing` did not move at E or at F**, in either state; G moved it.
- **Readahead (F) did not move any point read or seek**, in either state, and it was not meant to. It cut the `pread` calls
  of the compaction before the reads (47,907 in v0.1.1, 1,750 in 0.1.2), and that compaction took 1.574 s with E+F against
  1.530 s with E.
- **`fillsync` was not put through rounds.** One run of each build: 346 (v0.1.1), 344 (E), 346 (E+F) and 346 ops/s
  (0.1.2); a single run is not evidence of a movement.
- **LevelDB is not caught on any read row it leads, nor on `overwrite`,** and no read row is level with it under the rule.

### 2026-11: the first session, superseded

A first measured session ran from 18:50 to 21:14 on 2026-11-22, under the protocol as it was first committed, and is not a
result. It timed every write until the last write returned and read each build's own database right after `overwrite`, and
it compiled each build's `shale_bench` from that build's own tree. It reported the first-pass `readrandom`, `readmissing`
and `seekrandom` of 0.1.2 at 0.94, 0.83 and 0.59 of v0.1.1, and the write rows at 0.88, 0.82 and 0.72. Those compared
different things: when `overwrite`'s last write returned, v0.1.1 still owed 12 or 13 compactions, which then ran inside the
first-pass `readrandom`, and the first pass of v0.1.1 read the tree 0/4/49/8 while E, E+F and 0.1.2 read 3/5/50/8, whose three
level-0 tables every seek has to position in. The investigation that found this, and proved it in both directions, is
summarized in the protocol's section 9.1 and given with its numbers in section 6 of the evidence file. The protocol was then
revised (settle after writes, the same files for every build, one bench tool) before this session. The first session's
files stay at commit `3270703` (pull request #27, which was not merged), and none of its figures is set beside these.

### 2026-11: what these numbers do not say

- They are one machine on one day, after an operating system update. They do not carry over to the figures of 2026-10
  below, which were measured on macOS 26.6.2 with another libc++, nor to those of the superseded first session.
- Three rounds is few. For two builds that do not differ, the three rounds of one lie entirely on one side of the three of
  the other 1 time in 10, which is why a change needs a difference of 3% as well to be called moved.
- The machine was not idle during the session: the quiet check waited for the load to fall each time it was 2.0 or more,
  and the write rows came out at one of two levels for a reason that was not examined.
- The first-pass tree is one tree: the one that 0.1.2's binary left at 1,000,000 keys, once opened and settled, with no
  level-0 table. Another number of keys, or another build writing it, can leave another tree, and the first-pass rows depend
  on the tree (the superseded session shows by how much). LevelDB's first pass reads its own tree, which its first
  `readrandom` compacts.
- The database fits in the page cache, and every clone was read once before its run; a read that goes to the disk is a
  different benchmark. The diagnostic rows put the database in shale's block cache too, and match nothing.
- The explanation of each row rests on one probe run per build and state (the system calls and counters) and one profile
  of 4 seconds per row, of v0.1.1 and 0.1.2 only. The probe is of the same workloads, on clones of the same database, not of
  the rows' own runs. Where no probe was taken the text says "not examined".
- LevelDB is the Homebrew bottle, whose optimization level is not known, and whose tables are read through a mapping.
- Every read in every run was checked against a record of what was written, and none was wrong.

### 2026-11: reproducing

The protocol, [`benchmarks/2026-11-m2-protocol.md`](benchmarks/2026-11-m2-protocol.md), has the builds, the quiet check,
the order of the runs, the commands and, in section 9.9, every script that the session ran (`measure.sh`, `driver.sh`,
`build.sh`, `table.py`, `check-table.sh` and the probe's). The evidence file has the record of every run, the scripts that
read the samples and apply the reporting rules, and what the first session and the investigation found. The JSON files
are in `benchmarks/2026-11-m2-*.json`, `benchmarks/2026-11-m2-masters/`, `benchmarks/2026-11-m2-reads/`,
`benchmarks/2026-11-m2-writes/` and `benchmarks/2026-11-m2-diagnostic/`.

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
