#!/usr/bin/env bash
# scripts/preflight.sh: everything CI runs, locally, in CI's order.
#
# Run it before pushing. A stage that cannot run, because a tool is missing or has the wrong
# version, fails the whole script: a preflight that quietly checks half of CI produces the
# confidence without the coverage.
#
# What it does not run, which is a gap of this loop and not of what CI covers: the coverage job,
# which only reports, and needs the gcovr that the job pins; the compilers of the build-test matrix
# other than the Apple Clang and the GCC of this machine; and the fresh checkout of the clean-clone
# job. The stages here build the working tree, so a file that was never committed is not noticed
# by them. The steps of that job from the installation on do run, in the examples stage.
#
# Never pipe a stage into another command. With a pipe the exit status is the last command's, and
# a failing stage followed by `tail` reads as a success.
set -euo pipefail
cd "$(dirname "$0")/.."

# Keep these in step with the lint job in .github/workflows/ci.yml.
CLANG_FORMAT_VERSION=23.1.3
CLANG_TIDY_VERSION=22.1.8

# The fuzz targets need libFuzzer, which Apple's Clang does not ship, so they are built with the
# Clang from Homebrew's llvm. Set these for a Clang installed somewhere else.
FUZZ_CC="${FUZZ_CC:-/opt/homebrew/opt/llvm/bin/clang}"
FUZZ_CXX="${FUZZ_CXX:-/opt/homebrew/opt/llvm/bin/clang++}"
FUZZ_SECONDS=10

# This repository must be its own working tree. If it is a subdirectory of a larger one, every path
# and every push is pointing somewhere unintended, so fail before anything runs.
if [ "$(git rev-parse --show-toplevel)" != "$(pwd -P)" ]; then
  echo "preflight: $(pwd -P) is not the repository root ($(git rev-parse --show-toplevel))" >&2
  exit 1
fi

stage() {
  printf '\n==> %s\n' "$*"
}

# require_tool <tool> [<version>]: the tool must be on PATH and, when a version is given, report
# exactly that version.
require_tool() {
  local tool=$1 version=${2:-} actual
  if ! command -v "$tool" >/dev/null 2>&1; then
    if [ -n "$version" ]; then
      echo "preflight: $tool is not installed; run: uv tool install $tool==$version" >&2
    else
      echo "preflight: $tool is not installed; install it with your package manager" >&2
    fi
    exit 1
  fi
  [ -n "$version" ] || return 0
  # The output goes through a variable, not a pipe into grep -q: grep exits at the first match
  # and the tool then dies of SIGPIPE, which `pipefail` would report as a failure.
  actual="$("$tool" --version 2>&1)"
  case "$actual" in
    *"version $version"*) ;;
    *)
      echo "preflight: $tool must be version $version, found:" >&2
      echo "$actual" >&2
      echo "preflight: run: uv tool install --force $tool==$version" >&2
      exit 1
      ;;
  esac
}

# require_compiler <name>...: the GCC the release build uses must exist.
require_compiler() {
  local name
  for name in "$@"; do
    if ! command -v "$name" >/dev/null 2>&1; then
      echo "preflight: $name is not installed; the GCC release build needs it" >&2
      exit 1
    fi
  done
}

# build_and_test <preset>: configure, build and run the tests of one preset.
build_and_test() {
  stage "$1: configure, build, test"
  cmake --preset "$1"
  cmake --build --preset "$1"
  ctest --preset "$1"
}

# require_executable <path> <how to get it>: a compiler that is named by its path must be there.
require_executable() {
  if [ ! -x "$1" ]; then
    echo "preflight: $1 is not installed; $2" >&2
    exit 1
  fi
}

require_tool clang-format "$CLANG_FORMAT_VERSION"
require_tool clang-tidy "$CLANG_TIDY_VERSION"
require_tool doxygen
require_compiler gcc-16 g++-16
require_executable "$FUZZ_CXX" "run: brew install llvm, or set FUZZ_CC and FUZZ_CXX"

stage "clang-format"
git ls-files -z -- '*.h' '*.cc' | xargs -0 -r clang-format --dry-run --Werror

stage "layering"
bash scripts/check-layering.sh

stage "hygiene"
bash scripts/check-hygiene.sh

build_and_test dev
build_and_test asan

# The thread leg of the sanitize job of .github/workflows/ci.yml. Every test runs three times, since
# a race that does not happen on every run gets three chances to show. The simulator and its
# determinism test are single-threaded, so ThreadSanitizer has nothing to find in them: they run
# once, with fewer seeds, and make up most of what three runs would cost.
stage "tsan: configure, build, test three times"
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --repeat until-fail:3 -E '^(sim|determinism)_test$'
SHALE_SIM_SEEDS=20 ctest --preset tsan -R '^(sim|determinism)_test$'

stage "release with GCC"
CC=gcc-16 CXX=g++-16 cmake --preset release -B build/gcc
cmake --build build/gcc
ctest --test-dir build/gcc --output-on-failure --no-tests=error

