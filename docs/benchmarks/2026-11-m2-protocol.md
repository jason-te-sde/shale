# The protocol for measuring the read path, 2026-11

This document is committed before any measured run, and the measurement follows it step by step. It
says what is compared, how each build is made, how the machine is held, in what order the runs are
made, the exact commands, and what the results may claim. The reporting rules in section 6 are fixed
here so that no result can change them afterwards.

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
