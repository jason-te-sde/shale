#!/usr/bin/env bash
# Runs the shale_bench binary on small databases it makes in a directory of its own, and checks what
# a benchmark has to get right before its numbers mean anything: that every workload runs and
# prints its line, that the read benchmarks find what the writes left (they check every answer
# against a record of the keys written, so a wrong answer is an exit status and not a number), that
# a write benchmark starts from an empty database, that a seed repeats the same workload, and that
# the JSON is JSON and says where it ran. It checks no timing: a time on a loaded machine proves
# nothing either way.
#
# Usage: shale_bench_test.sh <path to the shale_bench binary> <leveldb|no-leveldb>
#
# The second argument says whether the binary was built with the LevelDB engine, which only a build
# that found LevelDB has. The test then checks what that build has to do, and otherwise that the
# binary refuses to run an engine it does not have, and says why.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/scripts/lib.sh
. "$here/../scripts/lib.sh"

bench="${1:?usage: shale_bench_test.sh <path to the shale_bench binary> <leveldb|no-leveldb>}"
leveldb="${2:?usage: shale_bench_test.sh <path to the shale_bench binary> <leveldb|no-leveldb>}"
case "$leveldb" in
  leveldb | no-leveldb) ;;
  *)
    echo "shale_bench_test: the second argument is leveldb or no-leveldb, not '$leveldb'" >&2
    exit 2
    ;;
esac
if [ ! -x "$bench" ]; then
  echo "shale_bench_test: $bench is not an executable" >&2
  exit 2
fi
# The JSON is read with Python. A test that skipped itself without it would pass as if it had run.
if ! command -v python3 >/dev/null 2>&1; then
  echo "shale_bench_test: python3 is needed to read the JSON and is not installed" >&2
  exit 2
fi
tmp="$(mktemp -d "${TMPDIR:-/tmp}/shale_bench_test.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

# check <description> <exit status> <argument>...
# Runs shale_bench with the arguments and requires the exit status. Standard output is kept in
# $tmp/out and standard error in $tmp/err, for out_has and err_has.
check() {
  local description=$1 want_status=$2 got
  shift 2
  cases=$((cases + 1))
  "$bench" "$@" >"$tmp/out" 2>"$tmp/err"
  got=$?
  if [ "$got" -ne "$want_status" ]; then
    fail "$description: exit status $got, wanted $want_status" \
      "$(printf 'stdout:\n%s\nstderr:\n%s' "$(cat "$tmp/out")" "$(cat "$tmp/err")")"
  fi
}

# out_has / err_has <description> <fragment>: the output of the last check has the fragment.
out_has() {
  cases=$((cases + 1))
  if ! grep -qF -e "$2" "$tmp/out"; then
    fail "$1: standard output lacks '$2'" "$(cat "$tmp/out")"
  fi
}
err_has() {
  cases=$((cases + 1))
  if ! grep -qF -e "$2" "$tmp/err"; then
    fail "$1: standard error lacks '$2'" "$(cat "$tmp/err")"
  fi
}

# row <description> <benchmark>: standard output has a line of the results table for the benchmark,
# which starts with its name and a number.
row() {
  cases=$((cases + 1))
  if ! grep -qE "^$2 +[0-9]" "$tmp/out"; then
    fail "$1: no results line for $2" "$(cat "$tmp/out")"
  fi
}

# expect_eq <description> <wanted> <got>
expect_eq() {
  cases=$((cases + 1))
  if [ "$2" != "$3" ]; then
    fail "$1: wanted '$2', got '$3'"
  fi
}

# json_has <description> <file> <python>: parses the JSON file as `d` and runs the Python, which
# raises (an assert) when what it says does not hold. json, os and sys are imported, so that the
# Python can read what the shell passes in the environment.
json_has() {
  local description=$1 file=$2 code=$3 output
  cases=$((cases + 1))
  if ! output="$(python3 -I -c 'import json, os, sys; d = json.load(open(sys.argv[1])); exec(sys.argv[2])' \
    "$file" "$code" 2>&1)"; then
    fail "$description" "$output"
  fi
}

