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
- **Read rows.** The same three, in three interleaved rounds (section 4, B).
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

Five clean exports, each made with `git archive <rev> | tar -x -C $WORK/<name>`, where `$WORK` is a
scratch directory outside the repository. `<rev>` is `9fefedf` for `v011`, `194417f` for `e`,
`2f95cb6` for `ef`, and for `final` the commit that merges this protocol.

The library of `final` must be that of E+F+G (`c7af18b`). Before any run:

```
git diff --quiet c7af18b <final> -- src include
```

must succeed, or measurement stops. (`10e9eb7`, which added `shale_bench --cache_size`, changed `tools/`
and `tests/` only, and the protocol adds a document.)

Each export is built with

```
cmake -S $WORK/<name> -B $WORK/<name>/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSHALE_BUILD_BENCH=ON -DSHALE_BUILD_TESTS=OFF -DCMAKE_PREFIX_PATH=$(brew --prefix leveldb)
cmake --build $WORK/<name>/build --target shale_bench
```

This is the same compiler and the same flags as 2026-10, where each build was made with
`cmake --preset release` (the preset is Ninja with `CMAKE_BUILD_TYPE=Release`) and the same options. The
binary of a build is `$WORK/<name>/build/tools/shale_bench`.

LevelDB is `final`'s `--engine=leveldb`: one LevelDB build, the one linked into `final`'s `shale_bench`.

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
- no process whose name matches `ninja|clang|cmake|ctest|ld64|shale_|fuzz_` is running;
- the machine draws from AC power;
- the run is made under `caffeinate -ims`.

The check waits up to 2 hours for these to hold. Past that, the run is not made and the controller is
told. `measure.sh` (appendix) does the check, the run and the record.

**Go.** No run starts before the controller's Go, which means the GPU evaluation that shares this machine
has finished. Builds are made before Go, at `-j4` at most; runs are not.

## 4. Order

`$DB` is an empty directory on the internal SSD, made empty again before every run. In the lists below,
"first pass" is the command as given and "compacted" is the same command with `--compact_before_reads`.

**A. The tables.** Full commands, first pass, in the order `v011, e, ef, final, leveldb`. Then the
compacted pass, in the same order. This gives the main table, the ablation, and the first pass for the
levels of 2026-10.

**B. Read rounds.** Rounds r = 1, 2, 3. In each round, every build's first-pass run, and then every
build's compacted run. The order of the builds rotates:

| Round | Order |
| --- | --- |
| 1 | `v011 e ef final leveldb` |
| 2 | `e ef final leveldb v011` |
| 3 | `ef final leveldb v011 e` |

**C. Diagnostic rounds.** Rounds r = 1, 2, 3: `final` with `--cache_size=1073741824`, then `leveldb`, each
in both passes.

**D. Probes.** For `v011` and `final`:

- **System calls** are counted with `interpose.c`, the library that counts `pread`, `read`, `write`,
  `mmap`, `munmap`, `fsync` and `fcntl(F_FULLFSYNC)` and that the probe snapshots around each benchmark.
- **Profiles** are made with `sample`, through `profile.sh`, `sample_parse.py` and `categories.py`, of
  `readrandom`, `readseq` and `seekrandom` in both states (first pass and compacted).
- The sources of the probe (`probe.cc`), `interpose.c`, `profile.sh`, `sample_parse.py` and
  `categories.py` are taken verbatim from section 6 and section 7.5 of
  [the 2026-10 evidence](2026-10-m2-evidence.txt). Any change to them is recorded as a diff against that
  text, next to the results. The probe is compiled as there, with `c++ -O2 -g -std=c++20`, against the
  libraries of the build it is probing.
- LevelDB's counts are not taken again: it made no `pread` in 2026-10, because it maps its table files.

## 5. Commands, verbatim

`<bin>` is `$WORK/<name>/build/tools/shale_bench`, and for `leveldb` it is `final`'s, with
`--engine=leveldb`. Every command is run from `measure.sh`, so the record is
`caffeinate -ims <bin> <arguments>`. The commands run from the root of the repository checkout that
will hold the JSON files, so the `--json` paths are relative to it.

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

**C.**

