#!/usr/bin/env bash
# scripts/preflight.sh: everything CI runs, locally, in CI's order.
#
# Run it before pushing. A stage that cannot run, because a tool is missing or has the wrong
# version, fails the whole script: a preflight that quietly checks half of CI produces the
# confidence without the coverage.
#
# Never pipe a stage into another command. With a pipe the exit status is the last command's, and
# a failing stage followed by `tail` reads as a success.
set -euo pipefail
cd "$(dirname "$0")/.."

# Keep these in step with the lint job in .github/workflows/ci.yml.
CLANG_FORMAT_VERSION=23.1.3
CLANG_TIDY_VERSION=22.1.8

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

require_tool clang-format "$CLANG_FORMAT_VERSION"
require_tool clang-tidy "$CLANG_TIDY_VERSION"
require_tool doxygen
require_compiler gcc-16 g++-16

stage "clang-format"
git ls-files -z -- '*.h' '*.cc' | xargs -0 -r clang-format --dry-run --Werror

stage "layering"
bash scripts/check-layering.sh

stage "hygiene"
bash scripts/check-hygiene.sh

build_and_test dev
build_and_test asan
build_and_test tsan

stage "release with GCC"
CC=gcc-16 CXX=g++-16 cmake --preset release -B build/gcc
cmake --build build/gcc
ctest --test-dir build/gcc --output-on-failure --no-tests=error

# The crash job of .github/workflows/ci.yml, on the Release build above: the tests of that build ran
# only the three rounds of the smoke test. A failed run leaves its database in the directory that
# the tool names.
stage "crash test"
crash_dir="$(mktemp -d)"
build/gcc/tools/shale_crashtest --rounds=20 --seed=7 --min_compactions=1 --db="$crash_dir/db"
build/gcc/tools/shale_crashtest --rounds=6 --seed=7 --sync --db="$crash_dir/db-sync"
rm -rf "$crash_dir"

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
