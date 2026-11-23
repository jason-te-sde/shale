# The protocol for measuring the read path, 2026-11

This document is committed before any measured run, and the measurement follows it step by step. It
says what is compared, how each build is made, how the machine is held, in what order the runs are
made, the exact commands, and what the results may claim. The reporting rules in section 6 are fixed
here so that no result can change them afterwards.

**Revised on 2026-11-22**, after the first measured session and before the second.
[Section 9](#9-revision-of-2026-11-22-settled-writes-and-first-pass-reads-on-the-same-files) says why:
the first session timed writes while a build's background still owed work, and its first-pass reads ran
on different trees. Section 9 replaces sections 1, 2, 4, 5 and 8 where it differs from them (in section
1, what the first pass reads and what `final` is); sections 1 to 8 are otherwise as they were when the
first session followed them.

It repeats the measurement of 2026-10 ([how the engines were matched](../benchmarks.md#how-the-engines-were-matched),
and the [evidence](2026-10-m2-evidence.txt)) on the builds that make reads cheaper in three steps. The
allocation counts that the explanations of the read path cite are those of the change that reduced them;
the documents that report this measurement cite them from there, and this protocol does not repeat them.

## 1. What is compared

- **Main table.** v0.1.1, v0.1.2 (the final build) and LevelDB, in both passes: the commands as given
  (first pass), and the same commands with the database compacted before the reads (compacted pass).
- **Read rows and write rows.** The same three, in three interleaved rounds each: the reads in section 4
  B, and `fillseq`, `fillrandom` and `overwrite` in section 4 W, as in section 7.2 of the 2026-10
  evidence. `fillsync` is a single run from A, and is reported as a single-run ratio and labelled so.
- **Ablation.** v0.1.1, E, E+F and E+F+G in the same session, as A to D were in 2026-10. The three
  steps were each squash-merged to `main` as one commit:

  | Name | Build | Commit | What changed |
  | --- | --- | --- | --- |
  | `v011` | v0.1.1 | `9fefedf` (tag `v0.1.1`) | the build before |
  | `e` | E | `194417f` | one table iterator |
  | `ef` | E+F | `2f95cb6` | readahead |
  | `final` | E+F+G, v0.1.2 | the commit that merges this protocol | fewer allocations (`c7af18b`) |

- **Diagnostic rows.** `readrandom` and `seekrandom` with a 1 GiB block cache for shale, against LevelDB
  as configured. These rows are marked diagnostic wherever they appear.

## 2. Builds

Four clean exports, each made with `mkdir -p "$WORK/<name>" && git archive <rev> | tar -x -C "$WORK/<name>"`,
where `$WORK` is a scratch directory outside the repository. `<rev>` is `9fefedf` for `v011`, `194417f`
for `e`, `2f95cb6` for `ef`, and for `final` the commit that merges this protocol. The fifth build,
`leveldb`, is not an export: it is `final`'s binary run with `--engine=leveldb`.

The library of `final` must be that of E+F+G (`c7af18b`), with the same build flags. Before any run:

```
git diff --quiet c7af18b <final> -- src include CMakeLists.txt CMakePresets.json
```

must succeed, or measurement stops. (`10e9eb7`, which added `shale_bench --cache_size`, changed `tools/`
and `tests/` only, and the protocol adds a document.)

Each export is built with

```
cmake -S "$WORK/<name>" -B "$WORK/<name>/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSHALE_BUILD_BENCH=ON -DSHALE_BUILD_TESTS=OFF -DCMAKE_PREFIX_PATH=$(brew --prefix leveldb)
cmake --build "$WORK/<name>/build" --target shale_bench
```

These are the same flags as 2026-10, where each build was made with `cmake --preset release` (the preset is
Ninja with `CMAKE_BUILD_TYPE=Release`) and the same options; the machine record of section 3 shows the
compiler and library versions of the day. The binary of a build is `$WORK/<name>/build/tools/shale_bench`.

## 3. The machine

**The record.** Before the first run, the record of section 0 of the 2026-10 evidence is taken again, in
its order and with the same commands: `date`, `sysctl` (the chip, `hw.ncpu`, `hw.physicalcpu`,
`hw.perflevel0.logicalcpu`, `hw.perflevel1.logicalcpu`, `hw.memsize`), `sw_vers`, `uname -srm`,
`pmset -g batt`, the disk (`diskutil info` and `df -h` of the volume that holds `$DB`), the compiler
(`c++ --version`), `cmake --version`, `brew list --versions leveldb snappy`, and `otool -L` of the
LevelDB library. Any difference from 2026-10 (the macOS version, the compiler, the library versions) is
stated next to the results.

**The quiet check, before every run.** All of these hold:

- the 1-minute load average is under 2.0;
- no process is running whose name is `ninja`, `clang`, `clang++`, any name beginning `clang-` (the versioned drivers, `clang-tidy`, `clang-format`),
  `cmake`, `ctest`, `ld`
  or `ld64`, or begins with `shale_` or `fuzz_`. The names are matched as whole names, not as text inside a
  name, so a language server called `clangd` does not hold a run (2026-10 recorded the pattern
  `ninja|clang|cmake|ctest|ld64|shale_|fuzz_`; the set of names above still covers what it was meant to
  catch, `clang-tidy` and `clang-format` included);
- the machine draws from AC power;
- the run is made under `caffeinate -ims`, and so is the waiting.

The check is made every 15 seconds, as in 2026-10, and waits up to 2 hours of wall time for these to hold.
Past that, the run is not made, `measure.sh` exits with status 3 and the controller is told. `measure.sh`
(appendix) does the check, the run and the record, for every run in this protocol, including the probes
of section 4 D: a probe run is also made after the quiet check, which 2026-10's probes were not.
`measure.sh` keeps each run's standard output and standard error under `$WORK/logs`, never in the
repository.

**Go.** No run starts before the controller's Go, which means the GPU evaluation that shares this machine
has finished. Builds are made before Go, at `-j4` at most; runs are not.

## 4. Order

The blocks run in the order A, B, W, C, then D.

`$DB` is an empty directory on the internal SSD, made empty again before every run. In the lists below,
"first pass" is the command as given and "compacted" is the same command with `--compact_before_reads`.

**The order of builds in a round**, for B, W and (with its own order of engines) C:

| Round | Order |
| --- | --- |
| 1 | `v011 e ef final leveldb` |
| 2 | `leveldb final ef e v011` |
| 3 | `ef v011 leveldb e final` |

This replaces the cyclic rotation of the brief, which kept `ef` before `final` and `final` before
`leveldb` in every round. Here every neighbouring pair of builds, and `v011` and `final`, run in both
orders across the three rounds (`ef` and `final` neighbour each other in round 1 and round 2 only in the
order ef, final in round 1 and final, ef in round 2; `final` and `leveldb` likewise).

Within a round, every build's first-pass run comes before any compacted run, each pass following the
round's order of builds: for example round 1 is `v011 e ef final leveldb` in the first pass and then
`v011 e ef final leveldb` compacted.

**A. The tables.** Full commands, first pass, in the order `v011, e, ef, final, leveldb`. Then the
compacted pass, in the same order. This gives the main table, the ablation, `fillsync`, and the first
pass for the levels of 2026-10.

**B. Read rounds.** Rounds r = 1, 2, 3, in the order above.

**W. Write rounds.** Rounds r = 1, 2, 3, in the order above, for all five builds. A write round has one
pass only (its benchmarks do not read), so it has no compacted half.

**C. Diagnostic rounds.** Rounds r = 1, 2, 3, each in both passes (all first-pass runs, then all compacted
runs). In each pass, `final` with `--cache_size=1073741824` and `leveldb` run in the order final, leveldb
in rounds 1 and 3 and leveldb, final in round 2.

**D. Probes.** For `v011` and `final`:

- **System calls** are counted with `interpose.c`, the library that counts `pread`, `read`, `write`,
  `mmap`, `munmap`, `fsync` and `fcntl(F_FULLFSYNC)` and that the probe snapshots around each benchmark.
- **Profiles** are made with `sample`, through `profile.sh`, `sample_parse.py` and `categories.py`, of
  `readrandom`, `readseq` and `seekrandom` in both states (first pass and compacted).
- **Every probe run, counting or profiling, goes through `profile.sh`**, which sets
  `DYLD_INSERT_LIBRARIES` on the probe's own command line. The variable is never set around
  `measure.sh`: `/usr/bin/env` and `caffeinate` are protected by SIP and drop `DYLD_*` variables, so a
  probe started under them would run without the counting library and still exit 0. `measure.sh` wraps
  `profile.sh` (`BENCH=profile.sh`), which is under `caffeinate` and the quiet check without that
  variable being lost, so no second check is needed inside `profile.sh`.
- **A probe run is a failure** unless both hold: the standard output that `measure.sh` kept
  (`$WORK/logs/<measure label>.stdout`) has the line `<profile label>: probe exit 0` that `profile.sh`
  prints, and the probe's own output file (`$SCRATCH/prof-<profile label>.out`, with `SCRATCH=$WORK/probe`)
  has lines beginning `interpose:`. `profile.sh` itself always exits 0, so these two checks, which are made
  after the run and written into the evidence, are what decide. A failed probe run is run once more and
  both are recorded, as in section 5.
- Both probes are built with the one command that 2026-10 recorded, `c++ -O2 -g -std=c++20`, against
  the Release libraries of the build probed (`v011` or `final`), and with the same arguments except
  for that build's directory. `interpose.dylib` is built once and used for both. The evidence records the
  exact compile and link commands, and every `profile.sh` invocation (its probe, label, number of keys,
  benchmarks, mark or delay, seconds, loops and compact argument); the invocations are the same for the
  two builds.
- Probe runs go through `measure.sh`, so each is made after the quiet check and recorded like the others.
- LevelDB's counts are not taken again: it made no `pread` in 2026-10, because it maps its table files.

## 5. Commands, verbatim

`<bin>` is `$WORK/<name>/build/tools/shale_bench`, and for `leveldb` it is `final`'s, with
`--engine=leveldb`. Every command is run from `measure.sh`, so the record is
`caffeinate -ims <bin> <arguments>`, with paths under `$WORK` and `$DB` written as such. The commands run
from the root of the repository checkout that will hold the JSON files, so the `--json` paths are
relative to it.

**A.** The command of 2026-10, with the build's name in the file:

```
shale_bench --engine=shale --benchmarks=fillseq,fillrandom,fillsync,overwrite,readrandom,readmissing,readseq,seekrandom --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-<build>.json
```

and the compacted pass adds `--compact_before_reads` and writes
`docs/benchmarks/2026-11-m2-<build>-compacted.json`. `<build>` is `v011`, `e`, `ef`, `final` or
`leveldb`; for `leveldb` the first argument is `--engine=leveldb`.

**B.**

```
shale_bench --engine=shale --benchmarks=overwrite,readrandom,readmissing,readseq,seekrandom --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-reads/<build>-<first|compacted>-<r>.json
```

with `--compact_before_reads` in the compacted runs. `overwrite` starts from an empty database, so the
reads see the state that the full command gives them.

**W.** The command of section 7.2 of the 2026-10 evidence, which is shale_bench with
`--benchmarks=fillseq,fillrandom,overwrite --num=1000000 --repeat=3 --db=$DB`:

```
shale_bench --engine=shale --benchmarks=fillseq,fillrandom,overwrite --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-writes/<build>-<round>.json
```

for `<build>` in `v011`, `e`, `ef`, `final` and `leveldb` (with `--engine=leveldb` for the last).

**C.** The benchmark list of B, with `--cache_size`, so that C runs the same sequence of benchmarks as B:

```
shale_bench --engine=shale --cache_size=1073741824 --benchmarks=overwrite,readrandom,readmissing,readseq,seekrandom --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-diagnostic/<final|leveldb>-<pass>-<r>.json
```

`<pass>` is `first` or `compacted`, with `--compact_before_reads` in the second. The `leveldb` runs
leave out `--cache_size` (it is a usage error with `--engine=leveldb`) and use `--engine=leveldb`:
LevelDB's cache is as configured, 8 MiB. The diagnostic rows that are reported are `readrandom` and
`seekrandom`; the other benchmarks are there for the state they leave.

**A failed run.** `measure.sh` records the exit status and keeps a non-empty standard error in
`$WORK/logs/<label>.stderr`, and names it in the record by that `$WORK`-relative path, with its last line
(paths under `$WORK` and `$DB` written as such). A run that fails is run again once. The rerun writes the
same `--json` path, replacing what the failed run left, and appends to the same standard error file; both
runs are in the evidence file. A second failure stops the measurement and the
controller is told.

## 6. What is reported

These rules are fixed before any run. They read ops/s: the latency percentiles move in steps of the
clock's tick and tie often, so no rule below reads them.

- **Main table.** Each run's medians, with the lowest and highest run in brackets.
- **Read rows and write rows.** The median of the three rounds' medians, with the lowest and highest round
  in brackets. This is the figure to quote. `fillsync` is a single run, and its ratio is labelled as a
  single-run ratio.
- **Interleave.** The rounds of two builds interleave on a row when neither build's three round medians
  all lie above the other's.
- **Moved.** A change **moved** a row only if both of these hold: all three of its round medians lie on
  one side of all three of the build before it, and the median of its round medians differs from the
  build before's by at least 3%. Otherwise the text says "not distinguishable from run-to-run variation".
  The size is stated with every "moved". (Three rounds each of two builds that do not differ separate
  completely 1 time in 10, which is why there is a floor.)
- **Level with LevelDB.** shale is **level with** LevelDB on a row only if the rounds of the two
  interleave and the ratio of the medians is within 0.95 to 1.05. Otherwise the ratio is given.
- **Because.** Every "because" cites a profile share, a syscall count or an allocation count, or says
  "not examined". The allocation counts are those that were counted for the change that reduced them
  (libc++, v0.1.1, E+F and E+F+G), cited from there.
- **No run is dropped.** A failed run is handled as in section 5, and both runs are recorded.
- **Two levels.** In 2026-10 the first pass read at one of two levels, run by run, while the repetitions
  within a run agreed to about 1% and the compacted pass had no levels
  ([what profiling found](../benchmarks.md#what-profiling-found-and-what-changed)); the smallest gap
  between its levels was on `readmissing`, about 1.5 million against 1.2 to 1.3 million entries a second,
  which is over 10%. A read row of a build **shows two levels** here if the three repetitions of each of
  its first-pass runs (the run of A and the three rounds of B) agree within 2%, and the medians of these
  runs fall into two groups such that every run of the higher group is more than 10% above every run of
  the lower. The figure to quote stays the median of the rounds, and any ratio "at the higher level" is
  extra and labelled as such.

## 7. What does not change

The conditions of the main table are those of 2026-10, in the table
[how the engines were matched](../benchmarks.md#how-the-engines-were-matched): an 8 MiB block cache,
checksums verified on every read, no compression, a 4 MiB write buffer, 4096-byte blocks, a Bloom filter
of 10 bits a key, 1000 open files, and LevelDB as `tools/bench_leveldb.cc` configures it. The 1 GiB cache
appears only in files under `docs/benchmarks/2026-11-m2-diagnostic/` and in rows marked diagnostic.

## 8. Appendix: the scripts

These are the scripts that make the runs and the tables. They are not tracked as scripts, because a
tracked script needs a test under the repository's rules; they are given here verbatim, and a run
uses these texts.

### `measure.sh`

Usage: `BENCH=<command> DB=$DB WORK=$WORK EVIDENCE=$WORK/evidence.txt measure.sh <label> <arguments>`. It
waits for the quiet check of section 3, makes `$DB` empty, and appends to the evidence file the label, the
quiet line (the load, and `waited`, as in section 7.1 of the 2026-10 evidence), the start, the uptime, the
power, the command, the exit status, the end and the uptime. The fields and their wording are those of
sections 0 and 7.1 of the 2026-10 evidence, laid out as in section 0 (the label at the margin, each field
indented by two spaces); the command is written with `$WORK` and `$DB` for those paths, as the
2026-10 evidence writes `$DB`. Three things are new: a line naming the standard output file, a line naming the standard error file and
its last line (with `$WORK` and `$DB` substituted) when the run wrote any, and the whole-name matching of the
quiet check.

```bash
#!/usr/bin/env bash
# measure.sh <label> <argument>...
# Waits for the machine to be quiet, then runs one command under caffeinate and appends the record of
# the run to $EVIDENCE. $BENCH is the command (shale_bench, or a probe), $DB the database directory (made
# empty before the run), $WORK the scratch directory. Paths under $WORK and $DB are written to the
# record as $WORK and $DB. Standard output and standard error are kept in $WORK/logs (never in the
# repository), and a non-empty standard error is named in the record. Exit status: the run's, or 3 if the machine was not
# quiet within two hours of wall time.
set -uo pipefail
: "${BENCH:?}" "${DB:?}" "${EVIDENCE:?}" "${WORK:?}"
label=$1; shift
names='^(ninja|clang|clang\+\+|clang-.*|cmake|ctest|ld|ld64|shale_.*|fuzz_.*)$'
busy='process named ninja, clang, cmake, ctest, ld, ld64, shale_* or fuzz_*'
limit=7200 poll=15 begin=$(date +%s)
while :; do
  load=$(sysctl -n vm.loadavg | tr -d '{}' | awk '{print $1}')
  procs=$(ps -axo comm= | awk -F/ '{print $NF}' | grep -E "$names" || true)
  power=$(pmset -g batt | head -1)
  waited=$(($(date +%s) - begin))
  if awk -v l="$load" 'BEGIN{exit !(l < 2.0)}' && [ -z "$procs" ] && [[ "$power" == *"AC Power"* ]]; then break; fi
  if [ "$waited" -ge "$limit" ]; then
    echo "$label: not quiet after $waited s: load $load, processes '${procs//$'\n'/ }', $power" | tee -a "$EVIDENCE" >&2
    exit 3
  fi
  caffeinate -ims sleep "$poll"
done
base="$WORK/logs/${label//[^A-Za-z0-9._-]/_}"
errfile="$base.stderr" outfile="$base.stdout"
for a in "$@"; do case "$a" in --json=*) mkdir -p "$(dirname "${a#--json=}")";; esac; done
mkdir -p "$WORK/logs"
shown() { local s=$*; s=${s//"$DB"/\$DB}; s=${s//"$WORK"/\$WORK}; echo "$s"; }
rm -rf "$DB"; mkdir -p "$DB"
{
  echo "$label"
  echo "  quiet: load average (1 min) $load, no $busy, waited $waited s"
  echo "  start: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "  uptime at start: $(uptime)"
  echo "  power at start: $power"
  echo "  command: $(shown "caffeinate -ims $BENCH $*")"
  echo "  standard output kept in: $(shown "$outfile")"
} >> "$EVIDENCE"
caffeinate -ims "$BENCH" "$@" > "$outfile.run" 2> "$errfile.run"
status=$?
{
  echo "  exit status: $status"
  if [ -s "$errfile.run" ]; then
    echo "  standard error kept in: $(shown "$errfile")"
    echo "  last line of standard error: $(shown "$(tail -n 1 "$errfile.run")")"
  fi
  echo "  end: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "  uptime at end: $(uptime)"
  echo
} >> "$EVIDENCE"
cat "$outfile.run" >> "$outfile"; cat "$outfile.run"
if [ -s "$errfile.run" ]; then cat "$errfile.run" >> "$errfile"; cat "$errfile.run" >&2; fi
rm -f "$errfile.run" "$outfile.run"
exit "$status"
```

### `table.py`

Usage: `python3 -I table.py FILE...`. With one file it prints a row for each benchmark; with the three
round files of a build and pass it prints the figure to quote and the lowest and highest round, as
section 6 says.

```python
#!/usr/bin/env python3
"""table.py FILE... : Markdown rows from shale_bench JSON files.

One file: each benchmark's median ops/s, with the lowest and highest of its runs in brackets (the main
table). Several files (the rounds of one build and pass): the median of the files' medians, with the
lowest and highest file in brackets (the figure to quote), and p99 the same way.
"""
import json
import statistics
import sys


def main(paths):
    docs = [json.load(open(p)) for p in paths]
    names = [r["name"] for r in docs[0]["results"]]
    print("| benchmark | ops/s | p99 (us) |")
    print("| --- | --- | --- |")
    for name in names:
        rows = [next(r for r in d["results"] if r["name"] == name) for d in docs]
        if len(docs) == 1:
            ops = [run["ops_per_sec"] for run in rows[0]["runs"]]
            p99 = [run["p99_us"] for run in rows[0]["runs"]]
            mid_ops, mid_p99 = rows[0]["ops_per_sec"], rows[0]["p99_us"]
        else:
            ops = [r["ops_per_sec"] for r in rows]
            p99 = [r["p99_us"] for r in rows]
            mid_ops = statistics.median(ops)
            mid_p99 = statistics.median(p99) if None not in p99 else None
        cell = "n/a"
        if None not in p99:
            cell = f"{mid_p99:,.1f} [{min(p99):,.1f}, {max(p99):,.1f}]"
        print(f"| {name} | {mid_ops:,.0f} [{min(ops):,.0f}, {max(ops):,.0f}] | {cell} |")


if __name__ == "__main__":
    main(sys.argv[1:])
```

## 9. Revision of 2026-11-22: settled writes, and first-pass reads on the same files

This section was added after the first measured session and before any run of the second. Where it
differs from sections 1, 2, 4, 5 and 8, it replaces them for the second session: in section 1, what the
first pass reads (9.4 and 9.5) and what `final` is (9.3). The rest of the document holds as it stands.
The first session followed sections 1 to 8 as they were first committed.

### 9.1 Why the protocol was revised

The first session ran from 18:50 to 21:14 on 2026-11-22. Its first-pass rows and its write rows did not
compare like with like. An investigation that evening, outside this protocol (its experiments ran under
a load of 1.8 to 3.0, and none of them is a result of this protocol), found the mechanism and proved it
in both directions:

- **E makes compaction faster.** Compaction reads every input table through the table iterator that E
  rewrote. Under a scheduler that runs every background task as soon as it is queued, so that both
  builds make the same 87 compactions in the same order, each compaction took 1.01 to 1.49 times as long
  in v0.1.1 as in E, with a median of 1.2.
- **At 1,000,000 keys that decides whether the one background thread keeps up with `overwrite`.** E's
  does: 50 compactions and no write slowdowns, and nothing is owed when the last write returns. v0.1.1's
  does not: 36 to 39 compactions and 79 to 123 slowdowns by then, with 12 or 13 compactions still owed
  (0.65 to 0.71 s of work, 111 to 120 MB written).
- **The figures were taken at the wrong moment.** shale_bench stopped the clock and read the counters
  when the last write returned, and the work still owed then ran inside the next benchmark, the
  first-pass `readrandom`. Once that work is done, both builds have made 50 compactions, and the write
  amplification of `overwrite` is 7.02 to 7.08 in v0.1.1 and 7.39 in E. Counting the wait, `overwrite`
  took 5.01 to 5.07 s in v0.1.1 and 4.72 to 4.76 s in E, against the 12 to 28% "slower" of the session.
- **The first-pass reads ran on different trees.** The files per level were 0/4/49/8 for v0.1.1, once
  its owed work was done, and 3/5/50/8 for E, E+F and E+F+G. The three level-0 tables that E leaves are
  an accident of 35 flushes taken four at a time, and nothing compacts them, since shale has no seek
  compaction ([ADR 0001](../design/0001-lsm-and-leveled-compaction.md)). Every seek positions in them:
  first-pass `seekrandom` made 3.14 million table reads against 1.42 million, and `readmissing` 25,000
  against 9,000. On the same files, the two builds make the same number of table reads to within 0.01%,
  and E's `seekrandom` is 11 to 12% faster.
- **The engine is not at fault.** Under the scheduler above, both builds write byte-identical databases.
  E slowed down by 4 microseconds before each table read reproduced v0.1.1's counters byte for byte in
  two runs of three (536,381,130 bytes read and 458,447,945 written by compaction, 5/7/52/4), and v0.1.1
  with its writes slowed reproduced E's in both of its runs (693,788,325 and 605,590,520, 3/5/50/8).

So the revision changes the bench tool and the protocol, and nothing in `src/` or `include/`.

### 9.2 What the bench tool does now

The rules below are those of `tools/` at the commit that merges this revision, which every build uses
(9.3). `shale_bench --help` states them as well.

**Rule 1: the background is waited for.** shale_bench waits until the engine is idle:

- after each run of a write benchmark, and in `overwrite` also after its untimed load, compaction and
  reopen, before its timed puts;
- after the open of a database that is already there (rule 2);
- after a `--compact_before_reads` compaction;
- after each read benchmark, before the next one.

So no benchmark runs while the work of another is being done. **Idle** means both of these, for each
engine:

- **shale:** no background task queued or running, and no level at a compaction score of 1 or more.
  The tasks are counted by the scheduler that the adapter opens each database with: the scheduler with a
  thread of its own that shale makes for a database opened without one, wrapped so that it counts each
  task from the moment it is handed in until it returns. shale's background task queues the next one
  before it returns when there is more to do, so the count is zero only when nothing is left. The scores
  are the picker's rule, worked out from `shale.stats` (the files and bytes of each level) and the
  options: level 0 its files over the smaller of `l0_compaction_trigger` and `l0_stop_trigger` (4),
  level n its bytes over 10 MiB times 10 to the power n - 1, and the last level not counted. The scores
  alone would not do: a flush that is queued or running leaves every score below 1 until it lands.
- **LevelDB:** the same rule, from LevelDB's public interfaces. LevelDB runs every flush and every
  compaction as a task that it hands to its Env's `Schedule()`, and a task that finds more to do
  schedules the next one before it returns. The adapter opens each database with an Env around the
  default one that counts those tasks, so the count is as exact as shale's. LevelDB has no property that
  says a compaction is running, and the Env is the interface it gives for exactly this work. The scores
  are LevelDB 1.23's rule (`VersionSet::Finalize()`: level 0 its files over 4, level n its bytes over
  10 MiB times 10 to the power n - 1, the last level not counted), from the files and sizes that
  `leveldb.sstables` lists. LevelDB's counters are the sums over the levels of the Time(sec), Read(MB)
  and Write(MB) columns of `leveldb.stats`, which it prints for each level as a whole number. LevelDB
  reports no number of compactions and no write amplification.

The bench looks every 10 ms, for at most 300 s. An engine that is not idle by then ends the run with
status 1, and the run is a failed run (section 5). The memtable that the last writes filled in part is
not waited for: neither engine writes it out on its own.

**What a write reports.** Its ops/s with the clock stopped when the last write returns, as before and as
db_bench does; the wait for the background, from that return to the look that found the engine idle
(`settle_seconds`, to within the 10 ms of a look); its ops/s counting the wait (`settled_ops_per_sec`);
and the engine's state at both points (`returned` and `settled`: the files and bytes of each level, the
counters, the write amplification, whether it was busy, and the highest score). The summary of a write
benchmark gives the medians over its runs, and `settled_write_amplification`.

**Rule 2: reads on a database that is already there.** `--existing_db=overwrite` opens `--db` as it is
and runs only read benchmarks on it. The reads are checked against the record of what `overwrite` writes
with the same `--num`, `--value_size` and `--seed`, which shale_bench works out again without writing, so
they are checked as strictly as the reads after a write in the same run; a database that does not match
ends the run with status 1. A path that holds no database is an error, and nothing is made there. The
open may write what the database's log holds into a table, as any open does, and the background is waited
for before the first read. With `--compact_before_reads` the database is then compacted, and waited for
again.

**Rule 4: every read row records the tree it read.** The summary of each read benchmark has `tree`: the
engine's state when its first run began, with the files per level; and `after`: its state once the
background was idle after its last run, the tree it left. For shale the two are the same. The table
script prints both trees of a read, and both states of a write (9.9).

### 9.3 Builds: one bench tool for every engine version (rule 3)

In the first session each build was compiled from its own tree, so v0.1.1's `shale_bench` was v0.1.1's
tool and lacked the flags of later commits. Now every engine version is built with the `tools/` of the
commit that merges this revision, called `P` below:

| Name | Engine (everything but `tools/`) | `tools/` |
| --- | --- | --- |
| `v011` | `9fefedf` (tag `v0.1.1`) | `P` |
| `e` | `194417f` | `P` |
| `ef` | `2f95cb6` | `P` |
| `final` | `c7af18b` | `P` |

Each is made by `build.sh <name> <engine> P` (9.9): a clean export of the engine's commit into
`$WORK/<name>`, its `tools/` replaced by the one exported from `P`, and the same CMake command as section
2, building the target `shale_bench` only. The other tools are not built, and could not all be:
`shalectl` reads internal headers that changed between these commits. `shale_bench` and its libraries use
the public API and three internal headers (`util/check.h`, `util/coding.h` and `util/random.h`) that are
the same in all four. The fifth build, `leveldb`, is `final`'s binary with `--engine=leveldb`, as before.

`final` is the engine of E+F+G with the tools of `P`. Before any run,

```
git diff --quiet c7af18b P -- src include CMakeLists.txt CMakePresets.json
```

must succeed, or measurement stops. `build.sh` records each build in `$WORK/builds.txt`: the name, the
two commits as full SHAs, the SHA-256 of its `tools/` (of the sorted list of its files' SHA-256), the
LevelDB that CMake found, and the time. Before M, the driver stops unless the check above succeeds
with the checkout at `P`, every build was made from the engine commit that the table above gives its
name, every build was made with `P`'s `tools/`, the four SHA-256 of `tools/` are the same and are those
recorded, and every build found LevelDB. Before this revision was merged, a dry run built all four from
its branch and ran every block of 9.5 with them.

### 9.4 The first-pass databases (rule 2)

The first pass of every shale build reads the same files, and LevelDB reads its own database, made the
same way.

- **Made once, before block A.** For shale, `final`'s binary runs `fillrandom,overwrite` at 1,000,000 keys
  with `--repeat=1` into `$WORK/master-shale-made`; the bench waits for the background after each write
  run. An APFS clone of it at `$WORK/master-shale` is then opened once as it is, with
  `--existing_db=overwrite --benchmarks=readseq --repeat=1`: what its log held is written into a table and
  the background is waited for again, and `readseq` checks that every key is there. That clone is the
  database that the session reads: every clone of it opens with nothing to replay and nothing to
  compact. LevelDB's is made the same way at `$WORK/master-leveldb`, with `--engine=leveldb`.
- **Recorded.** The build that made each, its files per level (the tree that `readseq` left, from the
  JSON of the open), and the SHA-256 of every file in it, in the evidence. The JSON files of the two
  steps are kept under `docs/benchmarks/2026-11-m2-masters/`.
- **Copied for every run.** Every read run of A, B and C, in both passes, and every probe of D, reads an
  APFS clone of its engine's database (`cp -c -R`), made by `measure.sh` (9.9) before the quiet check
  of the run, every file of which it reads once first (9.5). No run sees the files or the compactions
  of another. The compacted pass is the same command with `--compact_before_reads`, on its own clone.
- **Checked at the end.** After D, the SHA-256 of every file is taken again and must equal the record.
  If it does not, the session is void and the controller is told.

shale has no seek compaction, so every shale build reads the tree of the shale database, and each read
row shows it. LevelDB compacts a file that reads seek through too often, during the reads that do it:
that is LevelDB's design, as in 2026-10. Its row shows the tree when the benchmark began and the tree it
left, and the bench waits for any such work before the next benchmark.

### 9.5 Order and commands

The session runs the checks of 9.3 and 9.9, then **M** (the two databases of 9.4), then A, B, W, C and D
in that order, then the check of 9.4. The order of builds in a round is section 4's table, unchanged.
Every run goes through `measure.sh`. `<reads>` is `readrandom,readmissing,readseq,seekrandom`; "on a
clone" means that `measure.sh` makes `$DB` an APFS clone of `$WORK/master-shale`, or of
`$WORK/master-leveldb` for `leveldb`, and "empty" that it makes `$DB` empty.

**Every clone is read once before its run.** Before every run that reads a clone (M's open, every read
run of A, B and C in both passes, and every probe of D in both states), `measure.sh` reads every file of
the clone once (`find "$DB" -type f -exec cat {} + > /dev/null`), for every shale build and for LevelDB
alike, before the quiet check, so that the reading is over before the check and the run start. The
reason is the operating system's cache. A clone is a new file, and its blocks are not in the cache when
it is made, where a database that a run had just written had them in the first session and in 2026-10.
The review of this revision measured it: v0.1.1's `readrandom` (Release, 20,000 keys, `--repeat=3`) on
a clone made just before the run read 480,548 operations a second in its first repetition, with a
p99.9 of 269 us, against 1,474,473 and 1,478,433, with a p99.9 of 1.3 us, in the next two. Unread, the
first repetition of every first-pass row would read its blocks from the disk, and the later two would
not. The compacted pass is read first too. A compaction of the whole range leaves as it is a table of
the deepest level that no table above it overlaps, in LevelDB and in shale (`DB::CompactRange()` says
so): in the review, 3 of the 12 tables that a compacted LevelDB clone read at 200,000 keys (6.4 of about
29 MB) were the database's own files, which nothing had read, while shale's compaction rewrote every
table at 300,000 keys. How many such tables there are depends on the tree, so the rule is the same for
every clone.

**M.** For `shale` with `final` and for `leveldb`:

```
shale_bench --engine=shale --benchmarks=fillrandom,overwrite --num=1000000 --repeat=1 --db=$WORK/master-shale-made --json=docs/benchmarks/2026-11-m2-masters/shale-write.json
shale_bench --engine=shale --existing_db=overwrite --benchmarks=readseq --num=1000000 --repeat=1 --db=$WORK/master-shale --json=docs/benchmarks/2026-11-m2-masters/shale-open.json
```

the second on a clone of `$WORK/master-shale-made`, and the same for `leveldb` with `--engine=leveldb` and
`leveldb` in place of `shale` in the paths.

**A.** The writes of the command of 2026-10, for the five builds in the order `v011, e, ef, final,
leveldb`, empty:

```
shale_bench --engine=shale --benchmarks=fillseq,fillrandom,fillsync,overwrite --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-<build>-writes.json
```

then its reads, first pass, in the same order, on a clone:

```
shale_bench --engine=shale --existing_db=overwrite --benchmarks=<reads> --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-<build>.json
```

then the compacted pass, in the same order, on a clone, with `--compact_before_reads` and
`2026-11-m2-<build>-compacted.json`. For `leveldb` the first argument is `--engine=leveldb`.

**B.** Rounds 1 to 3. In each, every build's first pass and then every build's compacted pass, each in the
round's order, on a clone:

```
shale_bench --engine=shale --existing_db=overwrite --benchmarks=<reads> --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-reads/<build>-<first|compacted>-<r>.json
```

with `--compact_before_reads` in the compacted runs. `overwrite` is no longer in this list: its rows come
from A and W.

**W.** Unchanged (section 5), empty. The writes now report the settled figures beside the others.

**C.** As section 5's C, with the reads of B on a clone:

```
shale_bench --engine=shale --cache_size=1073741824 --existing_db=overwrite --benchmarks=<reads> --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-diagnostic/<final|leveldb>-<pass>-<r>.json
```

**D.** As section 4's D, with the revised probe and `profile.sh` (9.9), for `v011` and `final`, on clones of
the shale database: the counting probe runs `<reads>`, and each profile runs its one benchmark, each with
the database opened as `overwrite` left it, and compacted first in the compacted state. Every
`profile.sh` invocation is recorded with all its arguments.

### 9.6 What is reported

Section 6 holds, with these additions:

- **Write rows.** The ops/s when the last write returns and the ops/s counting the wait are both given,
  each under its name; "moved" and "level with" are applied to each on its own, and a sentence says which
  it reads. The write amplification is given when the last write returned and once settled.
- **Read rows.** Each gives the tree it read and the tree it left. Every shale read row is expected to show
  the shale database's tree in the first pass, and to leave it as it found it; a row that does not is
  reported as a deviation, with its trees.
- **Compaction.** This session counts compactions and their bytes, and times the wait for the background
  after the writes; it does not time compactions one by one. A figure for the time of one compaction cites
  the investigation of 9.1 as such.
- **Other sessions.** The first-pass figures of this session are not comparable with those of the first
  session or of 2026-10, and are not set beside them: the database that the first pass reads was opened
  once (9.4), so what the last writes left in the memtable is a level 0 table and the memtable is empty,
  and its files are read into the operating system's cache before the run (9.5) instead of having just
  been written.

### 9.7 What does not change

Section 1's comparisons (the builds, the passes, the read, write and diagnostic rows) and the names of
the builds, apart from what 9.3 to 9.5 change; section 3 (the record, the quiet check, Go); the order
of builds in a round and the three rounds; the blocks A, B, W, C and D and their order; section 6's
rules (interleave, "moved", "level with", "because", no run dropped, two levels); and section 7's
conditions. A failed run is
run again once, as in section 5; a clone that cannot be made stops the session, as a quiet check that
times out does.

### 9.8 How the first session is recorded

The first session is superseded and is not published as the result. Its JSON files and evidence stay at
commit `3270703` (PR #27, which is not merged); the results of the second session replace them. The
evidence file of the second session has a section on the first that gives what it showed, its counters
(among them v0.1.1's 5/7/52/4 when the last write returned, and 0/4/49/8 after the first-pass
`readrandom`), the cause of 9.1, and the experiments that proved it, with their numbers. The
investigation's scratch files and logs stay in the first session's scratch directory, untouched. The
second session's scratch directory is a new one, `r2` inside it, and is the `$WORK` of the second
session's records.

### 9.9 Appendix: the revised scripts

These replace section 8's for the second session; section 8 keeps the texts that the first session used.
`table.py` reads the files of both sessions; a figure that a file does not have is `n/a`.

Every script that the session runs is the text below, and so is every source of a probe it runs:
`measure.sh`, `driver.sh`, `build.sh`, `table.py`, `check-table.sh`, `profile.sh`, `build-probe.sh`,
`probe.cc` and `interpose.c`. The driver compares each of them with this section of the protocol in the
checkout before M, refuses to start if one differs, and writes their SHA-256 into the evidence.
`measure.sh` keeps the load limit of 2.0. A dry run, with `DRY_RUN=1`, may raise that limit and differ in
nothing else, and the driver then writes the limit it used into the dry run's evidence. The dry runs of
this revision were made so, because another agent's work kept the load above 2. The scripts that read
the samples after the session (`sample_parse.py` and its categories) are not run by the driver, and the
evidence gives them as before.

#### `measure.sh`

Usage as in section 8, with one more variable: `DB_FROM=<directory>` makes `$DB` an APFS clone of that
directory instead of an empty one, and every file of the clone is then read once. Both are done before
the quiet check. The record of a run says which, in a line `database:`. A database that cannot be made
as asked is recorded and stops the run with status 4, before the command runs.

```bash
#!/usr/bin/env bash
# measure.sh <label> <argument>...
# Makes the database, waits for the machine to be quiet, then runs one command under caffeinate and
# appends the record of the run to $EVIDENCE. $BENCH is the command (shale_bench, or a probe), $DB the
# database directory, $WORK the scratch directory. $DB is made empty, or, if $DB_FROM is set, an APFS
# clone of the directory $DB_FROM (cp -c -R), so that the run reads the files of that database and
# none of another run's; every file of a clone is then read once, so that its blocks are in the
# operating system's cache when the run starts, as a database that a run has just written has them.
# All of that is done before the quiet check, so that the check waits for the machine to be quiet
# after it. Paths under $WORK and $DB are written to the record as $WORK and $DB.
# Standard output and standard error are kept in $WORK/logs (never in the repository), and a
# non-empty standard error is named in the record. Exit status: the run's, 3 if the machine was not
# quiet within two hours of wall time, or 4 if the database could not be made as asked (the run is
# not made).
set -uo pipefail
: "${BENCH:?}" "${DB:?}" "${EVIDENCE:?}" "${WORK:?}"
label=$1; shift
in_work() { local s=$*; echo "${s//"$WORK"/\$WORK}"; }
rm -rf "$DB"
if [ -n "${DB_FROM:-}" ]; then
  made="an APFS clone of $(in_work "$DB_FROM") (cp -c -R), every file read once before the quiet check"
  if ! mkdir -p "$(dirname "$DB")" || ! cp -c -R "$DB_FROM" "$DB"; then
    echo "$label: the clone of $(in_work "$DB_FROM") could not be made at $(in_work "$DB")" | tee -a "$EVIDENCE" >&2
    exit 4
  fi
  if ! find "$DB" -type f -exec cat {} + > /dev/null; then
    echo "$label: the files of the clone at $(in_work "$DB") could not be read" | tee -a "$EVIDENCE" >&2
    exit 4
  fi
else
  made="empty"
  mkdir -p "$DB"
fi
names='^(ninja|clang|clang\+\+|clang-.*|cmake|ctest|ld|ld64|shale_.*|fuzz_.*)$'
busy='process named ninja, clang, cmake, ctest, ld, ld64, shale_* or fuzz_*'
limit=7200 poll=15 begin=$(date +%s)
while :; do
  load=$(sysctl -n vm.loadavg | tr -d '{}' | awk '{print $1}')
  procs=$(ps -axo comm= | awk -F/ '{print $NF}' | grep -E "$names" || true)
  power=$(pmset -g batt | head -1)
  waited=$(($(date +%s) - begin))
  if awk -v l="$load" 'BEGIN{exit !(l < 2.0)}' && [ -z "$procs" ] && [[ "$power" == *"AC Power"* ]]; then break; fi
  if [ "$waited" -ge "$limit" ]; then
    echo "$label: not quiet after $waited s: load $load, processes '${procs//$'\n'/ }', $power" | tee -a "$EVIDENCE" >&2
    exit 3
  fi
  caffeinate -ims sleep "$poll"
done
base="$WORK/logs/${label//[^A-Za-z0-9._-]/_}"
errfile="$base.stderr" outfile="$base.stdout"
for a in "$@"; do case "$a" in --json=*) mkdir -p "$(dirname "${a#--json=}")";; esac; done
mkdir -p "$WORK/logs"
shown() { local s=$*; s=${s//"$DB"/\$DB}; s=${s//"$WORK"/\$WORK}; echo "$s"; }
{
  echo "$label"
  echo "  quiet: load average (1 min) $load, no $busy, waited $waited s"
  echo "  start: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "  uptime at start: $(uptime)"
  echo "  power at start: $power"
  echo "  database: \$DB is $(in_work "$DB"), $made"
  echo "  command: $(shown "caffeinate -ims $BENCH $*")"
  echo "  standard output kept in: $(shown "$outfile")"
} >> "$EVIDENCE"
caffeinate -ims "$BENCH" "$@" > "$outfile.run" 2> "$errfile.run"
status=$?
{
  echo "  exit status: $status"
  if [ -s "$errfile.run" ]; then
    echo "  standard error kept in: $(shown "$errfile")"
    echo "  last line of standard error: $(shown "$(tail -n 1 "$errfile.run")")"
  fi
  echo "  end: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "  uptime at end: $(uptime)"
  echo
} >> "$EVIDENCE"
cat "$outfile.run" >> "$outfile"; cat "$outfile.run"
if [ -s "$errfile.run" ]; then cat "$errfile.run" >> "$errfile"; cat "$errfile.run" >&2; fi
rm -f "$errfile.run" "$outfile.run"
exit "$status"
```

#### `table.py`

Usage as in section 8: `python3 -I table.py FILE...`. It adds, for a write, the settled ops/s and the
write amplification at both moments; and for every row the engine's state before and after: the files
per level and the compaction counter, when a write's last write returned and once settled, and the tree
that a read began on and the one it left.

```python
#!/usr/bin/env python3
"""table.py FILE... : Markdown rows from shale_bench JSON files.

One file: each benchmark's median ops/s, with the lowest and highest of its runs in brackets (the main
table). Several files (the rounds of one build and pass): the median of the files' medians, with the
lowest and highest file in brackets (the figure to quote), and p99 the same way.

A write's row also gives, the same way, its ops/s counting the wait for the background ("settled"), and
its write amplification when the last write returned and once settled.

The last two columns give the engine's state before and after, as "before -> after", or once when the
two are the same: for a write, its state when the last write returned and once settled; for a read, the
tree it read and the tree it left. One column gives the files per level, the other the counter that
says how much compaction there was: shale's "compactions", or LevelDB's "write_mb" (the megabytes its
flushes and compactions wrote; it counts no compactions). A row of one file gives an entry for each run
of a write and one for a read; a row of several files gives one for each file (for a write, its last
run). The entries are given once if they are all the same, and all of them, in order, if not.

A figure that a file does not have, because shale_bench did not record it then, is n/a.
"""
import json
import statistics
import sys


def ranged(mid, values, digits):
    return f"{mid:,.{digits}f} [{min(values):,.{digits}f}, {max(values):,.{digits}f}]"


def once(entries):
    return entries[0] if len(set(entries)) == 1 else "; ".join(entries)


def files(state):
    return "/".join(str(n) for n in state["files_per_level"])


def counter(state):
    for name in ("compactions", "write_mb"):
        if name in state.get("counters", {}):
            return name, state["counters"][name]
    return None


def before_after(before, after):
    """The two entries of a state's files and counter, "before -> after" or once if the same."""
    if before is None:
        return "n/a", "n/a"
    after = after if after is not None else before
    files_entry = files(before) if files(before) == files(after) else f"{files(before)} -> {files(after)}"
    b, a = counter(before), counter(after)
    if b is None or a is None or b[0] != a[0]:
        counter_entry = "n/a"
    elif b[1] == a[1]:
        counter_entry = f"{b[0]} {b[1]}"
    else:
        counter_entry = f"{b[0]} {b[1]} -> {a[1]}"
    return files_entry, counter_entry


def states(row, one_file):
    """The (before, after) states of a row of one file."""
    if row["kind"] == "write":
        runs = row["runs"] if one_file else row["runs"][-1:]
        return [(run.get("returned"), run.get("settled")) for run in runs]
    return [(row.get("tree"), row.get("after"))]


def main(paths):
    docs = [json.load(open(p)) for p in paths]
    one_file = len(docs) == 1
    names = [r["name"] for r in docs[0]["results"]]
    print("| benchmark | ops/s | p99 (us) | settled ops/s | write-amp (returned / settled) "
          "| files per level (before -> after) | compaction (before -> after) |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for name in names:
        rows = [next(r for r in d["results"] if r["name"] == name) for d in docs]
        if one_file:
            runs = rows[0]["runs"]
            ops = [run["ops_per_sec"] for run in runs]
            p99 = [run["p99_us"] for run in runs]
            settled = [run.get("settled_ops_per_sec") for run in runs]
            mid_ops, mid_p99 = rows[0]["ops_per_sec"], rows[0]["p99_us"]
            mid_settled = rows[0].get("settled_ops_per_sec")
        else:
            ops = [r["ops_per_sec"] for r in rows]
            p99 = [r["p99_us"] for r in rows]
            settled = [r.get("settled_ops_per_sec") for r in rows]
            mid_ops = statistics.median(ops)
            mid_p99 = statistics.median(p99) if None not in p99 else None
            mid_settled = statistics.median(settled) if None not in settled else None
        p99_cell = ranged(mid_p99, p99, 1) if None not in p99 else "n/a"
        settled_cell = ranged(mid_settled, settled, 0) if None not in settled else "n/a"
        if rows[0]["kind"] == "write":
            amp_cell = once([f"{r.get('write_amplification') or 'n/a'} / "
                             f"{r.get('settled_write_amplification') or 'n/a'}" for r in rows])
        else:
            amp_cell = "n/a"
        entries = [before_after(b, a) for r in rows for b, a in states(r, one_file)]
        files_cell = once([f for f, _ in entries])
        counter_cell = once([c for _, c in entries])
        print(f"| {name} | {ranged(mid_ops, ops, 0)} | {p99_cell} | {settled_cell} | {amp_cell} "
              f"| {files_cell} | {counter_cell} |")


if __name__ == "__main__":
    main(sys.argv[1:])
```

#### `check-table.sh`

Usage: `WORK=$WORK check-table.sh`, from the root of the repository checkout; the driver runs it before
M and writes its result into the evidence, with the SHA-256 of every file of the fixture. It runs
`table.py` over the fixture that the repository keeps in
[`2026-11-m2-table-fixture/`](2026-11-m2-table-fixture/expected.md), and compares the output with
`expected.md` there, which was checked against the files by hand. The fixture is a file of the first
session (v0.1.1's of block A, from `3270703`), writes of shale and of LevelDB at 200,000 keys (LevelDB's
busy when their last write returned), and LevelDB first-pass reads at 20,000 keys that compacted while
they ran, alone and as three rounds. It is a fixture of the script and not a result.

```bash
#!/usr/bin/env bash
# check-table.sh: runs $WORK/table.py over the fixture that the repository keeps beside the protocol,
# docs/benchmarks/2026-11-m2-table-fixture/, and compares the output with expected.md there, which
# was checked against the files by hand. The fixture is a file of the first session, writes of shale
# and LevelDB of which LevelDB's were busy when the last write returned, and reads of LevelDB that
# compacted while they ran, alone and as three rounds. Run from the root of the repository checkout.
# Exit status 0 if they agree, 1 if not.
set -uo pipefail
: "${WORK:?}"
fixture=docs/benchmarks/2026-11-m2-table-fixture
if [ ! -f "$fixture/expected.md" ]; then
  echo "check-table: no fixture at $fixture; run from the root of the repository checkout" >&2
  exit 1
fi
got=$(cd "$fixture" && for files in first-session-v011.json shale-writes.json leveldb-writes.json \
  leveldb-first.json "leveldb-first-1.json leveldb-first-2.json leveldb-first-3.json"; do
  echo "## $files"
  # shellcheck disable=SC2086
  python3 -I "$WORK/table.py" $files
done)
if [ "$got" = "$(cat "$fixture/expected.md")" ]; then
  echo "check-table: ok"
  exit 0
fi
diff <(echo "$got") "$fixture/expected.md"
echo "check-table: table.py's output differs from the fixture's" >&2
exit 1
```

#### `build.sh`

Usage: `WORK=$WORK REPO=<the repository checkout> build.sh <name> <engine commit> <tools commit>`, run
for the four builds of 9.3 before Go, at `-j4`. It records each build in `$WORK/builds.txt`, which the
driver checks.

```bash
#!/usr/bin/env bash
# build.sh <name> <engine rev> <tools rev>: exports <engine rev> into $WORK/<name>, replaces its tools/
# with the tools/ of <tools rev>, and builds shale_bench in Release at -j4 (protocol, section 9.3). It
# appends to $WORK/builds.txt the name, the two commits, the SHA-256 of the build's tools/ (the SHA-256
# of the sorted list of its files' SHA-256), the LevelDB that CMake found, and the time. $REPO is the
# repository checkout. Refuses a <name> that is already there.
set -euo pipefail
: "${WORK:?}" "${REPO:?}"
name=$1 engine=$2 tools=$3
dir="$WORK/$name"
if [ -e "$dir" ]; then
  echo "build.sh: $dir is already there" >&2
  exit 2
fi
mkdir -p "$dir"
git -C "$REPO" archive "$engine" | tar -x -C "$dir"
rm -rf "$dir/tools"
git -C "$REPO" archive "$tools" tools | tar -x -C "$dir"
start=$(date +%s)
cmake -S "$dir" -B "$dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHALE_BUILD_BENCH=ON \
  -DSHALE_BUILD_TESTS=OFF -DCMAKE_PREFIX_PATH="$(brew --prefix leveldb)" > "$WORK/$name.cfg.log" 2>&1
cmake --build "$dir/build" --target shale_bench -j4 > "$WORK/$name.build.log" 2>&1
digest=$(cd "$dir" && find tools -type f | LC_ALL=C sort | xargs shasum -a 256 | shasum -a 256 | cut -d' ' -f1)
leveldb=$(grep -o 'found leveldb [0-9.]*' "$WORK/$name.cfg.log" || echo "no leveldb found")
echo "$name engine $(git -C "$REPO" rev-parse "$engine") tools $(git -C "$REPO" rev-parse "$tools") tools-sha256 $digest $leveldb built in $(($(date +%s) - start)) s" >> "$WORK/builds.txt"
```

#### `driver.sh`

Usage: `WORK=$WORK driver.sh`, run from the root of the repository checkout at `P`. It makes the checks
(the texts of this section, the builds of 9.3, `check-table.sh`) and then every run of 9.5 in its order
through `measure.sh`. With `CHECK_ONLY=1` it stops after the checks, which lets them be made before Go.
`NUM`, `R`, `MEASURE`, `EVIDENCE` and `DRY_RUN` may be set for a dry run; the session sets none of them.
It writes `$WORK/driver.done` when it ends, whatever the reason, its progress to `$WORK/driver.log`, and
a line `STOP:` to the evidence when it stops early.

```bash
#!/usr/bin/env bash
# Runs the revised protocol (section 9): the checks, the two first-pass databases, then blocks A, B, W,
# C and D, each run through measure.sh, then the check that the first-pass databases did not change.
# Run from the root of the repository checkout. Always ends: it writes $WORK/driver.done on exit.
set -uo pipefail
: "${WORK:?}"
export WORK DB=${DB:-$WORK/db} EVIDENCE=${EVIDENCE:-$WORK/evidence-runs.txt} SCRATCH=$WORK/probe
MEASURE=${MEASURE:-$WORK/measure.sh} NUM=${NUM:-1000000} R=${R:-docs/benchmarks}
log=$WORK/driver.log
rm -f "$WORK/driver.done"
trap 'echo "$(date +%H:%M:%S) driver exit" >> "$log"; touch "$WORK/driver.done"' EXIT
say() { echo "$(date +%H:%M:%S) $*" >> "$log"; }
stop() { say "STOP: $1"; echo "STOP: $1" >> "$EVIDENCE"; exit "$2"; }
ALL="v011 e ef final leveldb"
order() { case $1 in 1) echo "v011 e ef final leveldb";; 2) echo "leveldb final ef e v011";; 3) echo "ef v011 leveldb e final";; esac; }
bin() { echo "$WORK/$([ "$1" = leveldb ] && echo final || echo "$1")/build/tools/shale_bench"; }
eng() { [ "$1" = leveldb ] && echo --engine=leveldb || echo --engine=shale; }
# The first-pass database that a build reads: LevelDB's for leveldb, shale's for every shale build.
master() { [ "$1" = leveldb ] && echo "$WORK/master-leveldb" || echo "$WORK/master-shale"; }
tools_digest() { (cd "$1" && find tools -type f | LC_ALL=C sort | xargs shasum -a 256 | shasum -a 256 | cut -d' ' -f1); }

say "checks"
# 1. Every script that the session runs, and every source of a probe it runs, is the text of section
#    9.9. A dry run (DRY_RUN=1) may differ in the load limit of measure.sh alone, and says so in the
#    evidence.
protocol=docs/benchmarks/2026-11-m2-protocol.md
text_of() { awk -v h="#### \`$1\`" '$0 == h { p = 1; next } p && /^```/ { if (++c == 2) exit; next } p && c == 1' "$protocol"; }
pinned() {
  local name=$1 file=$2 text
  text=$(text_of "$name")
  [ -n "$text" ] || stop "no $name in section 9.9 of $protocol" 6
  [ -f "$file" ] && [ "$text" = "$(cat "$file")" ] && return 0
  if [ "$name" = measure.sh ] && [ "${DRY_RUN:-0}" = 1 ] &&
    [ "$(sed 's/exit !(l < [0-9.]*)/exit !(l < 2.0)/' "$file")" = "$text" ]; then
    echo "DRY RUN: measure.sh is the text of section 9.9 with the load limit $(grep -o 'exit !(l < [0-9.]*' "$file" | grep -o '[0-9.]*$') instead of 2.0" >> "$EVIDENCE"
    return 0
  fi
  stop "the $name given is not the $name of section 9.9" 6
}
scripts=(measure.sh "$MEASURE" driver.sh "${BASH_SOURCE[0]}" build.sh "$WORK/build.sh"
  table.py "$WORK/table.py" check-table.sh "$WORK/check-table.sh" profile.sh "$WORK/probe/profile.sh"
  build-probe.sh "$WORK/probe/build-probe.sh" probe.cc "$WORK/probe/probe.cc"
  interpose.c "$WORK/probe/interpose.c")
for ((i = 0; i < ${#scripts[@]}; i += 2)); do pinned "${scripts[i]}" "${scripts[i + 1]}"; done
# The probes and the interposer were built after their sources, and the probes after their builds.
for b in v011 final; do
  [ "$WORK/probe/probe-$b" -nt "$WORK/probe/probe.cc" ] && [ "$WORK/probe/probe-$b" -nt "$(bin $b)" ] ||
    stop "probe-$b is missing, or older than probe.cc or the build" 6
done
[ "$WORK/probe/interpose.dylib" -nt "$WORK/probe/interpose.c" ] || stop "interpose.dylib is missing, or older than interpose.c" 6
# 2. The four builds are the engine commits of section 9.3, each with the tools/ of this checkout,
#    the same in each and as builds.txt records it, with LevelDB found; and the engine of final is
#    that of E+F+G.
git diff --quiet c7af18b HEAD -- src include CMakeLists.txt CMakePresets.json || stop "the checkout's engine is not c7af18b's" 6
digest=""
for n in v011 e ef final; do
  case $n in v011) engine=9fefedf ;; e) engine=194417f ;; ef) engine=2f95cb6 ;; final) engine=c7af18b ;; esac
  engine=$(git rev-parse "$engine^{commit}")
  d=$(tools_digest "$WORK/$n")
  rec=$(grep "^$n engine " "$WORK/builds.txt" | tail -1)
  [ "$(echo "$rec" | awk '{print $3}')" = "$engine" ] || stop "build $n was not made from the engine commit $engine" 6
  tools_rev=$(echo "$rec" | awk '{print $5}')
  [ -n "$tools_rev" ] && [ "$(git rev-parse "$tools_rev:tools")" = "$(git rev-parse HEAD:tools)" ] || stop "build $n was not made with the tools/ of this checkout" 6
  case "$rec" in *"tools-sha256 $d found leveldb"*) ;; *) stop "build $n: tools/ $d, or its LevelDB, is not what builds.txt records" 6;; esac
  [ -z "$digest" ] || [ "$d" = "$digest" ] || stop "the four builds' tools/ differ" 6
  digest=$d
done
# 3. The table script reads the fixture that the repository keeps beside the protocol as recorded.
result=$(WORK=$WORK bash "$WORK/check-table.sh" 2>&1) || { echo "$result" >> "$log"; stop "table.py does not read its fixture as recorded" 6; }
{
  echo "checks, all passed:"
  echo "  the scripts, each the text of section 9.9 (SHA-256):"
  for ((i = 0; i < ${#scripts[@]}; i += 2)); do echo "    $(shasum -a 256 < "${scripts[i + 1]}" | cut -d' ' -f1)  ${scripts[i]}"; done
  echo "  the builds: v011 9fefedf, e 194417f, ef 2f95cb6, final c7af18b, each with the tools/ of $(git rev-parse --short HEAD) (SHA-256 $digest), LevelDB found"
  echo "  the table fixture, docs/benchmarks/2026-11-m2-table-fixture/ (SHA-256):"
  (cd docs/benchmarks/2026-11-m2-table-fixture && shasum -a 256 -- *) | sed 's/^/    /'
  echo "  $result"
} >> "$EVIDENCE"
say "checks passed"
if [ "${CHECK_ONLY:-0}" = 1 ]; then say "CHECK_ONLY: stopped after the checks"; exit 0; fi

# run <label> <build> <database> <args...>: one run of shale_bench through measure.sh. <database> is
# "-" for an empty one, or a directory, of which measure.sh makes a clone and reads every file once
# before the quiet check. A failure is run again once; a second failure, a quiet check that timed out
# or a database that could not be made stops the driver.
run() {
  local label=$1 build=$2 from=$3; shift 3
  [ "$from" = - ] && from=""
  for attempt in 1 2; do
    say "start $label (attempt $attempt)"
    local l=$label; [ $attempt = 2 ] && l="$label (rerun)"
    DB_FROM=$from BENCH=$(bin "$build") bash "$MEASURE" "$l" "$@"
    local rc=$?
    say "end $label rc=$rc"
    [ $rc = 0 ] && return 0
    [ $rc = 3 ] && stop "quiet check timed out" 3
    [ $rc = 4 ] && stop "the database could not be made" 4
  done
  stop "$label failed twice" 1
}
READS=readrandom,readmissing,readseq,seekrandom

say "M: the first-pass databases"
for e in shale leveldb; do
  b=$([ $e = leveldb ] && echo leveldb || echo final)
  made=$WORK/master-$e-made m=$WORK/master-$e
  # Written once by final's binary, every background task finished (shale_bench settles), then
  # opened once as it is, so that what its log held is in a table and the background is done
  # again: every clone of it then opens with nothing to replay.
  DB=$made run "M write $e" $b - $(eng $b) --benchmarks=fillrandom,overwrite --num=$NUM --repeat=1 --db=$made --json=$R/2026-11-m2-masters/$e-write.json
  DB=$m run "M open $e" $b "$made" $(eng $b) --existing_db=overwrite --benchmarks=readseq --num=$NUM --repeat=1 --db=$m --json=$R/2026-11-m2-masters/$e-open.json
  (cd "$m" && find . -type f | LC_ALL=C sort | xargs shasum -a 256) > "$m.sha256"
  {
    echo "M record $e"
    echo "  made by: $(bin $b | sed "s#$WORK#\$WORK#") $(eng $b)"
    echo "  files per level: $(python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); print("/".join(map(str, d["results"][0]["after"]["files_per_level"])))' "$R/2026-11-m2-masters/$e-open.json") (the tree that readseq left, settled)"
    echo "  checksums (shasum -a 256, kept in \$WORK/master-$e.sha256):"
    sed 's/^/    /' "$m.sha256"
    echo
  } >> "$EVIDENCE"
  say "master $e recorded"
done

# Every read run reads a clone of its engine's database, every file of which measure.sh has read once
# before the quiet check, in both passes.
say "A writes"
for b in $ALL; do run "A writes $b" $b - $(eng $b) --benchmarks=fillseq,fillrandom,fillsync,overwrite --num=$NUM --repeat=3 --db=$DB --json=$R/2026-11-m2-$b-writes.json; done
say "A first pass"
for b in $ALL; do run "A first $b" $b "$(master $b)" $(eng $b) --existing_db=overwrite --benchmarks=$READS --num=$NUM --repeat=3 --db=$DB --json=$R/2026-11-m2-$b.json; done
say "A compacted pass"
for b in $ALL; do run "A compacted $b" $b "$(master $b)" $(eng $b) --existing_db=overwrite --benchmarks=$READS --num=$NUM --repeat=3 --db=$DB --json=$R/2026-11-m2-$b-compacted.json --compact_before_reads; done
say "B"
for r in 1 2 3; do
  for b in $(order $r); do run "B first r$r $b" $b "$(master $b)" $(eng $b) --existing_db=overwrite --benchmarks=$READS --num=$NUM --repeat=3 --db=$DB --json=$R/2026-11-m2-reads/$b-first-$r.json; done
  for b in $(order $r); do run "B compacted r$r $b" $b "$(master $b)" $(eng $b) --existing_db=overwrite --benchmarks=$READS --num=$NUM --repeat=3 --db=$DB --json=$R/2026-11-m2-reads/$b-compacted-$r.json --compact_before_reads; done
done
say "W"
for r in 1 2 3; do
  for b in $(order $r); do run "W r$r $b" $b - $(eng $b) --benchmarks=fillseq,fillrandom,overwrite --num=$NUM --repeat=3 --db=$DB --json=$R/2026-11-m2-writes/$b-$r.json; done
done
say "C"
for r in 1 2 3; do
  if [ $r = 2 ]; then co="leveldb final"; else co="final leveldb"; fi
  for pass in first compacted; do
    for b in $co; do
      extra=(); [ $pass = compacted ] && extra=(--compact_before_reads)
      cs=(); [ $b = final ] && cs=(--cache_size=1073741824)
      run "C $pass r$r $b" $b "$(master $b)" $(eng $b) ${cs[@]+"${cs[@]}"} --existing_db=overwrite --benchmarks=$READS --num=$NUM --repeat=3 --db=$DB --json=$R/2026-11-m2-diagnostic/$b-$pass-$r.json ${extra[@]+"${extra[@]}"}
    done
  done
done
say "D"
# probe <build> <label> <args to profile.sh between the label and the database>: the probe reads
# $DB, which measure.sh makes a clone of the shale database and reads once before the quiet check.
probe() {
  local build=$1 label=$2; shift 2
  for attempt in 1 2; do
    local l=$label; [ $attempt = 2 ] && l="$label (rerun)"
    say "start probe $label (attempt $attempt)"
    DB_FROM=$WORK/master-shale BENCH=$WORK/probe/profile.sh bash "$MEASURE" "$l" "$WORK/probe/probe-$build" "$label" "$@" "$DB" overwrite; local rc=$?
    local san=${l//[^A-Za-z0-9._-]/_}
    [ $rc = 3 ] && stop "quiet check timed out" 3
    [ $rc = 4 ] && stop "the database could not be made" 4
    if [ $rc = 0 ] && grep -q "^$label: probe exit 0" "$WORK/logs/$san.stdout" && grep -q '^interpose:' "$SCRATCH/prof-$label.out"; then say "end probe $label ok"; return 0; fi
    say "end probe $label FAILED (rc=$rc)"
  done
  stop "probe $label failed twice" 1
}
for b in v011 final; do
  for state in first compacted; do
    c=0; [ $state = compacted ] && c=1
    probe $b "count-$b-$state" "$NUM" $READS 1 1 1 $c
  done
  for state in first compacted; do
    c=0; [ $state = compacted ] && c=1
    for bench in readrandom:3 readseq:40 seekrandom:2; do
      n=${bench%%:*}; loops=${bench##*:}
      probe $b "prof-$b-$state-$n" "$NUM" $n "MARK loop $n" 4 $loops $c
    done
  done
done
say "check of the first-pass databases"
for e in shale leveldb; do
  if (cd "$WORK/master-$e" && find . -type f | LC_ALL=C sort | xargs shasum -a 256) | cmp -s - "$WORK/master-$e.sha256"; then
    echo "M check $e: every file as recorded" >> "$EVIDENCE"; say "master $e unchanged"
  else
    echo "M check $e: CHANGED since it was recorded" >> "$EVIDENCE"; stop "master $e CHANGED" 5
  fi
done
say "ALL DONE"
```

#### The probe

The probe and its scripts live in `$WORK/probe/`, as in the first session; the texts below are the ones
the session uses, and the evidence gives the diff of each against the first session's. `probe.cc` drives
the engines through shale_bench's own adapters (`NewShaleBenchEngine(Options())` and
`NewLevelDBBenchEngine()`), so a write waits for the background as shale_bench's does; with
`PROBE_EXISTING=overwrite` it opens its database as it is and checks its reads against
`RecordOfWrite()`, as `--existing_db` does; it waits for the background after a `PROBE_COMPACT`
compaction and after each read benchmark; and it prints the engine's state after each benchmark.
`profile.sh` takes every argument in a fixed place, and the probe opens the database given as it is:
`$DB`, which `measure.sh` made a clone of the shale database, and read, before the quiet check.
`build-probe.sh` is the one compile command of section 4 D, unchanged, run for `v011` and `final` before
Go; `interpose.dylib` is built from `interpose.c` with `cc -dynamiclib -O2 -o interpose.dylib
interpose.c`, as in 2026-10. The driver checks that each binary is newer than its source.

#### `profile.sh`

```bash
#!/usr/bin/env bash
# profile.sh <probe> <label> <num> <benchmarks> <mark-or-delay> <seconds> <loops> <compact 0|1> <database or -> <existing write or ->
# Runs the probe and samples it: after a line matching the pattern <mark> appears, or after <delay> seconds
# when the mark is a number. <database> is the directory the probe opens, as it is, as the database that
# <existing write> made: the clone that measure.sh made at $DB before the quiet check (and read once, in
# the first-pass state). With "-" the probe gets an empty directory of its own, removed after the run.
# Revised from profile-2026-11-22.sh for the settled protocol: every argument in a fixed place, and the
# database given.
set -uo pipefail
S=$SCRATCH
probe=$1 label=$2 num=$3 benches=$4 mark=$5 secs=$6 loops=$7 compact=$8 database=$9 existing=${10}
if [ "$database" = - ]; then
  db="$S/probe-db"; rm -rf "$db"; mkdir -p "$db"
else
  db=$database
fi
out="$S/prof-$label"
if [ "$compact" = 1 ]; then export PROBE_COMPACT=1; else unset PROBE_COMPACT; fi
if [ "$existing" != - ]; then export PROBE_EXISTING=$existing; else unset PROBE_EXISTING; fi
DYLD_INSERT_LIBRARIES="$S/interpose.dylib" "$probe" "${ENGINE:-shale}" "$num" "$db" "$benches" 0 "$loops" > "$out.out" 2>&1 &
pid=$!
if [[ "$mark" =~ ^[0-9]+$ ]]; then
  sleep "$mark"
else
  until grep -qE "$mark" "$out.out" || ! kill -0 $pid 2>/dev/null; do sleep 0.2; done
  sleep 0.3
fi
sample $pid "$secs" -file "$out.sample.txt" > /dev/null 2>&1
wait $pid
echo "$label: probe exit $?"
if [ "$database" = - ]; then rm -rf "$db"; fi
```

#### `build-probe.sh`

```bash
#!/usr/bin/env bash
# build-probe.sh <name>: the one compile command for both probes.
set -euo pipefail
n=$1; B=$WORK/$n/build
c++ -O2 -g -std=c++20 -I"$WORK/$n/include" -I"$WORK/$n/tools" -I"$(brew --prefix leveldb)/include" \
  "$WORK/probe/probe.cc" -o "$WORK/probe/probe-$n" \
  "$B/tools/libshale_bench_support.a" "$B/tools/libshale_bench_leveldb.a" \
  "$B/src/libshale_db.a" "$B/src/libshale_io.a" "$B/src/libshale_core.a" "$B/src/libshale_util.a" \
  -L"$(brew --prefix leveldb)/lib" -lleveldb
```

#### `probe.cc`

```cpp
// A scratch probe, not part of the repository: runs the shale_bench workloads once through the
// same functions and the same engine adapters as shale_bench, on shale or on LevelDB, and prints
// the engine's state after each benchmark, so that a row can be explained from what the engine says
// it did.
//
// Revised for the settled protocol (section 9 of the protocol), from probe-2026-11-22.cc:
// - the engines are shale_bench's own adapters (NewShaleBenchEngine(Options()) and
//   NewLevelDBBenchEngine()), with the options shale_bench gives them by default;
// - a write benchmark waits for the background as shale_bench does (RunWriteBenchmark() settles);
// - with PROBE_EXISTING set to the write benchmark that made the database at <db>, the database is
//   opened as it is and its background waited for, as shale_bench --existing_db does, and the reads
//   are checked against RecordOfWrite();
// - PROBE_COMPACT compacts once before the first read, then waits for the background;
// - after each read benchmark the background is waited for, as shale_bench does.
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bench_engine.h"
#include "bench_leveldb.h"
#include "bench_workloads.h"
#include "shale/options.h"

using namespace shale;

static void Snap(const std::string& label) {
  using Fn = void (*)(const char*);
  static Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "interpose_snapshot"));
  if (fn) fn(label.c_str());
}

static void PrintState(BenchEngine* engine, const char* when) {
  EngineState s;
  Status st = engine->GetState(&s);
  if (!st.ok()) {
    std::printf("state %s: %s\n", when, st.ToString().c_str());
    return;
  }
  std::string counters;
  for (const auto& [name, value] : s.counters) counters += " " + name + "=" + std::to_string(value);
  std::printf("state %s: busy=%d score=%.3f levels %s wa %s counters%s\n", when,
              s.background_busy ? 1 : 0, s.highest_score, FilesPerLevelText(s.files_per_level).c_str(),
              s.write_amplification.c_str(), counters.c_str());
  std::fflush(stdout);
}

static int Fail(const char* what, const Status& s) {
  std::printf("%s failed: %s\n", what, s.ToString().c_str());
  return 1;
}

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "probe <shale|leveldb> <num> <db> <bench,bench...> [pause_ms_before_each] [loops]\n");
    return 2;
  }
  const std::string engine_name = argv[1];
  WorkloadOptions options;
  options.num = std::strtoull(argv[2], nullptr, 10);
  options.sync_num = 10000;
  options.value_size = 100;
  options.seed = 301;
  options.db = argv[3];
  std::vector<Benchmark> list;
  {
    std::string l = argv[4];
    size_t st = 0;
    while (true) {
      size_t c = l.find(',', st);
      auto n = FindBenchmark(l.substr(st, c == std::string::npos ? c : c - st));
      if (!n) { std::fprintf(stderr, "bad benchmark\n"); return 2; }
      list.push_back(*n);
      if (c == std::string::npos) break;
      st = c + 1;
    }
  }
  const int pause_ms = argc > 5 ? std::atoi(argv[5]) : 0;
  const int loops = argc > 6 ? std::atoi(argv[6]) : 1;
  const std::unique_ptr<BenchEngine> e =
      engine_name == "shale" ? NewShaleBenchEngine(Options()) : NewLevelDBBenchEngine();
  std::optional<Populated> source;
  bool compact_pending = false;
  if (const char* existing = std::getenv("PROBE_EXISTING"); existing != nullptr && *existing != '\0') {
    const std::optional<Benchmark> written_by = FindBenchmark(existing);
    if (!written_by || !BenchmarkWrites(*written_by)) { std::fprintf(stderr, "bad PROBE_EXISTING\n"); return 2; }
    if (Status s = e->OpenExisting(options.db); !s.ok()) return Fail("open existing", s);
    PrintState(e.get(), "opened");
    SettleResult settled;
    if (Status s = Settle(e.get(), options.settle, &settled); !s.ok()) return Fail("settle after open", s);
    std::printf("MARK opened existing, settled in %.3f s\n", settled.seconds);
    PrintState(e.get(), "settled");
    source = RecordOfWrite(options, *written_by);
    compact_pending = std::getenv("PROBE_COMPACT") != nullptr;
  }
  for (Benchmark b : list) {
    RunResult r;
    Status s;
    if (pause_ms) std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms));
    Snap(std::string("before ") + BenchmarkName(b));
    if (BenchmarkWrites(b)) {
      Populated p;
      s = RunWriteBenchmark(e.get(), options, b, &p, &r);
      source = std::move(p);
      compact_pending = std::getenv("PROBE_COMPACT") != nullptr;
    } else {
      if (!source) { std::fprintf(stderr, "a read with no database to read\n"); return 2; }
      if (compact_pending) {
        Status c = e->CompactAll();
        compact_pending = false;
        SettleResult settled;
        Status w = c.ok() ? Settle(e.get(), options.settle, &settled) : c;
        std::printf("MARK compacted %s\n", w.ok() ? "ok" : w.ToString().c_str()); std::fflush(stdout);
        if (!w.ok()) return Fail("compact", w);
      }
      std::printf("MARK loop %s x%d\n", BenchmarkName(b), loops); std::fflush(stdout);
      Snap(std::string("loop start ") + BenchmarkName(b));
      for (int i = 0; i < loops; i++) {
        s = RunReadBenchmark(e.get(), options, b, *source, &r);
        if (!s.ok()) break;
      }
      Snap(std::string("after ") + std::to_string(loops) + " x " + BenchmarkName(b));
      if (s.ok()) {
        SettleResult settled;
        s = Settle(e.get(), options.settle, &settled);
      }
    }
    if (!s.ok()) return Fail(BenchmarkName(b), s);
    std::printf("== %s %s: %.0f ops/s, %.3f s, engine %.3f s, wa %s\n", engine_name.c_str(), BenchmarkName(b), r.OpsPerSecond(), r.seconds, r.engine_seconds, r.write_amplification ? r.write_amplification->c_str() : "-");
    if (r.latency) std::printf("   p50 %.2f p99 %.2f p99.9 %.2f us, avg %.2f us\n", r.latency->p50, r.latency->p99, r.latency->p999, r.latency->average);
    if (r.settle_seconds) std::printf("   settled in %.3f s: %.0f ops/s\n", *r.settle_seconds, r.SettledOpsPerSecond());
    PrintState(e.get(), "after");
  }
  return 0;
}
```

#### `interpose.c`

```c
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stdatomic.h>

