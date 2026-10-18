#!/usr/bin/env bash
# Tests for scripts/run-flaws.sh: that the flaws CI runs are the flaws the build defines, that the
# names are read from cmake/ShaleOptions.cmake whatever the layout of the list, and that the
# script's verdicts mean what they say.
#
# The last matters most. A script that says "caught" must also be seen to say something else, and a
# crash, an abort or a failed floor is not a catch: only the seeded invariant run failing with an
# invariant's id is. So the verdict cases run the real script, in a fixture tree, with a fake cmake
# first on PATH that "builds" a fake simulator, and the fake simulator does what each case needs.
#
# Usage: flaws_test.sh
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/scripts/lib.sh
. "$here/lib.sh"

source_dir="$(cd "$here/../.." && pwd)"
script="$source_dir/scripts/run-flaws.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# --- The fakes. -----------------------------------------------------------------------------

mkdir "$tmp/bin"

# The fake cmake: a configure creates the build directory, a build puts the fake simulator in it,
# and every call is written to $FAKE_LOG, so that a case can see that nothing was built.
cat >"$tmp/bin/cmake" <<'EOF'
#!/usr/bin/env bash
echo "$*" >>"$FAKE_LOG"
case "$1" in
  -S) mkdir -p "$4" ;;
  --build)
    if [ -n "${FAKE_BUILD_FAILS:-}" ]; then
      echo "fake cmake: the build fails" >&2
      exit 1
    fi
    mkdir -p "$2/tests"
    cp "$FAKE_SIM" "$2/tests/sim_test"
    chmod +x "$2/tests/sim_test"
    ;;
esac
EOF
chmod +x "$tmp/bin/cmake"

# The fake simulator. It first insists on being run the way the script is to run it: the seeded
# invariant test alone, over 100 seeds, with nothing in the environment that narrows the run.
cat >"$tmp/fake_sim_test" <<'EOF'
#!/usr/bin/env bash
run="[ RUN      ] SimulationTest.RandomSeedsHoldAllInvariants"
failed="[  FAILED  ] SimulationTest.RandomSeedsHoldAllInvariants"
if [ "$*" != "--gtest_filter=SimulationTest.RandomSeedsHoldAllInvariants" ] ||
  [ "${SHALE_SIM_SEEDS:-}" != 100 ] || [ -n "${SHALE_SIM_SEED+x}" ] ||
  [ -n "${SHALE_SIM_STEPS+x}" ] || [ -n "${SHALE_SIM_KEYS+x}" ]; then
  echo "fake sim_test: wrong arguments or environment: '$*' seeds='${SHALE_SIM_SEEDS:-}'"
  exit 99
fi
case "$FAKE_SCENARIO" in
  passes)
    echo "$run"
    echo "[       OK ] SimulationTest.RandomSeedsHoldAllInvariants (10 ms)"
    ;;
  invariant)
    echo "$run"
    echo "Expected: true"
    echo "I4: the recovered state matches no prefix of the writes"
    echo "$failed"
    exit 1
    ;;
  invariant-mid-line)
    echo "$run"
    echo "Expected: true"
    echo "step 12: (I6: a write succeeded after the database stopped writing)"
    echo "$failed"
    exit 1
    ;;
  floor)
    echo "$run"
    echo "Expected: total.flushes >= floor_of(kFloors.flushes)"
    echo "$failed"
    exit 1
    ;;
  abort)
    echo "$run"
    echo "check failed: a table was begun while the obsolete files were swept"
    exit 134
    ;;
  id-and-abort)
    echo "$run"
    echo "I2: last_sequence went back from 9 to 3"
    exit 134
    ;;
  id-inside-a-word)
    echo "$run"
    echo "ABI2: not an invariant's id"
    echo "$failed"
    exit 1
    ;;
  not-run)
    echo "Running main() from gtest_main.cc"
    echo "[==========] 0 tests ran."
    ;;
  *)
    echo "fake sim_test: no scenario '$FAKE_SCENARIO'"
    exit 98
    ;;
esac
EOF
export FAKE_SIM="$tmp/fake_sim_test"
export FAKE_LOG="$tmp/fake.log"

# --- Fixture trees. -------------------------------------------------------------------------

tree_id=0

