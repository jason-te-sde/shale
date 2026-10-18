#!/usr/bin/env bash
# Tests for the build options in cmake/ShaleOptions.cmake: what each accepts, what it rejects, and
# that an accepted value reaches the compile commands. Every case configures with the tests turned
# off, so nothing is fetched and nothing is compiled.
#
# Usage: options_test.sh <cmake> <generator> <c++ compiler>
#
# The compiler is the one the build under test was configured with. SHALE_FUZZ accepts or rejects a
# compiler, so the cases must run with that one and not with whatever CXX the environment holds.
# Only the C++ compiler is passed: the project enables no other language, and CMake warns about a
# C compiler it never uses.
set -uo pipefail

usage="usage: options_test.sh <cmake> <generator> <c++ compiler>"
cmake_cmd="${1:?$usage}"
generator="${2:?$usage}"
cxx_compiler="${3:?$usage}"
here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/scripts/lib.sh
. "$here/lib.sh"

source_dir="$(cd "$here/../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

build="$tmp/build"

# configure <flaw> <sanitize> <coverage> <fuzz>: configures from scratch settings on every call,
# so no option leaks from one case into the next. Leaves the output in $output and the exit status
# in $status.
configure() {
  output="$("$cmake_cmd" -S "$source_dir" -B "$build" -G "$generator" \
    "-DCMAKE_CXX_COMPILER=$cxx_compiler" -DSHALE_BUILD_TESTS=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "-DSHALE_FLAW=$1" "-DSHALE_SANITIZE=$2" "-DSHALE_COVERAGE=$3" "-DSHALE_FUZZ=$4" 2>&1)"
  status=$?
}

# accepts <description> <flaw> <sanitize> <coverage> <fuzz> [compile-command fragment...]
accepts() {
  local description=$1 fragment
  shift
  cases=$((cases + 1))
  configure "$1" "$2" "$3" "$4"
  shift 4
  if [ "$status" -ne 0 ]; then
    fail "$description: configure failed with status $status" "$output"
    return
  fi
  for fragment in "$@"; do
    if ! grep -qF -- "$fragment" "$build/compile_commands.json"; then
      fail "$description: compile commands lack '$fragment'"
    fi
  done
}

# flatten: CMake wraps long messages at 80 columns, so a phrase can span two lines. This joins the
# lines and collapses the runs of spaces that the indentation leaves.
flatten() { tr '\n' ' ' | tr -s ' '; }

# rejects <description> <flaw> <sanitize> <coverage> <fuzz> <fragment of the error message>
rejects() {
  local description=$1 want=$6
  shift
  cases=$((cases + 1))
  configure "$1" "$2" "$3" "$4"
  if [ "$status" -eq 0 ]; then
    fail "$description: configure succeeded" "$output"
  elif [[ $(flatten <<<"$output") != *"$want"* ]]; then
    fail "$description: error does not mention '$want'" "$output"
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

# The configure above must have used the compiler it was told to use.
cases=$((cases + 1))
used="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$build/CMakeCache.txt")"
if [ "$used" != "$cxx_compiler" ]; then
  fail "the compiler under test: the configure used '$used', not '$cxx_compiler'"
fi
cases=$((cases + 1))
help_text="$(sed -n '/^\/\/Compile in one deliberate flaw/,/^SHALE_FLAW:/p' "$build/CMakeCache.txt" |
  sed 's#^//##' | flatten)"
if [[ $help_text != *"empty or one of skip_wal_sync, skip_dir_sync, drop_tombstones_early"* ]]; then
  fail "the SHALE_FLAW help text lists the valid names with commas" "$help_text"
fi
leaves_out "the defaults add no flaw" "SHALE_FLAW_"
leaves_out "the defaults add no sanitizer" "-fsanitize"
leaves_out "the defaults add no coverage" "--coverage"
leaves_out "the defaults add no atomic profile update" "-fprofile-update"

# The warning flags of every target are those of examples/CMakeLists.txt. The example has to build
# with nothing of the repository but its own directory, so it repeats the list instead of including
# it, and this is what keeps the copy from falling behind. -Wthread-safety is left out of both: only
# Clang takes it, so the compile commands of another compiler lack it, and the example names it in
# a generator expression.
warning_flags() {
  grep -oE '(^|[[:space:]])-W[a-z][a-z-]*' "$1" | sed 's/^[[:space:]]*//' | sort -u |
    grep -v -x -e '-Wthread-safety'
}
cases=$((cases + 1))
build_flags="$(warning_flags "$build/compile_commands.json")"
example_flags="$(warning_flags "$source_dir/examples/CMakeLists.txt")"
if [ -z "$build_flags" ] || [ "$build_flags" != "$example_flags" ]; then
  fail "the example's warning flags are those of every other target" \
    "$(printf 'compile commands:\n%s\nexamples/CMakeLists.txt:\n%s' "$build_flags" "$example_flags")"