static atomic_long fullfsync, fsync_calls, fcntl_other;
static atomic_long fullfsync_fail;
static atomic_long n_pread, n_read, n_write, n_pwrite, n_mmap, n_munmap;
static atomic_long b_pread, b_write;

#define DYLD_INTERPOSE(_replacement, _replacee) \
  __attribute__((used)) static struct { const void* replacement; const void* replacee; } \
  _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
    (const void*)(unsigned long)&_replacement, (const void*)(unsigned long)&_replacee };

static int my_fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  long arg = va_arg(ap, long);
  va_end(ap);
  int r = fcntl(fd, cmd, arg);
  if (cmd == F_FULLFSYNC) {
    atomic_fetch_add(&fullfsync, 1);
    if (r != 0) atomic_fetch_add(&fullfsync_fail, 1);
  } else {
    atomic_fetch_add(&fcntl_other, 1);
  }
  return r;
}
static int my_fsync(int fd) {
  atomic_fetch_add(&fsync_calls, 1);
  return fsync(fd);
}
static ssize_t my_pread(int fd, void* b, size_t n, off_t o) { ssize_t r = pread(fd, b, n, o); atomic_fetch_add(&n_pread, 1); if (r > 0) atomic_fetch_add(&b_pread, r); return r; }
static ssize_t my_read(int fd, void* b, size_t n) { atomic_fetch_add(&n_read, 1); return read(fd, b, n); }
static ssize_t my_write(int fd, const void* b, size_t n) { ssize_t r = write(fd, b, n); atomic_fetch_add(&n_write, 1); if (r > 0) atomic_fetch_add(&b_write, r); return r; }
static ssize_t my_pwrite(int fd, const void* b, size_t n, off_t o) { atomic_fetch_add(&n_pwrite, 1); return pwrite(fd, b, n, o); }
static void* my_mmap(void* a, size_t l, int p, int f, int fd, off_t o) { atomic_fetch_add(&n_mmap, 1); return mmap(a, l, p, f, fd, o); }
static int my_munmap(void* a, size_t l) { atomic_fetch_add(&n_munmap, 1); return munmap(a, l); }
DYLD_INTERPOSE(my_fcntl, fcntl)
DYLD_INTERPOSE(my_pread, pread)
DYLD_INTERPOSE(my_read, read)
DYLD_INTERPOSE(my_write, write)
DYLD_INTERPOSE(my_pwrite, pwrite)
DYLD_INTERPOSE(my_mmap, mmap)
DYLD_INTERPOSE(my_munmap, munmap)
DYLD_INTERPOSE(my_fsync, fsync)