# digest <json>: what a run must repeat when its seed does: the counts, not the times.
digest() {
  python3 -I -c '
import json, sys
d = json.load(open(sys.argv[1]))
for r in d["results"]:
    print(r["name"], r["entries"], r["source"],
          [(x["operations"], x["bytes"], x["found"]) for x in r["runs"]])
' "$1"
}

small=(--num=2000 --sync_num=20 --seed=7)

# --- Every benchmark runs, prints its line, and is checked against what was written. --------------

check "all benchmarks, twice each" 0 "${small[@]}" --repeat=2 --db="$tmp/db1" --json="$tmp/all.json"
for name in fillseq fillrandom fillsync overwrite readrandom readmissing readseq seekrandom; do
  row "all benchmarks" "$name"
done
out_has "all benchmarks: the table has a header" "write-amp"
out_has "all benchmarks: the output says where it ran" "# machine:"
out_has "all benchmarks: the output says how it was built" "# build:"
out_has "all benchmarks: the output says what its clock figure is" \
  "# clock: steady_clock, nominal period 1 ns, smallest step seen between two readings "

# The machine is named by the system's name, release and architecture, which is what uname -srm
# prints, and by nothing that names the owner of the machine: not the host name, and not the
# kernel's version string, which has the build host in it on some systems. The database is named
# by the last component of its path alone, for the same reason.
system="$(uname -srm)"
host="$(hostname)"
cases=$((cases + 1))
if [ -z "$host" ] || grep -qF -e "$host" <<<"$system"; then
  fail "this test cannot tell the host name '$host' from the system '$system'"
fi
system_line="$(grep '^# system: ' "$tmp/out" || true)"
expect_eq "the # system: line is uname -srm and nothing else" "# system: $system" "$system_line"
cases=$((cases + 1))
if grep -qF -e "$host" "$tmp/all.json"; then
  fail "the JSON holds the host name '$host'" "$(grep -F -e "$host" "$tmp/all.json")"
fi
cases=$((cases + 1))
if grep -qF -e "$host" <<<"$system_line"; then
  fail "the # system: line holds the host name '$host'" "$system_line"
fi
expect_eq "the database is named by its last component" "# database: db1" \
  "$(grep '^# database: ' "$tmp/out" || true)"
cases=$((cases + 1))
if grep -qF -e "$tmp" "$tmp/all.json"; then
  fail "the JSON holds the directory of the database" "$(grep -F -e "$tmp" "$tmp/all.json")"
fi

SYSTEM="$system" OUT="$tmp/out" json_has "the JSON of all benchmarks" "$tmp/all.json" '
import statistics

def close(a, b, rel=1e-5):
    return abs(a - b) <= rel * max(1.0, abs(b))

assert d["tool"] == "shale_bench" and d["engine"] == "shale", d["engine"]
c = d["config"]
assert c["num"] == 2000 and c["sync_num"] == 20 and c["repeat"] == 2 and c["seed"] == 7, c
assert c["value_size"] == 100 and c["compact_before_reads"] is False, c
assert c["db"] == "db1", c["db"]
names = [r["name"] for r in d["results"]]
want = ["fillseq", "fillrandom", "fillsync", "overwrite", "readrandom", "readmissing", "readseq",
        "seekrandom"]
assert names == want, names
assert c["benchmarks"] == want, c["benchmarks"]

m = d["machine"]
assert m["uname"] == os.environ["SYSTEM"], m["uname"]
assert m["cpu"] and m["logical_cores"] >= 1, m
assert m["clock_nominal_period_ns"] == 1, m
assert isinstance(m["clock_smallest_step_ns"], int) and 1 <= m["clock_smallest_step_ns"] <= 100000, m
assert "clock_resolution_ns" not in m, "what is measured is a step between readings, not a resolution"
b = d["build"]
assert b["compiler_id"] and b["compiler_version"] and b["build_type"], b
assert b["coverage"] in (True, False) and isinstance(b["flaw"], str), b