fi

# The package asks for SameMajorVersion: of the releases that are not older than a request, only
# those of its major version answer it. An installed 2.3.0 is newer than a request for 0.1 and is no
# answer to it, and it is one to a request for 2. The version file is made at configure time, so this
# runs it as find_package does, from a copy that says 2.3.0 where the file says the version of this
# tree. An installation of this tree is 0.x, and a check against it alone could not tell this mode
# from AnyNewerVersion, which refuses a request for 1 as well.
cases=$((cases + 1))
version_text="$(<"$build/shaleConfigVersion.cmake")"
this_version="$(sed -n 's/^set(PACKAGE_VERSION "\(.*\)")$/\1/p' <<<"$version_text")"
if [ -z "$this_version" ]; then
  fail "the version file names the version of this tree" "$version_text"
else
  mkdir -p "$tmp/fake" "$tmp/consumer"
  printf '%s\n' "${version_text//"$this_version"/2.3.0}" >"$tmp/fake/shaleConfigVersion.cmake"
  : >"$tmp/fake/shaleConfig.cmake"
  printf '%s\n' 'cmake_minimum_required(VERSION 3.25)' 'project(consumer LANGUAGES NONE)' \
    'find_package(shale ${REQUEST} QUIET CONFIG PATHS "${FAKE}" NO_DEFAULT_PATH)' \
    'if(shale_FOUND)' '  message(STATUS "answer=yes")' 'else()' '  message(STATUS "answer=no")' \
    'endif()' >"$tmp/consumer/CMakeLists.txt"
  # answer <requested version> <yes or no>: whether find_package takes the 2.3.0 for the request.
  answer() {
    local got
    cases=$((cases + 1))
    got="$("$cmake_cmd" -S "$tmp/consumer" -B "$tmp/consumer/build-$1" -DREQUEST="$1" \
      -DFAKE="$tmp/fake" 2>&1 | sed -n 's/^-- answer=//p')"
    if [ "$got" != "$2" ]; then
      fail "a request for $1 of an installed 2.3.0: answer '$got', wanted '$2'"
    fi
  }
  answer 0.1 no
  answer 2 yes
  answer 3 no
fi

for flaw in skip_wal_sync skip_dir_sync drop_tombstones_early ignore_snapshots_in_compaction \
  skip_manifest_sync delete_live_files; do
  upper="$(printf '%s' "$flaw" | tr '[:lower:]' '[:upper:]')"
  accepts "flaw $flaw" "$flaw" "" OFF OFF "-DSHALE_FLAW_$upper=1"
done

rejects "an unknown flaw" "no_such_flaw" "" OFF OFF "SHALE_FLAW"
rejects "an unknown flaw lists the valid names with commas" "no_such_flaw" "" OFF OFF \
  "skip_wal_sync, skip_dir_sync, drop_tombstones_early"
rejects "a flaw spelled in upper case" "SKIP_WAL_SYNC" "" OFF OFF "SHALE_FLAW"

accepts "address,undefined sanitizers" "" "address,undefined" OFF OFF \
  "-fsanitize=address,undefined" "-fno-sanitize-recover=all"
accepts "thread sanitizer" "" "thread" OFF OFF "-fsanitize=thread"
leaves_out "thread sanitizer does not recover-disable" "-fno-sanitize-recover"
rejects "an unknown sanitizer" "" "memory" OFF OFF "SHALE_SANITIZE"
rejects "address without undefined" "" "address" OFF OFF "SHALE_SANITIZE"

# The counters have to be updated atomically: the tests run threads, and with plain counters gcovr
# stopped on a negative count of hits and wrote no report.
accepts "coverage" "" "" ON OFF "--coverage" "-fprofile-update=atomic"

# SHALE_FUZZ needs Clang, and which compiler this is depends on the machine running the test.
compiler_id="$(sed -n 's/^set(CMAKE_CXX_COMPILER_ID "\(.*\)")$/\1/p' \
  "$build"/CMakeFiles/*/CMakeCXXCompiler.cmake | head -n 1)"
if [ "$compiler_id" = Clang ]; then
  # Fuzzing needs AddressSanitizer and the tests' subdirectory (fuzz_db_ops links the test kit,
  # which needs GoogleTest), and every case here turns the tests off so that nothing is fetched. So
  # these cases show that each check is there, in that order, and the fuzz job of the CI and the
  # preflight's fuzz stage configure and build the real thing.
  rejects "fuzzing without sanitizers" "" "" OFF ON "SHALE_SANITIZE=address,undefined"
  rejects "fuzzing with the thread sanitizer" "" "thread" OFF ON "SHALE_SANITIZE=address,undefined"
  rejects "fuzzing without the tests" "" "address,undefined" OFF ON "SHALE_BUILD_TESTS=ON"
else
  rejects "fuzzing with $compiler_id" "" "" OFF ON "Clang"
fi

finish options_test
