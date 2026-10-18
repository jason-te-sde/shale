#!/usr/bin/env bash
# scripts/run-flaws.sh [flaw...]: proves that the simulator can go red.
#
# A test suite that has only ever passed has not shown that it can fail. The engine has six
# deliberate flaws, each one `#if defined(SHALE_FLAW_<NAME>)` branch in src/ that is compiled in
# only by -DSHALE_FLAW=<name> (cmake/ShaleOptions.cmake lists the names). For each flaw named on the
# command line, or for all of them with no arguments, this builds the simulator with that flaw in
# build/flaw-<name>, a Release build, and runs it over 100 seeds. The flaw is caught if sim_test
# exits non-zero; a simulator that passes with a flaw in the engine has a hole in it.
#
# Prints one line per flaw:
#   flaw <name>: caught by I<n>     the first invariant id in the output, "I1:" to "I6:"
#   flaw <name>: NOT CAUGHT         sim_test passed on a database with the flaw in it
# A flaw that did not build, or whose sim_test could not be run, prints BUILD FAILED or COULD NOT
# RUN instead and counts as not caught. The output of each sim_test run is kept in
# build/flaw-<name>/sim-output.log. When GITHUB_STEP_SUMMARY is set, a table of what caught each
# flaw is appended to it.
#
# Exit status: 0 every flaw was caught, 1 some flaw was not, 2 a usage error.
set -uo pipefail
cd "$(dirname "$0")/.."

# How many seeds each flaw is run over. The flaws are caught within the first seeds or two; the rest
# are for a flaw that a change to the simulator would make harder to see.
seeds=100

# Every name in SHALE_FLAW_NAMES, one per line, read from the one place that defines them.
all_flaws() {
  awk '
    /^set\(SHALE_FLAW_NAMES/ { inside = 1; next }
    inside {
      line = $0
      done = sub(/\).*/, "", line)
      gsub(/[[:space:]]/, "", line)
      if (line != "") print line
      if (done) exit
    }' cmake/ShaleOptions.cmake
}

# What an invariant id stands for, in the words of the table of invariants in the simulator.
describe_invariant() {
  case "$1" in
    I1) echo "a read disagrees with the reference model" ;;
    I2) echo "the version is unsound, the sequence number went back, or a live table is missing" ;;
    I3) echo "a table is left in the directory that no live version names" ;;
    I4) echo "a reopened database does not hold what a crash may leave" ;;
    I5) echo "an error that no injected fault explains" ;;
    I6) echo "a write succeeded after the database stopped writing" ;;
    *) echo "no invariant of the simulator" ;;
  esac
}

known=()
while IFS= read -r name; do
  known+=("$name")
done < <(all_flaws)
if [ "${#known[@]}" -eq 0 ]; then
  echo "run-flaws: found no flaw names in cmake/ShaleOptions.cmake" >&2
  exit 2
fi

flaws=("$@")
if [ "${#flaws[@]}" -eq 0 ]; then
  flaws=("${known[@]}")
fi
for flaw in "${flaws[@]}"; do
  valid=0
  for name in "${known[@]}"; do
    [ "$flaw" = "$name" ] && valid=1
  done
  if [ "$valid" -eq 0 ]; then
    echo "run-flaws: '$flaw' is not a flaw; the flaws are: ${known[*]}" >&2
    echo "usage: scripts/run-flaws.sh [flaw...]" >&2
    exit 2
  fi
done

summary() {
  [ -n "${GITHUB_STEP_SUMMARY:-}" ] || return 0
  printf '%s\n' "$*" >>"$GITHUB_STEP_SUMMARY"
}

summary "### Injected flaws"
summary ""
summary "| flaw | result | what caught it |"
summary "| --- | --- | --- |"

# run_flaw <flaw>: builds and runs the simulator with the flaw in. Returns 0 if it was caught.
run_flaw() {
  local flaw=$1 dir="build/flaw-$1" log status id
  log="$dir/sim-output.log"
  rm -f "$log"

  if ! cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DSHALE_FLAW=$flaw" ||
    ! cmake --build "$dir" --target sim_test; then
    echo "flaw $flaw: BUILD FAILED"
    summary "| $flaw | BUILD FAILED | |"
    return 1
  fi

  # The variables that narrow a run to fewer seeds or a different shape are the user's to set when
  # replaying a failure, and here they would let a flaw through unseen.
  env -u SHALE_SIM_SEED -u SHALE_SIM_STEPS -u SHALE_SIM_KEYS \
    "SHALE_SIM_SEEDS=$seeds" "$dir/tests/sim_test" >"$log" 2>&1
  status=$?
  cat "$log"

  # 126 and 127 are the shell's: the program was not executable, or not there.
  if [ "$status" -eq 126 ] || [ "$status" -eq 127 ]; then
    echo "flaw $flaw: COULD NOT RUN $dir/tests/sim_test (status $status)"
    summary "| $flaw | COULD NOT RUN | |"
    return 1
  fi
  if [ "$status" -eq 0 ]; then
    echo "flaw $flaw: NOT CAUGHT"
    summary "| $flaw | NOT CAUGHT | |"
    return 1
  fi

  id="$(grep -m 1 -oE 'I[0-9]+:' "$log")"
  id="${id%:}"
  if [ -n "$id" ]; then
    echo "flaw $flaw: caught by $id"
    summary "| $flaw | caught | $id: $(describe_invariant "$id") |"
  else
    # sim_test failed, but not with an invariant's alarm: an abort in the engine's own checks, or a
    # floor on what the simulator provokes. It is red, which is what is asked, and it is said.
    echo "flaw $flaw: caught by no invariant id (sim_test exited with status $status)"
    summary "| $flaw | caught | no invariant id; sim_test exited with status $status |"
  fi
  return 0
}

failed=0
for flaw in "${flaws[@]}"; do
  run_flaw "$flaw" || failed=1
done
exit "$failed"
