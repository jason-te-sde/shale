#!/usr/bin/env bash
# Runs shale_crashtest with --self_test_damage, which overwrites a key that holds a value with a
# wrong one after the kill and before the check, and requires what the tool must do then: exit with
# status 1, and say that the check named the key it damaged. A check that cannot fail would pass
# every round of the crash test, so this is the test that the crash test can fail.
#
# The exit status is compared with 1 and not with "not 0": a usage error (2), a run that did not
# reach what it is for (3) and a crash all end with another status, and none of them shows that the
# check can see damage.
#
# Usage: crashtest_damage_test.sh <path to the shale_crashtest binary>
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/scripts/lib.sh
. "$here/../scripts/lib.sh"

crashtest="${1:?usage: crashtest_damage_test.sh <path to the shale_crashtest binary>}"
if [ ! -x "$crashtest" ]; then
  echo "crashtest_damage_test: $crashtest is not an executable" >&2
  exit 2
fi
tmp="$(mktemp -d "${TMPDIR:-/tmp}/crashtest_damage_test.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

"$crashtest" --rounds=3 --seed=1 --self_test_damage --db="$tmp/db" >"$tmp/out" 2>"$tmp/err"
status=$?
both="$(printf 'stdout:\n%s\nstderr:\n%s' "$(cat "$tmp/out")" "$(cat "$tmp/err")")"

cases=$((cases + 1))
if [ "$status" -ne 1 ]; then
  fail "a run with a damaged key: exit status $status, wanted 1" "$both"
fi

# The tool says which key it damaged, and then whether the check named that key.
damaged="$(sed -n 's/^self test: key \(key[0-9]*\) holds a wrong value now$/\1/p' "$tmp/out")"
cases=$((cases + 1))
if [ -z "$damaged" ]; then
  fail "a run with a damaged key: the output does not say which key was damaged" "$both"
fi

cases=$((cases + 1))
if ! grep -qxF "self test: the check named key $damaged" "$tmp/out"; then
  fail "a run with a damaged key: the check did not name '$damaged'" "$both"
fi

cases=$((cases + 1))
if grep -qF "did NOT name" "$tmp/out"; then
  fail "a run with a damaged key: the check said it did not name the key" "$both"
fi

# The problem itself names the key, on a line of its own, and the round is the one that failed.
cases=$((cases + 1))
if [ -n "$damaged" ] && ! grep -q "^  $damaged " "$tmp/out"; then
  fail "a run with a damaged key: no problem line starts with '$damaged'" "$both"
fi
cases=$((cases + 1))
if ! grep -q "FAILED$" "$tmp/out"; then
  fail "a run with a damaged key: no round is reported as FAILED" "$both"
fi

finish crashtest_damage_test