# examples_against_install <build dir> <C++ compiler>: the clean-clone job of
# .github/workflows/ci.yml from the installation on. It installs the tree into a scratch prefix,
# builds examples/ against that prefix and nothing else, runs the program and compares what it
# prints with examples/basic.expected, then writes a key with shalectl and reads it back in a
# process of its own. The compiler must be the one that built the libraries: a static library is
# only usable by the standard library it was compiled against.
examples_against_install() {
  local build_dir=$1 cxx=$2 work got
  work="$(mktemp -d)"
  echo "scratch directory: $work (left in place if this stage fails)"
  cmake --install "$build_dir" --prefix "$work/prefix"
  # Nothing of src/ is installed: the headers in the prefix are the public ones and no more.
  diff <(git ls-files include | sort) <(cd "$work/prefix" && find include -type f | sort)
  CXX="$cxx" cmake -S examples -B "$work/build" -G Ninja -DCMAKE_PREFIX_PATH="$work/prefix"
  cmake --build "$work/build"
  "$work/build/basic" "$work/exdb" >"$work/basic.out"
  diff -u examples/basic.expected "$work/basic.out"
  # The package is the one of version 0.1: a project that asks for it gets it, and one that asks for
  # version 1 does not. The same project asks both times, so that the refusal can only be the
  # version's. That the mode is SameMajorVersion and not AnyNewerVersion, which refuses version 1
  # too, is pinned by tests/scripts/options_test.sh: an installation of 0.1 cannot show it.
  mkdir "$work/version"
  cat >"$work/version/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.25)
project(version_check LANGUAGES CXX)
find_package(shale ${REQUESTED} REQUIRED)
EOF
  CXX="$cxx" cmake -S "$work/version" -B "$work/version/build-0.1" -DREQUESTED=0.1 \
    -DCMAKE_PREFIX_PATH="$work/prefix"
  if CXX="$cxx" cmake -S "$work/version" -B "$work/version/build-1" -DREQUESTED=1 \
    -DCMAKE_PREFIX_PATH="$work/prefix" >/dev/null 2>&1; then
    echo "preflight: find_package(shale 1) was satisfied by the installed 0.1" >&2
    exit 1
  fi
  "$build_dir/tools/shalectl" --db="$work/db" --create put greeting hello
  got="$("$build_dir/tools/shalectl" --db="$work/db" get greeting)"
  if [ "$got" != hello ]; then
    echo "preflight: shalectl get greeting printed '$got', wanted 'hello'" >&2
    exit 1
  fi
  rm -rf "$work"
}

stage "examples against a local install"
examples_against_install build/gcc g++-16

# The crash job of .github/workflows/ci.yml, on the Release build above: the tests of that build ran
# only the three rounds of the smoke test. Twenty rounds here against the job's two hundred, to keep
# the preflight short. A failed run leaves its database in the directory that the tool names.
stage "crash test"
crash_dir="$(mktemp -d)"
build/gcc/tools/shale_crashtest --rounds=20 --seed=7 --min_compactions=1 --db="$crash_dir/db"
build/gcc/tools/shale_crashtest --rounds=6 --seed=7 --sync --db="$crash_dir/db-sync"
rm -rf "$crash_dir"

# The fuzz job of .github/workflows/ci.yml, at ten seconds a target and not sixty. A crash leaves its
# input in build/fuzz/crashes, and the target reproduces it when it is run with that file. What a run
# finds goes to a directory that starts empty, so that the run is the seed corpus and ten seconds, not
# also every earlier run: a target replays everything it is given before the time limit starts.
stage "fuzz"
CC="$FUZZ_CC" CXX="$FUZZ_CXX" cmake --preset fuzz
cmake --build --preset fuzz --target fuzz_targets
rm -rf build/fuzz/found
mkdir -p build/fuzz/crashes
# The targets are those of fuzz/fuzz_targets.txt, as in the job: the name, then what it links.
while read -r target _; do
  case "$target" in '' | '#'*) continue ;; esac
  mkdir -p "build/fuzz/found/$target"
  build/fuzz/fuzz/"$target" -max_total_time="$FUZZ_SECONDS" -timeout=60 \
    -artifact_prefix="build/fuzz/crashes/$target-" \
    "build/fuzz/found/$target" "build/fuzz/fuzz/corpus/$target" </dev/null
done < fuzz/fuzz_targets.txt

# The bench-smoke job of .github/workflows/ci.yml, which asserts completion only. It has a build tree
# of its own, because the micro benchmarks need Google Benchmark, which the other trees do not fetch.
stage "bench smoke"
cmake --preset release -B build/bench -DSHALE_BUILD_BENCH=ON -DSHALE_BUILD_TESTS=OFF
cmake --build build/bench --target shale_bench micro_bench
bench_dir="$(mktemp -d)"
build/bench/tools/shale_bench --num=20000 --sync_num=200 --repeat=1 --db="$bench_dir/db"
build/bench/bench/micro_bench --benchmark_min_time=0.01s
rm -rf "$bench_dir"

stage "clang-tidy"
# The standalone clang-tidy does not ask Xcode where the SDK is, so it cannot find <string>.
if [ "$(uname -s)" = Darwin ] && [ -z "${SDKROOT:-}" ]; then
  SDKROOT="$(xcrun --show-sdk-path)"
  export SDKROOT
fi
git ls-files -z -- 'src/*.cc' 'tools/*.cc' | xargs -0 -r clang-tidy -p build/dev --quiet

stage "docs"
cmake --build --preset dev --target docs

# The slowest stage, so the last: six Release builds of the simulator, each with one deliberate flaw
# compiled in, each of which must make it fail. The flaws job in .github/workflows/ci.yml runs the
# same script, one flaw to a job.
stage "injected flaws"
bash scripts/run-flaws.sh

echo
echo "preflight: ok"