# new_tree: a tree holding a copy of the script and the cmake list on stdin, in $tree.
new_tree() {
  tree_id=$((tree_id + 1))
  tree="$tmp/tree$tree_id"
  mkdir -p "$tree/scripts" "$tree/cmake"
  cp "$script" "$tree/scripts/run-flaws.sh"
  cat >"$tree/cmake/ShaleOptions.cmake"
  : >"$FAKE_LOG"
}

# run_script <tree> <argument...>: runs the tree's copy of the script with the fake cmake in front.
# Leaves its output in $output and its exit status in $status.
run_script() {
  local root=$1
  shift
  output="$(cd "$root" && PATH="$tmp/bin:$PATH" bash "$root/scripts/run-flaws.sh" "$@" 2>&1)"
  status=$?
}

# --- The list of names, read from cmake. ----------------------------------------------------

# lists <description> <name...>: --list in $tree prints exactly these names, in this order.
lists() {
  local description=$1 want
  shift
  want="$(printf '%s\n' "$@")"
  cases=$((cases + 1))
  run_script "$tree" --list
  if [ "$status" -ne 0 ]; then
    fail "$description: --list exited with status $status" "$output"
  elif [ "$output" != "$want" ]; then
    fail "$description: --list printed the wrong names" "$output"
  fi
}

new_tree <<'EOF'
set(SHALE_FLAW_NAMES
  alpha
  beta
  gamma)
EOF
lists "one name to a line" alpha beta gamma

new_tree <<'EOF'
set(SHALE_FLAW_NAMES alpha beta
  gamma)
EOF
lists "names on the line of the set" alpha beta gamma

new_tree <<'EOF'
set(SHALE_FLAW_NAMES alpha beta gamma)
EOF
lists "all the names on one line" alpha beta gamma

new_tree <<'EOF'
# Not this one: set(SHALE_FLAW_NAMES nothing)
set(SHALE_FLAW_NAMES  # the flaws
  alpha  # the first
  beta
)
list(JOIN SHALE_FLAW_NAMES ", " SHALE_FLAW_NAMES_TEXT)
set(SHALE_FLAW_NAMES_TEXT_EXTRA not_a_flaw)
set(OTHER not_a_flaw_either)
EOF
lists "comments, a closing parenthesis of its own and other lists after it" alpha beta

new_tree <<'EOF'
set(SOMETHING_ELSE alpha)
EOF
cases=$((cases + 1))
run_script "$tree" --list
if [ "$status" -ne 2 ] || [[ $output != *"found no flaw names"* ]]; then
  fail "a cmake file with no list: want exit 2 and 'found no flaw names', got $status" "$output"
fi

# --- The list against the CI job's matrix. --------------------------------------------------