# The warning that a build is not worth quoting is printed exactly when the JSON says it is not one
# that is: an optimized build, without a sanitizer, a coverage instrument or a deliberate flaw.
representative = (b["build_type"] in ("Release", "RelWithDebInfo") and b["sanitizer"] == ""
                  and b["coverage"] is False and b["flaw"] == "")
warned = "# WARNING" in open(os.environ["OUT"]).read()
assert warned == (not representative), (warned, b)

by = {r["name"]: r for r in d["results"]}
for r in d["results"]:
    assert len(r["runs"]) == 2, r["name"]
    for x in r["runs"]:
        assert x["operations"] > 0, (r["name"], x)
        # The rates of a run are its operations and bytes over its time, with a megabyte of 2^20.
        # `seconds` is written to a nanosecond, which is a relative error of 5e-10 / seconds in the
        # rate that is worked out from it.
        slack = 1e-5 + 1e-9 / x["seconds"]
        assert close(x["ops_per_sec"], x["operations"] / x["seconds"], slack), (r["name"], x)
        assert close(x["mb_per_sec"], x["bytes"] / 1048576 / x["seconds"], slack), (r["name"], x)
        # The time inside the engine is part of the time of the loop, and a scan is all of it.
        assert 0 < x["engine_seconds"] <= x["seconds"] * (1 + 1e-6), (r["name"], x)
        if r["name"] == "readseq":
            assert close(x["engine_seconds"], x["seconds"]), (r["name"], x)
    # Every figure of a benchmark is the median of that figure over its runs, the same figure and
    # not another one: a median of two is their mean.
    for key in ("ops_per_sec", "mb_per_sec", "p50_us", "p99_us", "p999_us"):
        values = [x[key] for x in r["runs"]]
        if values[0] is None:
            assert r[key] is None and all(v is None for v in values), (r["name"], key)
        else:
            assert close(r[key], statistics.median(values)), (r["name"], key, r[key], values)

for name, ops in (("fillseq", 2000), ("fillrandom", 2000), ("overwrite", 2000), ("fillsync", 20)):
    r = by[name]
    assert r["kind"] == "write" and r["source"] == name, r["name"]
    assert all(x["operations"] == ops for x in r["runs"]), (name, r["runs"])
    for x in r["runs"] + [r]:
        float(x["write_amplification"])           # the engine reports one, and it is a number

assert by["fillseq"]["entries"] == 2000
assert 0 < by["fillrandom"]["entries"] < 2000      # random keys repeat, so some are never drawn
assert 0 < by["fillsync"]["entries"] <= 20
assert by["overwrite"]["entries"] == 2000          # loaded with every key, then overwritten

# The reads ran on what the last write benchmark, overwrite, left: every key is there.
for name in ("readrandom", "readmissing", "readseq", "seekrandom"):
    r = by[name]
    assert r["kind"] == "read" and r["source"] == "overwrite" and r["entries"] == 2000, name
    assert r["write_amplification"] is None and r["compact_seconds"] is None, name
assert all(x["found"] == 2000 for x in by["readrandom"]["runs"]), by["readrandom"]["runs"]
assert all(x["found"] == 0 for x in by["readmissing"]["runs"]), by["readmissing"]["runs"]
assert all(x["found"] == 2000 and x["operations"] == 2000 for x in by["readseq"]["runs"])
assert all(x["found"] == 2000 for x in by["seekrandom"]["runs"]), by["seekrandom"]["runs"]
assert by["readmissing"]["mb_per_sec"] == 0          # a miss moves no entry
assert by["readrandom"]["mb_per_sec"] > 0

# A scan has no latency per entry to report, and the others do. (A median of zero is possible: a
# call that takes less than the tick of the clock reads as no time at all.)
assert by["readseq"]["p50_us"] is None and by["readseq"]["p999_us"] is None
for name in ("readrandom", "readmissing", "seekrandom"):
    assert by[name]["p50_us"] is not None and by[name]["p999_us"] is not None, name
'

