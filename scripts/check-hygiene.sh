#!/usr/bin/env bash
# scripts/check-hygiene.sh [root]
#
# Two repository-wide rules, enforced from the first commit:
#   1. no CJK characters (Chinese, Japanese, Korean) in any tracked file or commit message: the
#      repository is English only
#   2. no authorship credit in any tracked file or in any commit message: the repository has one
#      author, so there is no co-author trailer, and no line says what produced a change
# `root` is a git working tree and defaults to the repository root.
#
# Rule 2 matches the *shapes* of such a credit, listed one per line in attribution-patterns.txt next
# to this script (blank lines and lines starting with # are ignored), whatever the case.
#
# Everything matches at the byte level with plain `grep -E` and LC_ALL=C. Stock macOS has a grep
# without -P and a bash 3.2 without \u escapes, and both of them silently do the wrong thing
# rather than fail; a byte-range pattern behaves the same on every grep as long as matching is
# forced to be byte-oriented rather than left to the locale the runner happens to start in.
#
# The script fails closed. grep reports "no match" with status 1 and "something is wrong" (a
# pattern it cannot compile, a file it cannot read) with status 2, and a scan that took both for
# "nothing found" would print "ok" having checked nothing. So every pattern is compiled once before
# anything is scanned, and every scan tells the two statuses apart.
#
# Exit status: 0 clean, 1 a violation or a failure of the check itself, 2 nothing to check. A
# checker that checked nothing must not say "ok".
set -uo pipefail
export LC_ALL=C

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$root" 2>/dev/null || {
  echo "hygiene: cannot enter $root" >&2
  exit 2
}
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
  echo "hygiene: $root is not a git working tree" >&2
  exit 2
}
if [ -z "$(git ls-files | head -n 1)" ]; then
  echo "hygiene: $root has no tracked files, so there is nothing to check" >&2
  exit 2
fi

errfile="$(mktemp)"
trap 'rm -f "$errfile"' EXIT

fail=0

# The UTF-8 byte ranges of the blocked code points. A range is a run of 3-byte sequences, so each
# is cut along the boundaries where the second or third byte changes meaning.
#   U+3000-30FF   CJK punctuation, hiragana, katakana        E3 80..83
#   U+4E00-9FFF   CJK Unified Ideographs                      E4 B8..BF, E5..E8, E9
#   U+AC00-D7AF   Hangul syllables                            EA B0..BF, EB..EC, ED 80..9D, ED 9E 80..AF
#   U+FF00-FFEF   Halfwidth and fullwidth forms               EF BC..BE, EF BF 80..AF
# E4 is shared with code points below U+4E00 (down to U+4000), so its second byte is narrowed to
# what U+4E00 and up produce. U+FEFF, the byte order mark, is EF BB BF and is not in the last range.
cjk_pattern=$'\xe3[\x80-\x83][\x80-\xbf]'
cjk_pattern+=$'|\xe4[\xb8-\xbf][\x80-\xbf]|[\xe5-\xe8][\x80-\xbf][\x80-\xbf]|\xe9[\x80-\xbf][\x80-\xbf]'
cjk_pattern+=$'|\xea[\xb0-\xbf][\x80-\xbf]|[\xeb\xec][\x80-\xbf][\x80-\xbf]'
cjk_pattern+=$'|\xed([\x80-\x9d][\x80-\xbf]|\x9e[\x80-\xaf])'
cjk_pattern+=$'|\xef([\xbc-\xbe][\x80-\xbf]|\xbf[\x80-\xaf])'

# require_valid_pattern <what it is> <pattern>: grep must be able to compile it. A pattern that
# cannot be compiled makes every later scan with it exit 2.
require_valid_pattern() {
  local status
  grep -E -e "$2" </dev/null >/dev/null 2>"$errfile"
  status=$?
  if [ "$status" -gt 1 ]; then
    echo "hygiene: $1 is not a valid regular expression: $2" >&2
    cat "$errfile" >&2
    exit 1
  fi
}

# The patterns live in their own file so that only that small file, not this whole script, has to
# sit outside the scan it feeds.
script_dir="$(cd "$(dirname "$0")" && pwd)"
patterns_file="$script_dir/attribution-patterns.txt"

