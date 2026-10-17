#!/usr/bin/env bash
# Tests for the build options in cmake/ShaleOptions.cmake: what each accepts, what it rejects, and
# that an accepted value reaches the compile commands. Every case configures with the tests turned
# off, so nothing is fetched and nothing is compiled.
#
# Usage: options_test.sh <cmake> <generator>
set -uo pipefail

cmake_cmd="${1:?usage: options_test.sh <cmake> <generator>}"
generator="${2:?usage: options_test.sh <cmake> <generator>}"
source_dir="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

build="$tmp/build"
failures=0
cases=0

# configure <flaw> <sanitize> <coverage> <fuzz>: configures from scratch settings on every call,
# so no option leaks from one case into the next. Leaves the output in $output and the exit status
# in $status.
configure() {
  output="$("$cmake_cmd" -S "$source_dir" -B "$build" -G "$generator" \
    -DSHALE_BUILD_TESTS=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "-DSHALE_FLAW=$1" "-DSHALE_SANITIZE=$2" "-DSHALE_COVERAGE=$3" "-DSHALE_FUZZ=$4" 2>&1)"
  status=$?
}

fail() {
  echo "FAIL: $1" >&2
  printf '%s\n' "$output" | sed 's/^/    | /' >&2
  failures=$((failures + 1))
}

# accepts <description> <flaw> <sanitize> <coverage> <fuzz> [compile-command fragment...]
accepts() {
  local description=$1 fragment
  shift
  cases=$((cases + 1))
  configure "$1" "$2" "$3" "$4"
  shift 4
  if [ "$status" -ne 0 ]; then
    fail "$description: configure failed with status $status"
    return
  fi
  for fragment in "$@"; do
    if ! grep -qF -- "$fragment" "$build/compile_commands.json"; then
      fail "$description: compile commands lack '$fragment'"
    fi
  done
}

# rejects <description> <flaw> <sanitize> <coverage> <fuzz> <fragment of the error message>
rejects() {
  local description=$1 want=$6
  shift
  cases=$((cases + 1))
  configure "$1" "$2" "$3" "$4"
  if [ "$status" -eq 0 ]; then
    fail "$description: configure succeeded"
  elif [[ $output != *"$want"* ]]; then
    fail "$description: error does not mention '$want'"
  fi
}

# leaves_out <description> <fragment>: the last accepted configure must not mention it.
leaves_out() {
  cases=$((cases + 1))
  if grep -qF -- "$2" "$build/compile_commands.json"; then
    fail "$1: compile commands contain '$2'"
  fi
}

accepts "the defaults" "" "" OFF OFF "-Wold-style-cast" "-Werror"
leaves_out "the defaults add no flaw" "SHALE_FLAW_"
leaves_out "the defaults add no sanitizer" "-fsanitize"
leaves_out "the defaults add no coverage" "--coverage"

for flaw in skip_wal_sync skip_dir_sync drop_tombstones_early ignore_snapshots_in_compaction \
  skip_manifest_sync delete_live_files; do
  upper="$(printf '%s' "$flaw" | tr '[:lower:]' '[:upper:]')"
  accepts "flaw $flaw" "$flaw" "" OFF OFF "-DSHALE_FLAW_$upper=1"
done

rejects "an unknown flaw" "no_such_flaw" "" OFF OFF "SHALE_FLAW"
rejects "a flaw spelled in upper case" "SKIP_WAL_SYNC" "" OFF OFF "SHALE_FLAW"

accepts "address,undefined sanitizers" "" "address,undefined" OFF OFF \
  "-fsanitize=address,undefined" "-fno-sanitize-recover=all"
accepts "thread sanitizer" "" "thread" OFF OFF "-fsanitize=thread"
leaves_out "thread sanitizer does not recover-disable" "-fno-sanitize-recover"
rejects "an unknown sanitizer" "" "memory" OFF OFF "SHALE_SANITIZE"
rejects "address without undefined" "" "address" OFF OFF "SHALE_SANITIZE"

accepts "coverage" "" "" ON OFF "--coverage"

# SHALE_FUZZ needs Clang, and which compiler this is depends on the machine running the test.
compiler_id="$(sed -n 's/^set(CMAKE_CXX_COMPILER_ID "\(.*\)")$/\1/p' \
  "$build"/CMakeFiles/*/CMakeCXXCompiler.cmake | head -n 1)"
if [ "$compiler_id" = Clang ]; then
  accepts "fuzzing with Clang" "" "" OFF ON
else
  rejects "fuzzing with $compiler_id" "" "" OFF ON "Clang"
fi

echo "options_test: $((cases - failures))/$cases cases behaved as expected"
[ "$failures" -eq 0 ]