# A trailing slash does not make the last component empty.
check "a database path with a trailing slash" 0 "${small[@]}" --repeat=1 --db="$tmp/db-slash//" \
  --benchmarks=fillseq
expect_eq "the database is named without the slashes" "# database: db-slash" \
  "$(grep '^# database: ' "$tmp/out" || true)"

# --- The engine is chosen with --engine. ----------------------------------------------------------

check "the shale engine, by name" 0 "${small[@]}" --repeat=1 --db="$tmp/e1" --engine=shale \
  --json="$tmp/e1.json" --benchmarks=fillseq,readseq
out_has "--engine=shale: the output names the engine" "# shale_bench: engine shale,"
json_has "--engine=shale: the JSON names the engine" "$tmp/e1.json" 'assert d["engine"] == "shale"'
check "an engine that does not exist" 2 --db="$tmp/x" --engine=nosuch
err_has "an engine that does not exist" "unknown engine 'nosuch'"
err_has "an engine that does not exist: the engines are listed" "the engines are shale"
check "--engine without a value" 2 --db="$tmp/x" --engine
err_has "--engine without a value" "--engine needs a value"
check "--engine empty" 2 --db="$tmp/x" --engine=
err_has "--engine empty" "unknown engine ''"
check "--help" 0 --help
out_has "--help says that the engine can be chosen" "--engine=shale|leveldb"
out_has "--help says what the LevelDB engine is held to" "LevelDB gets the"
out_has "--help says that LevelDB reports no write amplification" "its column shows n/a"

if [ "$leveldb" = leveldb ]; then
  # Every workload runs on LevelDB through the same code as on shale: the same operations in the
  # same order, so that the counts of a run are the counts of the other engine's.
  check "all benchmarks on LevelDB" 0 "${small[@]}" --repeat=2 --db="$tmp/ldb1" --engine=leveldb \
    --json="$tmp/ldb.json"
  for name in fillseq fillrandom fillsync overwrite readrandom readmissing readseq seekrandom; do
    row "all benchmarks on LevelDB" "$name"
  done
  out_has "--engine=leveldb: the output names the engine" "# shale_bench: engine leveldb,"
  json_has "--engine=leveldb: the JSON names the engine and gives no write amplification" \
    "$tmp/ldb.json" '
assert d["tool"] == "shale_bench" and d["engine"] == "leveldb", d["engine"]
assert d["config"]["db"] == "ldb1", d["config"]["db"]
for r in d["results"]:
    if r["kind"] == "write":
        assert r["write_amplification"] == "n/a", (r["name"], r["write_amplification"])
        assert all(x["write_amplification"] == "n/a" for x in r["runs"]), r["name"]
    else:
        assert r["write_amplification"] is None, r["name"]
'
  check "the same workload on shale" 0 "${small[@]}" --repeat=2 --db="$tmp/ldb2" --engine=shale \
    --json="$tmp/shale-for-ldb.json"
  cases=$((cases + 1))
  if [ "$(digest "$tmp/ldb.json")" != "$(digest "$tmp/shale-for-ldb.json")" ]; then
    fail "the two engines did not run the same operations" \
      "$(diff <(digest "$tmp/ldb.json") <(digest "$tmp/shale-for-ldb.json"))"
  fi
else
  check "LevelDB in a build that has none" 2 --db="$tmp/x" --engine=leveldb
  err_has "LevelDB in a build that has none" "this build has no leveldb engine"
  err_has "LevelDB in a build that has none: it says how to get one" "SHALE_BUILD_BENCH"
  cases=$((cases + 1))
  if [ -e "$tmp/x" ]; then
    fail "a refused engine touched the database path" "$(ls -la "$tmp/x")"
  fi
fi

# --- Reads see the database that the most recent write benchmark left. ----------------------------

check "reads after fillsync" 0 "${small[@]}" --repeat=1 --db="$tmp/db2" --json="$tmp/sync.json" \
  --benchmarks=fillseq,fillsync,readrandom,readseq