# matrix_names: the names under the matrix of the flaws job in ci.yml, one to a line.
matrix_names() {
  awk '
    /^  flaws:/ { in_job = 1; next }
    in_job && /^  [^ #]/ { exit }
    in_job && /^ +flaw:[[:space:]]*$/ { in_list = 1; next }
    in_list && /^[[:space:]]*(#.*)?$/ { next }
    in_list && /^ +- / {
      sub(/^ +- /, "")
      sub(/[[:space:]]*(#.*)?$/, "")
      print
      next
    }
    in_list { exit }' "$source_dir/.github/workflows/ci.yml"
}

cases=$((cases + 1))
in_cmake="$(bash "$script" --list | sort)"
in_matrix="$(matrix_names | sort)"
if [ -z "$in_cmake" ]; then
  fail "SHALE_FLAW_NAMES: the script found no names in cmake/ShaleOptions.cmake"
elif [ -z "$in_matrix" ]; then
  fail "the flaws job in ci.yml: found no matrix of flaw names, so nothing was compared"
elif [ "$in_cmake" != "$in_matrix" ]; then
  # A name only in cmake is a flaw that CI never runs.
  only_in_cmake="$(comm -23 <(printf '%s\n' "$in_cmake") <(printf '%s\n' "$in_matrix"))"
  only_in_matrix="$(comm -13 <(printf '%s\n' "$in_cmake") <(printf '%s\n' "$in_matrix"))"
  fail "the flaws matrix in ci.yml and SHALE_FLAW_NAMES in cmake differ" \
    "only in cmake, so never run in CI: ${only_in_cmake:-nothing}
only in ci.yml, so not a flaw: ${only_in_matrix:-nothing}"
fi

# --- Names the script is given. -------------------------------------------------------------

new_tree <<'EOF'
set(SHALE_FLAW_NAMES
  flaw_one
  flaw_two)
EOF
run_script "$tree" no_such_flaw
cases=$((cases + 1))
if [ "$status" -ne 2 ] || [[ $output != *"'no_such_flaw' is not a flaw"* ]] ||
  [[ $output != *"flaw_one flaw_two"* ]]; then
  fail "an unknown name: want exit 2, the name, and the valid names; got $status" "$output"
fi

# Everything is checked before anything is built, so a typo after a good name builds nothing.
: >"$FAKE_LOG"
FAKE_SCENARIO=passes run_script "$tree" flaw_one no_such_flaw
cases=$((cases + 1))
if [ "$status" -ne 2 ]; then
  fail "a good name and then an unknown one: want exit 2, got $status" "$output"
elif [ -s "$FAKE_LOG" ]; then
  fail "a good name and then an unknown one: something was built before the name was refused" \
    "$(cat "$FAKE_LOG")"
fi

run_script "$tree" --no-such-option
cases=$((cases + 1))
if [ "$status" -ne 2 ]; then
  fail "an option the script does not have: want exit 2, got $status" "$output"
fi

# --- The verdicts. --------------------------------------------------------------------------

new_tree <<'EOF'
set(SHALE_FLAW_NAMES
  flaw_one)
EOF
fixture="$tree"

# The variables that narrow a run are set here, so that the script is seen to clear them.
export SHALE_SIM_SEED=3 SHALE_SIM_STEPS=10 SHALE_SIM_KEYS=5 SHALE_SIM_SEEDS=2

# verdict <description> <scenario> <exit status> [output fragment...]
verdict() {
  local description=$1 scenario=$2 want=$3 fragment
  shift 3
  cases=$((cases + 1))
  export FAKE_SCENARIO="$scenario"
  run_script "$fixture" flaw_one
  if [ "$status" -ne "$want" ]; then
    fail "$description: exit status $status, wanted $want" "$output"
    return
  fi
  for fragment in "$@"; do
    if [[ $output != *"$fragment"* ]]; then
      fail "$description: output lacks '$fragment'" "$output"
    fi
  done
}

verdict "the seeded test fails with an invariant's id" invariant 0 "flaw flaw_one: caught by I4"
verdict "an id after other text on a line" invariant-mid-line 0 "flaw flaw_one: caught by I6"
verdict "the seeded test passes" passes 1 "flaw flaw_one: NOT CAUGHT"
verdict "the seeded test fails on a floor and has no id" floor 1 \
  "flaw flaw_one: FAILED WITHOUT AN INVARIANT (status 1)"
verdict "the simulator aborts" abort 1 "flaw flaw_one: FAILED WITHOUT AN INVARIANT (status 134)"
verdict "an id and then an abort, the test never reporting its failure" id-and-abort 1 \
  "FAILED WITHOUT AN INVARIANT (status 134)"
verdict "something that only looks like an id" id-inside-a-word 1 \
  "FAILED WITHOUT AN INVARIANT (status 1)"
verdict "the test does not run at all" not-run 1 "flaw flaw_one: COULD NOT RUN"

cases=$((cases + 1))
export FAKE_SCENARIO=invariant FAKE_BUILD_FAILS=1
run_script "$fixture" flaw_one
unset FAKE_BUILD_FAILS
if [ "$status" -ne 1 ] || [[ $output != *"flaw flaw_one: BUILD FAILED"* ]]; then
  fail "a build that fails: want exit 1 and BUILD FAILED, got $status" "$output"
fi

# One flaw that is not caught fails the whole run, whatever the others did.
new_tree <<'EOF'
set(SHALE_FLAW_NAMES
  flaw_one
  flaw_two)
EOF
cases=$((cases + 1))
export FAKE_SCENARIO=passes
run_script "$tree"
if [ "$status" -ne 1 ] || [[ $output != *"flaw flaw_one: NOT CAUGHT"* ]] ||
  [[ $output != *"flaw flaw_two: NOT CAUGHT"* ]]; then
  fail "no names given: want exit 1 and a verdict for each of the two flaws, got $status" "$output"
fi

finish flaws_test