static long last[12];
void interpose_snapshot(const char* label) {
  long cur[12] = {atomic_load(&n_pread), atomic_load(&b_pread), atomic_load(&n_read), atomic_load(&n_write), atomic_load(&b_write), atomic_load(&n_pwrite), atomic_load(&n_mmap), atomic_load(&n_munmap), atomic_load(&fullfsync), atomic_load(&fsync_calls), 0, 0};
  fprintf(stderr, "SNAP %-28s pread=%ld (%ld B) read=%ld write=%ld (%ld B) pwrite=%ld mmap=%ld munmap=%ld F_FULLFSYNC=%ld fsync=%ld\n", label,
          cur[0]-last[0], cur[1]-last[1], cur[2]-last[2], cur[3]-last[3], cur[4]-last[4], cur[5]-last[5], cur[6]-last[6], cur[7]-last[7], cur[8]-last[8], cur[9]-last[9]);
  for (int k = 0; k < 12; k++) last[k] = cur[k];
}

__attribute__((destructor)) static void report(void) {
  fprintf(stderr, "interpose: pread=%ld (%ld bytes) read=%ld write=%ld (%ld bytes) pwrite=%ld mmap=%ld munmap=%ld\n", atomic_load(&n_pread), atomic_load(&b_pread), atomic_load(&n_read), atomic_load(&n_write), atomic_load(&b_write), atomic_load(&n_pwrite), atomic_load(&n_mmap), atomic_load(&n_munmap));
  fprintf(stderr, "interpose: F_FULLFSYNC=%ld (failed %ld) fsync=%ld other_fcntl=%ld\n",
          atomic_load(&fullfsync), atomic_load(&fullfsync_fail), atomic_load(&fsync_calls),
          atomic_load(&fcntl_other));
}
```