json_has "reads after fillsync run on what fillsync left" "$tmp/sync.json" '
by = {r["name"]: r for r in d["results"]}
n = by["fillsync"]["entries"]
assert 0 < n <= 20, n
assert by["readseq"]["source"] == "fillsync" and by["readseq"]["entries"] == n
assert by["readseq"]["runs"][0]["operations"] == n, by["readseq"]["runs"]
assert by["readrandom"]["source"] == "fillsync"
# 2000 random keys of 2000, of which n are there: found is about n, never 2000.
assert by["readrandom"]["runs"][0]["found"] > 0
assert by["readrandom"]["runs"][0]["found"] < 2000
'
out_has "reads after fillsync say whose database they read" "reads fillsync's database"

# --- A write benchmark starts from an empty database. ---------------------------------------------

check "fillseq of 500 into a new directory" 0 --num=500 --repeat=1 --db="$tmp/db3" --benchmarks=fillseq
check "fillseq of 100 into the database that holds 500" 0 --num=100 --repeat=1 --db="$tmp/db3" \
  --json="$tmp/fresh.json" --benchmarks=fillseq,readseq
json_has "a write benchmark starts empty" "$tmp/fresh.json" '
by = {r["name"]: r for r in d["results"]}
assert by["readseq"]["runs"][0]["operations"] == 100, by["readseq"]["runs"]
'

# --- A seed repeats the workload; compaction does not change what the database holds. ----------

check "a run to repeat" 0 "${small[@]}" --repeat=1 --db="$tmp/db4" --json="$tmp/a.json"
check "the same run again" 0 "${small[@]}" --repeat=1 --db="$tmp/db5" --json="$tmp/b.json"
cases=$((cases + 1))
if [ "$(digest "$tmp/a.json")" != "$(digest "$tmp/b.json")" ]; then
  fail "the same seed did not repeat the workload" \
    "$(diff <(digest "$tmp/a.json") <(digest "$tmp/b.json"))"
fi

check "random writes then reads" 0 "${small[@]}" --repeat=1 --db="$tmp/db6" --json="$tmp/plain.json" \
  --benchmarks=fillrandom,readrandom,readmissing,readseq,seekrandom
check "the same with --compact_before_reads" 0 "${small[@]}" --repeat=1 --db="$tmp/db7" \
  --json="$tmp/compacted.json" --compact_before_reads \
  --benchmarks=fillrandom,readrandom,readmissing,readseq,seekrandom
out_has "--compact_before_reads says that it compacted" "# compacted"
json_has "--compact_before_reads is in the JSON" "$tmp/compacted.json" '
assert d["config"]["compact_before_reads"] is True
by = {r["name"]: r for r in d["results"]}
assert by["readrandom"]["compact_seconds"] is not None and by["readrandom"]["compact_seconds"] > 0
# Compacted once, before the first read benchmark, and not again before the next ones.
assert all(by[n]["compact_seconds"] is None for n in ("readmissing", "readseq", "seekrandom"))
'
cases=$((cases + 1))
if [ "$(digest "$tmp/plain.json")" != "$(digest "$tmp/compacted.json")" ]; then
  fail "compaction changed what the reads found" \
    "$(diff <(digest "$tmp/plain.json") <(digest "$tmp/compacted.json"))"
fi

# --- Every flag that is wrong is a usage error, and says which. ------------------------------------

