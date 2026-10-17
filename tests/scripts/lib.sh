# shellcheck shell=bash
# shellcheck disable=SC2154  # $script and $root are set by the test that sources this file.
#
# Shared by the script tests in this directory. Source it; it is not a test.
#
# A test points $script at the checker and $root at the tree the checker should look at, calls
# expect once per case, and ends with finish. A checker that is only ever shown violations cannot
# be told apart from one that rejects everything, and one that is only shown clean trees cannot be
# told apart from one that accepts everything, so every rule gets cases of both kinds.

failures=0
cases=0

# indent: shows captured output under a failure message.
indent() { sed 's/^/    | /' >&2; }

# fail <message> [output]: records one failed check and shows the output it was made from.
fail() {
  echo "FAIL: $1" >&2
  if [ $# -gt 1 ]; then
    printf '%s\n' "$2" | indent
  fi
  failures=$((failures + 1))
}

# expect <description> <exit status> [expected output fragment...]
# Runs "$script" on "$root" and compares its exit status and its stdout and stderr combined.
expect() {
  local description=$1 want=$2 output got fragment
  shift 2
  cases=$((cases + 1))
  output="$(bash "$script" "$root" 2>&1)"
  got=$?
  if [ "$got" -ne "$want" ]; then
    fail "$description: exit status $got, wanted $want" "$output"
    return
  fi
  for fragment in "$@"; do
    case "$output" in
      *"$fragment"*) ;;
      *) fail "$description: output lacks '$fragment'" "$output" ;;
    esac
  done
}

# finish <test name>: prints the tally; the exit status says whether every case behaved.
finish() {
  echo "$1: $((cases - failures))/$cases cases behaved as expected"
  [ "$failures" -eq 0 ]
}