# A missing patterns file must fail loudly on every bash. Without this, bash 3.2 aborts later on
# the empty array under `set -u`, bash 4.4 and newer loop zero times, and the script would print
# "hygiene: ok" having scanned for nothing.
[ -r "$patterns_file" ] || {
  echo "hygiene: cannot read $patterns_file" >&2
  exit 1
}

patterns=()
while IFS= read -r line || [ -n "$line" ]; do
  # Blank, whitespace-only and comment lines are not patterns: an empty pattern matches every line.
  [ -z "${line//[[:space:]]/}" ] && continue
  case "$line" in '#'*) continue ;; esac
  patterns+=("$line")
done <"$patterns_file"

# A truncated or emptied file would pass the check above and still scan for nothing. This is a
# floor, not an exact count, so that adding a fifth pattern does not break the script. Only real
# patterns count toward it.
[ "${#patterns[@]}" -ge 4 ] || {
  echo "hygiene: expected at least 4 attribution patterns in $patterns_file, found ${#patterns[@]}" >&2
  exit 1
}

require_valid_pattern "the CJK pattern" "$cjk_pattern"
for pattern in "${patterns[@]}"; do
  require_valid_pattern "an attribution pattern in $patterns_file" "$pattern"
done

# grep_batch <grep options> <pattern> <file>...: appends the matching lines to $hits. Status 1 is
# no match; anything else above 1 is a failure of the check and ends it.
hits=""
grep_batch() {
  local options=$1 pattern=$2 matches status
  shift 2
  matches="$(grep "-$options" -e "$pattern" -- "$@" 2>"$errfile")"
  status=$?
  case "$status" in
    0) hits+="$matches"$'\n' ;;
    1) ;;
    *)
      echo "hygiene: grep failed with status $status while scanning tracked files for: $pattern" >&2
      cat "$errfile" >&2
      exit 1
      ;;
  esac
}

# scan_tracked <grep options> <pattern> <pathspec>...: leaves the matching lines of the tracked
# files in $hits. The files go to grep in batches, and it is the batches' own statuses that are
# read: xargs would fold them into a single 123 whenever one batch finds nothing, hiding a hit
# in another. A tracked file that is missing from the working tree is a pending deletion, not an
# error.
scan_tracked() {
  local options=$1 pattern=$2 file batch=()
  shift 2
  hits=""
  while IFS= read -r -d '' file; do
    [ -f "$file" ] || continue
    batch+=("$file")
    if [ "${#batch[@]}" -ge 200 ]; then
      grep_batch "$options" "$pattern" "${batch[@]}"
      batch=()
    fi
  done < <(git ls-files -z -- "$@")
  if [ "${#batch[@]}" -gt 0 ]; then
    grep_batch "$options" "$pattern" "${batch[@]}"
  fi
}

# Every commit message reachable from HEAD, in one string. A repository with no commit yet has none.
messages=""
if git rev-parse --verify -q HEAD >/dev/null 2>&1; then
  messages="$(git log --format=%B)" || {
    echo "hygiene: git log failed" >&2
    exit 1
  }
fi

# scan_messages <grep options> <pattern>: leaves the matching lines of the commit messages in $hits.
scan_messages() {
  local options=$1 pattern=$2 status
  hits=""
  [ -n "$messages" ] || return 0
  hits="$(grep "-n$options" -e "$pattern" 2>"$errfile" <<<"$messages")"
  status=$?
  case "$status" in
    0 | 1) ;;
    *)
      echo "hygiene: grep failed with status $status while scanning commit messages for: $pattern" >&2
      cat "$errfile" >&2
      exit 1
      ;;
  esac
}

report() { # <hits> <message>
  printf '%s\n' "$1"
  echo "hygiene: $2" >&2
  fail=1
}

scan_tracked nHIE "$cjk_pattern" .
[ -n "$hits" ] && report "$hits" "CJK characters found in tracked files (see above)"

scan_messages E "$cjk_pattern"
[ -n "$hits" ] && report "$hits" "CJK characters found in a commit message (see above)"

for pattern in "${patterns[@]}"; do
  scan_tracked nHIEi "$pattern" . ':(exclude)scripts/attribution-patterns.txt'
  [ -n "$hits" ] && report "$hits" "authorship credit found in tracked files: $pattern"

  scan_messages Ei "$pattern"
  [ -n "$hits" ] && report "$hits" "authorship credit found in a commit message: $pattern"
done

if [ "$fail" -eq 0 ]; then
  echo "hygiene: ok"
fi
exit "$fail"