check "no flags" 2
err_has "no flags" "--db=PATH is needed"
out_has_usage() { err_has "$1" "usage: shale_bench"; }
check "an unknown flag" 2 --db="$tmp/x" --nosuch=1
err_has "an unknown flag" "unknown flag --nosuch"
out_has_usage "an unknown flag"
check "a bare word" 2 --db="$tmp/x" fillseq
err_has "a bare word" "'fillseq' is not a flag"
check "an unknown benchmark" 2 --db="$tmp/x" --benchmarks=fillseq,nosuch
err_has "an unknown benchmark" "unknown benchmark 'nosuch'"
check "an empty benchmark list" 2 --db="$tmp/x" --benchmarks=
err_has "an empty benchmark list" "has an empty one"
check "a read benchmark with nothing written" 2 --db="$tmp/x" --benchmarks=readrandom
err_has "a read benchmark with nothing written" "readrandom needs a database to read"
check "a read benchmark before the first write" 2 --db="$tmp/x" --benchmarks=readseq,fillseq
err_has "a read benchmark before the first write" "readseq needs a database to read"
check "--num=0" 2 --db="$tmp/x" --num=0
err_has "--num=0" "--num is a whole number from 1"
check "--num is not a number" 2 --db="$tmp/x" --num=many
err_has "--num is not a number" "--num is a whole number"
check "--num too large" 2 --db="$tmp/x" --num=4294967296
err_has "--num too large" "--num is a whole number from 1 to 4294967295"
check "--repeat=0" 2 --db="$tmp/x" --repeat=0
err_has "--repeat=0" "--repeat is a whole number from 1"
check "--value_size=0" 2 --db="$tmp/x" --value_size=0
err_has "--value_size=0" "--value_size is a whole number from 1"
check "--sync_num=0" 2 --db="$tmp/x" --sync_num=0
err_has "--sync_num=0" "--sync_num is a whole number from 1"
check "--seed is not a number" 2 --db="$tmp/x" --seed=-1
err_has "--seed is not a number" "--seed is a whole number"
check "--db without a value" 2 --db
err_has "--db without a value" "--db needs a value"
check "--db empty" 2 --db=
err_has "--db empty" "--db needs a path"
check "a boolean flag with a value that is not one" 2 --db="$tmp/x" --compact_before_reads=maybe
err_has "a boolean flag with a value that is not one" "--compact_before_reads is true or false"
check "--help" 0 --help
out_has "--help" "usage: shale_bench"
# What a number means is in the usage, and these are the sentences that the numbers' readers
# depend on, so that they cannot drift away from the tool without a test going red.
out_has "--help says what overwrite is" "all --num keys are put in order into an empty database"
out_has "--help says what the default order is" \
  "The default order is fillseq, fillrandom, fillsync, overwrite, then the four reads."
out_has "--help says why fillsync is where it is" "fillsync where the reads still see the large database"
out_has "--help says what a read after fillsync reads" "placed after fillsync reads at most --sync_num keys"
out_has "--help defines the spread" "(fastest - slowest) / median of the runs' ops/s"
out_has "--help says that the repeats of a read are warm" "the median is a warm run"
out_has "--help says what the printed clock figure is" "smallest step it saw between two readings"
out_has "--help says that a fine clock's step is the cost of a read" "what one reading costs"
out_has "--help says that ops/s is wall time" "wall-clock time of its"
out_has "--help says where the time inside the engine is" "engine_seconds"
out_has "--help says that the database is named by its last component" "last component"
cases=$((cases + 1))
if [ -e "$tmp/x" ]; then
  fail "a usage error touched the database path" "$(ls -la "$tmp/x")"
fi

# --- A JSON file that cannot be written is known before the benchmarks run. -----------------------

check "--json into a directory that does not exist" 1 "${small[@]}" --repeat=1 --db="$tmp/db8" \
  --json="$tmp/no-such-dir/result.json" --benchmarks=fillseq
err_has "--json into a directory that does not exist" "$tmp/no-such-dir/result.json"
cases=$((cases + 1))
if [ -e "$tmp/db8" ]; then
  fail "--json that cannot be written: the benchmarks ran anyway" "$(ls -la "$tmp/db8")"
fi

# --- A run that fails says which benchmark and why, and leaves no results. ------------------------

: >"$tmp/a-file"
check "a database path under a file" 1 "${small[@]}" --repeat=1 --db="$tmp/a-file/db" \
  --json="$tmp/failed.json" --benchmarks=fillseq
err_has "a database path under a file" "fillseq failed"
cases=$((cases + 1))
if [ ! -e "$tmp/failed.json" ] || [ -s "$tmp/failed.json" ]; then
  fail "a failed run did not leave an empty --json file" "$(ls -la "$tmp/failed.json" 2>&1)"
fi

finish shale_bench_test
