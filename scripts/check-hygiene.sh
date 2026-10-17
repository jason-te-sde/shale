#!/usr/bin/env bash
# scripts/check-hygiene.sh [root]
#
# Two repository-wide rules, enforced from the first commit:
#   1. no Chinese characters in any tracked file
#   2. no AI attribution in any tracked file or in any commit message
# `root` is a git working tree and defaults to the repository root.
#
# Rule 2 matches attribution *shapes*, listed one per line in attribution-patterns.txt next to this
# script. The bare words "Claude" and "Anthropic" are not violations: a vendor name in prose is
# not attribution.
#
# Everything matches at the byte level with plain `grep -E` and LC_ALL=C. Stock macOS has a grep
# without -P and a bash 3.2 without \u escapes, and both of them silently do the wrong thing
# rather than fail; a byte-range pattern behaves the same on every grep as long as matching is
# forced to be byte-oriented rather than left to the locale the runner happens to start in.
#
# Exit status: 0 clean, 1 a violation or an unusable patterns file, 2 nothing to check. A checker
# that checked nothing must not say "ok".
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

fail=0

# CJK Unified Ideographs, U+4E00-U+9FFF, are a contiguous block of 3-byte UTF-8 sequences. The
# leading byte 0xE4 is shared with code points below U+4E00 (down to U+4000), so its second byte is
# narrowed to what U+4E00 and up produce. 0xE9 needs no split because all of U+9000-U+9FFF is
# inside the block.
cjk_pattern=$'\xe4[\xb8-\xbf][\x80-\xbf]|[\xe5-\xe8][\x80-\xbf][\x80-\xbf]|\xe9[\x80-\xbf][\x80-\xbf]'

# The hits are captured and tested for emptiness rather than trusting an exit status: xargs
# reports 123 when any one of its grep invocations found nothing, which would hide a hit found by
# another invocation of a long file list.
hits="$(git ls-files -z | xargs -0 grep -nHIE "$cjk_pattern" 2>/dev/null || true)"
if [ -n "$hits" ]; then
  printf '%s\n' "$hits"
  echo "hygiene: Chinese characters found in tracked files (see above)" >&2
  fail=1
fi

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
  patterns+=("$line")
done <"$patterns_file"

# A truncated or emptied file would pass the check above and still scan for nothing. This is a
# floor, not an exact count, so that adding a sixth pattern does not break the script.
[ "${#patterns[@]}" -ge 5 ] || {
  echo "hygiene: expected at least 5 attribution patterns in $patterns_file, found ${#patterns[@]}" >&2
  exit 1
}

for pattern in "${patterns[@]}"; do
  hits="$(git ls-files -z -- . ':(exclude)scripts/attribution-patterns.txt' |
    xargs -0 grep -nHIEi "$pattern" 2>/dev/null || true)"
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits"
    echo "hygiene: AI attribution found in tracked files: $pattern" >&2
    fail=1
  fi
  hits="$(git log --format=%B 2>/dev/null | grep -nEi "$pattern" || true)"
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits"
    echo "hygiene: AI attribution found in a commit message: $pattern" >&2
    fail=1
  fi
done

if [ "$fail" -eq 0 ]; then
  echo "hygiene: ok"
fi
exit "$fail"