```
shale_bench --engine=shale --cache_size=1073741824 --benchmarks=overwrite,readrandom,seekrandom --num=1000000 --repeat=3 --db=$DB --json=docs/benchmarks/2026-11-m2-diagnostic/<final|leveldb>-<pass>-<r>.json
```

`<pass>` is `first` or `compacted`, with `--compact_before_reads` in the second. The `leveldb` runs
leave out `--cache_size` (it is a usage error with `--engine=leveldb`) and use `--engine=leveldb`:
LevelDB's cache is as configured, 8 MiB.

The evidence of 2026-10 puts `--repeat=3` and `--num=1000000` in the A commands and the B and C commands
keep them, so a run's median is of three repeats in all of them.

## 6. What is reported

These rules are fixed before any run.

- **Main table.** Each run's medians, with the lowest and highest run in brackets.
- **Read rows.** The median of the three rounds' medians, with the lowest and highest round in brackets.
  This is the figure to quote.
- **Moved.** A change **moved** a row only if all three of its round medians lie on one side of all three
  of the build before it. Otherwise the text says "not distinguishable from run-to-run variation".
- **Level with LevelDB.** shale is **level with** LevelDB on a row only if the ratio of the medians is
  within 0.95 to 1.05 and the rounds of the two interleave. Otherwise the ratio is given.
- **Because.** Every "because" cites a profile share, a syscall count or an allocation count, or says
  "not examined". The allocation counts are those that were counted for the change that reduced them
  (libc++, v0.1.1, E+F and E+F+G), cited from there.
- **No run is dropped.** A run that fails is run again once, and both are recorded.
- **Two levels.** If the first pass shows the two levels of 2026-10 (a read row that comes out at one of two
  levels from run to run, [described in the 2026-10 results](../benchmarks.md)), it is reported as such,
  run by run.

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

Usage: `BENCH=<bin> DB=$DB EVIDENCE=$WORK/evidence.txt measure.sh <label> <shale_bench arguments>`. It
waits for the quiet check of section 3 (up to 2 hours, then exits with status 3 without a run), makes
`$DB` empty, and appends to the evidence file the label, the load, the start, the uptime, the power, the
command, the exit status, the end and the uptime, in the format of section 7.1 of the 2026-10 evidence.

```bash
#!/usr/bin/env bash
# measure.sh <label> <shale_bench argument>...
# Waits for the machine to be quiet, then runs one shale_bench under caffeinate and appends the record
# of the run to $EVIDENCE. $BENCH is the binary, $DB the database directory (made empty before the run).
# Exit status: the run's, or 3 if the machine was not quiet within two hours.
set -uo pipefail
: "${BENCH:?}" "${DB:?}" "${EVIDENCE:?}"
label=$1; shift
busy='ninja|clang|cmake|ctest|ld64|shale_|fuzz_'
limit=7200 waited=0
while :; do
  load=$(sysctl -n vm.loadavg | tr -d '{}' | awk '{print $1}')
  procs=$(ps -axo comm= | awk -F/ '{print $NF}' | grep -E "$busy" || true)
  power=$(pmset -g batt | head -1)
  if awk -v l="$load" 'BEGIN{exit !(l < 2.0)}' && [ -z "$procs" ] && [[ "$power" == *"AC Power"* ]]; then break; fi
  if [ "$waited" -ge "$limit" ]; then
    echo "not quiet after $waited s: load $load, processes '${procs//$'\n'/ }', $power" | tee -a "$EVIDENCE" >&2
    exit 3
  fi
  sleep 30; waited=$((waited + 30))
done
for a in "$@"; do case "$a" in --json=*) mkdir -p "$(dirname "${a#--json=}")";; esac; done
rm -rf "$DB"; mkdir -p "$DB"
{
  echo "$label"
  echo "  quiet: load average (1 min) $load, no $busy process, waited $waited s"
  echo "  start: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "  uptime at start: $(uptime)"
  echo "  power at start: $power"
  echo "  command: caffeinate -ims $BENCH $*"
} >> "$EVIDENCE"
caffeinate -ims "$BENCH" "$@"
status=$?
{
  echo "  exit status: $status"
  echo "  end: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "  uptime at end: $(uptime)"
  echo
} >> "$EVIDENCE"
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
