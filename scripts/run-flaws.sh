#!/usr/bin/env bash
# scripts/run-flaws.sh [flaw...]: proves that the simulator can go red.
#
# A test suite that has only ever passed has not shown that it can fail. The engine has six
# deliberate flaws, each one `#if defined(SHALE_FLAW_<NAME>)` branch in src/ that is compiled in
# only by -DSHALE_FLAW=<name> (cmake/ShaleOptions.cmake lists the names). For each flaw named on the
# command line, or for all of them with no arguments, this builds the simulator with that flaw in
# build/flaw-<name>, a Release build, and runs the seeded invariant test over 100 seeds.
#
# What it runs is not all of sim_test but SimulationTest.RandomSeedsHoldAllInvariants alone, the
# test that runs the seeds. The exit status of the whole binary cannot tell an invariant's alarm
# from a crash, an abort, an out-of-memory kill, a failure to start, a floor on what the simulator
# provokes or a failing double, and all of those are red without showing that a flaw was seen. So a
# flaw is caught only if that one test failed and its output carries an invariant's id ("I1:" to
# "I6:"); a run that fails any other way is reported as such and counts as not caught.
#
# Prints one line per flaw:
#   flaw <name>: caught by I<n>                   the first invariant id in the output
#   flaw <name>: NOT CAUGHT                       the test passed on a database with the flaw in it
#   flaw <name>: FAILED WITHOUT AN INVARIANT (status N)
#                                                 the run failed, with no invariant to show for it
#   flaw <name>: BUILD FAILED                     the simulator did not build
#   flaw <name>: COULD NOT RUN ...                the binary was not run, or the test did not start
# The output of each run is kept in build/flaw-<name>/sim-output.log. When GITHUB_STEP_SUMMARY is
# set, a table of what caught each flaw is appended to it.
#
# scripts/run-flaws.sh --list prints the names of the flaws, one to a line, and nothing else.
#
# Exit status: 0 every flaw was caught, 1 some flaw was not, 2 a usage error.

# No -e: the status that is expected of the simulator is a non-zero one, and every status is read.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

# How many seeds each flaw is run over. The flaws are caught within the first seeds or two; the rest
# are for a flaw that a change to the simulator would make harder to see.
seeds=100

# The one test that runs the seeds, and so the one that has to fail.
seeded_test=SimulationTest.RandomSeedsHoldAllInvariants

# Every name in SHALE_FLAW_NAMES, one per line, read from the one place that defines them. The list
# is the arguments of a set(), which may open on the line of the set() or the line after, hold
# several names to a line and comments, and close on the last name or on a line of its own.
all_flaws() {
  awk '
    /^[[:space:]]*set\([[:space:]]*SHALE_FLAW_NAMES([[:space:]]|\)|#|$)/ {
      inside = 1
      sub(/^[[:space:]]*set\([[:space:]]*SHALE_FLAW_NAMES/, "")
    }
    inside {
      line = $0
      sub(/#.*/, "", line)
      closed = sub(/\).*/, "", line)
      count = split(line, words, /[[:space:]]+/)
      for (i = 1; i <= count; i++) {
        if (words[i] != "") print words[i]
      }
      if (closed) exit
    }' cmake/ShaleOptions.cmake
}

# describe_invariant <id>: what the invariant is, in the words of the simulator's own table of them.
# Prints nothing and fails for an id that this script has no words for.
describe_invariant() {
  case "$1" in
    I1) echo "a read disagrees with the reference model" ;;
    I2) echo "the version is unsound, the sequence number went back, or a live table is missing" ;;
    I3) echo "a table is left in the directory that no live version names" ;;
    I4) echo "a reopened database does not hold what a crash may leave" ;;
    I5) echo "an error that no injected fault explains" ;;
    I6) echo "a write succeeded after the database stopped writing" ;;
    *) return 1 ;;
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

if [ "$#" -eq 1 ] && [ "$1" = "--list" ]; then
  printf '%s\n' "${known[@]}"
  exit 0
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
    echo "usage: scripts/run-flaws.sh [--list | flaw...]" >&2
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

# invariant_id <log>: the first invariant id in the log, such as I4, or nothing. An id starts the
# text or follows a character that is not a letter or a digit, as the checkers' own rule has it
# (Classify() in tests/testkit/invariants.cc), so that part of a longer word is not one.
invariant_id() {
  local found
  found="$(grep -oE '(^|[^[:alnum:]])I[0-9]+:' "$1" | head -n 1)"
  [[ $found =~ (I[0-9]+): ]] && echo "${BASH_REMATCH[1]}"
}

# run_flaw <flaw>: builds and runs the simulator with the flaw in. Returns 0 if it was caught.
run_flaw() {
  local flaw=$1 dir="build/flaw-$1" log status id description
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
    "SHALE_SIM_SEEDS=$seeds" "$dir/tests/sim_test" "--gtest_filter=$seeded_test" >"$log" 2>&1
  status=$?
  cat "$log"

  # 126 and 127 are the shell's: the program was not executable, or not there. And a run in which
  # the test never started, a filter that matches nothing, shows nothing about the flaw.
  if [ "$status" -eq 126 ] || [ "$status" -eq 127 ] ||
    ! grep -qF "[ RUN      ] $seeded_test" "$log"; then
    echo "flaw $flaw: COULD NOT RUN $seeded_test in $dir/tests/sim_test (status $status)"
    summary "| $flaw | COULD NOT RUN | status $status |"
    return 1
  fi
  if [ "$status" -eq 0 ]; then
    echo "flaw $flaw: NOT CAUGHT"
    summary "| $flaw | NOT CAUGHT | |"
    return 1
  fi

  # The test has to have reported its own failure, not merely taken the process down, and the
  # failure has to be an invariant's.
  id=""
  if grep -qF "[  FAILED  ] $seeded_test" "$log"; then
    id="$(invariant_id "$log")"
  fi
  if [ -z "$id" ]; then
    echo "flaw $flaw: FAILED WITHOUT AN INVARIANT (status $status)"
    summary "| $flaw | FAILED WITHOUT AN INVARIANT | status $status |"
    return 1
  fi
  echo "flaw $flaw: caught by $id"
  if description="$(describe_invariant "$id")"; then
    summary "| $flaw | caught | $id: $description |"
  else
    summary "| $flaw | caught | $id |"
  fi
  return 0
}

failed=0
for flaw in "${flaws[@]}"; do
  run_flaw "$flaw" || failed=1
done
exit "$failed"
