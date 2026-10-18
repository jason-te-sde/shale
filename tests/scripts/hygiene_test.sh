#!/usr/bin/env bash
# Tests for scripts/check-hygiene.sh.
#
# The script under test searches for CJK characters and for authorship credits (a co-author
# trailer, a line that says what produced a change, the robot emoji), and this repository must
# itself pass it. So every needle is assembled at run time from pieces: neither the byte sequences
# nor the credit strings appear anywhere in this file.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/scripts/lib.sh
. "$here/lib.sh"

scripts_dir="$(cd "$here/../../scripts" && pwd)"
# shellcheck disable=SC2034  # read by expect in lib.sh
script="$scripts_dir/check-hygiene.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Fixture repositories must not pick up the developer's own git configuration or hooks.
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null GIT_CONFIG_NOSYSTEM=1

# Nor may they land in the repository this test was started from. A git hook that runs the suite
# has these set, and every `git -C <fixture>` would then act on that repository instead.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE

case_id=0

# new_repo: creates an empty git repository and leaves its path in $root.
new_repo() {
  case_id=$((case_id + 1))
  root="$tmp/repo$case_id"
  mkdir -p "$root"
  git -C "$root" -c init.defaultBranch=main init -q
}

git_in() {
  git -C "$root" -c user.name=Fixture -c user.email=fixture@example.com \
    -c commit.gpgsign=false "$@"
}

# write_file <relative path> <printf format> [argument...]: the format is where the byte escapes go.
write_file() {
  local rel=$1
  shift
  mkdir -p "$root/$(dirname "$rel")"
  # shellcheck disable=SC2059
  printf "$@" >"$root/$rel"
}

# commit <subject> [more paragraphs...]: stages everything and commits it.
commit() {
  local args=() paragraph
  for paragraph in "$@"; do args+=(-m "$paragraph"); done
  git_in add -A
  git_in commit -q "${args[@]}"
}

# The credit strings, assembled so that no single line of this file contains one. The names are
# placeholders: the check does not care who or what is named.
co_author_trailer() { printf '%s-%s-%s: %s <person@example.com>' Co Authored By "$1"; }
robot() { printf '\xf0\x9f\xa4\x96'; }

# One sample of each shape in attribution-patterns.txt, in the order of the file: a trailer, the
# two phrases (the second in mixed case, to show that the match ignores case) and the emoji.
sample_trailer() { co_author_trailer 'Example Person'; }
sample_generated_with() { printf '%s with %s' Generated 'Example Tool'; }
sample_generated_by() { printf '%s%s %s %s' Gener ated By 'Example Tool'; }
sample_robot() { printf '%s done' "$(robot)"; }
shape_samples=(sample_trailer sample_generated_with sample_generated_by sample_robot)

# --- Clean repositories pass. ------------------------------------------------------------------

new_repo
write_file README.md '%s\n' 'A clean file.'
commit 'chore: add a clean file' 'The body explains why and says what was left out.'
expect "a clean repository" 0 "hygiene: ok"

new_repo
write_file notes.md '%s\n' 'The generated file is checked in, and a person wrote the rest by hand.'
commit 'docs: describe the generated file' 'Each word of a credit is fine on its own; it is the phrase that is not.'
expect "words that are only part of a credit" 0 "hygiene: ok"

# --- CJK characters in a tracked file: both edges of every blocked range, and what lies outside. ---

# Each entry is "<code point> <the UTF-8 bytes as printf escapes>".
blocked_in_files=(
  'U+3000 \xe3\x80\x80' 'U+3001 \xe3\x80\x81' 'U+303F \xe3\x80\xbf'
  'U+3040 \xe3\x81\x80' 'U+3042 \xe3\x81\x82' 'U+30A2 \xe3\x82\xa2' 'U+30FF \xe3\x83\xbf'
  'U+4E00 \xe4\xb8\x80' 'U+4E2D \xe4\xb8\xad' 'U+5E74 \xe5\xb9\xb4' 'U+7684 \xe7\x9a\x84'
  'U+9FFF \xe9\xbf\xbf'
  'U+AC00 \xea\xb0\x80' 'U+AFFF \xea\xbf\xbf' 'U+B000 \xeb\x80\x80' 'U+C73C \xec\x9d\xbc'
  'U+D000 \xed\x80\x80' 'U+D77F \xed\x9d\xbf' 'U+D780 \xed\x9e\x80' 'U+D7AF \xed\x9e\xaf'
  'U+FF00 \xef\xbc\x80' 'U+FF21 \xef\xbc\xa1' 'U+FF5E \xef\xbd\x9e' 'U+FF9F \xef\xbe\x9f'
  'U+FFEF \xef\xbf\xaf'
)
for case_spec in "${blocked_in_files[@]}"; do
  new_repo
  write_file doc.md "%b\n" "${case_spec#* }"
  commit 'docs: add a file'
  expect "a file containing ${case_spec%% *}" 1 "CJK characters" "tracked files" "doc.md"
done

# Neighbours of those ranges that are not CJK, and the usual non-ASCII an English text carries.
allowed_in_files=(
  'U+00E9 \xc3\xa9' 'U+2014 \xe2\x80\x94' 'U+4DFF \xe4\xb7\xbf' 'U+A000 \xea\x80\x80'
  'U+ABFF \xea\xaf\xbf' 'U+E000 \xee\x80\x80' 'U+FEFF \xef\xbb\xbf' 'U+FFF0 \xef\xbf\xb0'
)
for case_spec in "${allowed_in_files[@]}"; do
  new_repo
  write_file doc.md "caf%b\n" "${case_spec#* }"
  commit 'docs: add a file'
  expect "a file containing ${case_spec%% *}, which is not blocked" 0 "hygiene: ok"
done

# --- CJK characters in a commit message: one case per script. ------------------------------------

for case_spec in 'U+4E2D \xe4\xb8\xad' 'U+3042 \xe3\x81\x82' 'U+30A2 \xe3\x82\xa2' \
  'U+C73C \xec\x9d\xbc' 'U+FF21 \xef\xbc\xa1'; do
  new_repo
  write_file a.txt '%s\n' 'clean'
  commit "feat: add a file $(printf '%b' "${case_spec#* }")"
  expect "a commit message containing ${case_spec%% *}" 1 "CJK characters" "commit message"
done

new_repo
write_file a.txt '%s\n' 'clean'
commit "feat: add a file $(printf '\xe2\x80\x94') caf$(printf '\xc3\xa9')"
expect "a commit message with an em dash and an accented letter" 0 "hygiene: ok"

# --- A credit in a commit message, anywhere in the history. ------------------------------------

new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file' "$(co_author_trailer 'Example Person')"
expect "a co-author trailer in the only commit" 1 "commit message"

new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file' "$(co_author_trailer 'Example Person')"
write_file b.txt '%s\n' 'also clean'
commit 'feat: add another file'
expect "a co-author trailer in an earlier commit" 1 "commit message"

new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file' "$(printf '%s-%s-%s: %s <person@example.com>' co authored by 'Example Person')"
expect "a co-author trailer in lower case" 1 "commit message"

for sample in "${shape_samples[@]}"; do
  new_repo
  write_file a.txt '%s\n' 'clean'
  commit 'feat: add a file' "$($sample)"
  expect "$sample in a commit message" 1 "commit message"
done

# --- A credit in a tracked file. ---------------------------------------------------------------

for sample in "${shape_samples[@]}"; do
  new_repo
  write_file src.cc '// %s\n' "$($sample)"
  commit 'feat: add a file'
  expect "$sample in a tracked file" 1 "tracked files" "src.cc"
done

# An untracked file is not part of the repository yet; the check covers what git would commit.
new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file'
write_file untracked.txt '%s\n' "$(sample_generated_with)"
expect "an untracked file with a credit" 0 "hygiene: ok"

# --- The checker must refuse to report success when it checked nothing. ------------------------

case_id=$((case_id + 1))
root="$tmp/not-a-repo"
mkdir -p "$root"
expect "a directory that is not a git repository" 2 "hygiene:"

new_repo
expect "a repository with no tracked files" 2 "hygiene:"

# --- The patterns file is part of the checker. -------------------------------------------------

patterns="$scripts_dir/attribution-patterns.txt"

# real_patterns: the patterns in the real file, without its comments and blank lines.
real_patterns() { grep -vE '^[[:space:]]*(#|$)' "$patterns"; }

# use_script_copy <name>: from now on check with a private copy of the script. The patterns file
# is looked up next to the script, so the copy's can be damaged without touching the real one.
use_script_copy() {
  copy_dir="$tmp/copy-$1"
  mkdir -p "$copy_dir"
  cp "$scripts_dir/check-hygiene.sh" "$copy_dir/check-hygiene.sh"
  script="$copy_dir/check-hygiene.sh"
}

use_real_script() {
  # shellcheck disable=SC2034  # read by expect in lib.sh
  script="$scripts_dir/check-hygiene.sh"
}

new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file' "$(co_author_trailer 'Example Person')"

use_script_copy missing
expect "a missing patterns file" 1 "cannot read"

use_script_copy short
real_patterns | head -n 3 >"$copy_dir/attribution-patterns.txt"
expect "a patterns file with three patterns" 1 "at least 4"

# Comment lines and blank lines are not patterns, so they do not count toward the floor...
use_script_copy padded-short
{
  echo '# a comment'
  real_patterns | head -n 3
  echo
  echo '   '
  echo '# another comment'
} >"$copy_dir/attribution-patterns.txt"
expect "three patterns padded out with comments and blank lines" 1 "at least 4"

# ...and they are not scanned for either: an empty pattern would match every line of every file.
new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file'
use_script_copy padded
{
  echo '# one extended regular expression per line'
  echo
  cat "$patterns"
  echo '   '
  echo '# the end'
} >"$copy_dir/attribution-patterns.txt"
expect "the real patterns padded with comments and blank lines" 0 "hygiene: ok"

# A pattern grep cannot compile must stop the check. grep reports it with status 2, the same as a
# missing file, and a scan that read that as "no match" would print "ok" having looked for nothing.
for bad_pattern in 'unbalanced (paren' 'unclosed [bracket' "dangling \\"; do
  use_script_copy bad-regex
  { cat "$patterns"; printf '%s\n' "$bad_pattern"; } >"$copy_dir/attribution-patterns.txt"
  expect "the pattern '$bad_pattern' in the patterns file" 1 "not a valid regular expression"
done
use_script_copy bad-regex-only
{ real_patterns | head -n 3; echo 'unbalanced (paren'; } >"$copy_dir/attribution-patterns.txt"
expect "a bad pattern as the fourth and last one" 1 "not a valid regular expression"

# Every pattern is what catches its own shape. Each sample above is refused by the real patterns
# file; with its pattern alone taken out of a private copy (and a filler that matches nothing put
# in, for the floor) the same sample goes through, so no pattern is carried by another one, and a
# pattern that is added without a sample is noticed here.
cases=$((cases + 1))
real_count="$(real_patterns | wc -l | tr -d ' ')"
if [ "$real_count" -ne "${#shape_samples[@]}" ]; then
  fail "the patterns file has $real_count patterns and this test has ${#shape_samples[@]} samples"
fi
index=0
for sample in "${shape_samples[@]}"; do
  index=$((index + 1))
  new_repo
  write_file a.txt '%s\n' 'clean'
  commit 'feat: add a file' "$($sample)"
  use_script_copy "without-$index"
  { real_patterns | sed "${index}d"; echo 'qzqzqzqzqzqzqzqz'; } >"$copy_dir/attribution-patterns.txt"
  expect "$sample in a commit message, without its own pattern" 0 "hygiene: ok"

  new_repo
  write_file src.cc '// %s\n' "$($sample)"
  commit 'feat: add a file'
  expect "$sample in a tracked file, without its own pattern" 0 "hygiene: ok"
done

# An error while scanning a file is not "no match" either. (root can read anything, so there is
# nothing to provoke.)
use_real_script
if [ "$(id -u)" -ne 0 ]; then
  new_repo
  write_file a.txt '%s\n' 'clean'
  write_file secret.txt '%s\n' 'clean too'
  commit 'feat: add files'
  chmod 000 "$root/secret.txt"
  expect "a tracked file the scan cannot read" 1 "secret.txt" "hygiene:"
  chmod 600 "$root/secret.txt"
else
  echo "SKIP: a tracked file the scan cannot read (running as root)" >&2
fi

# A tracked file that is deleted in the working tree is a normal state, not a scan error.
new_repo
write_file a.txt '%s\n' 'clean'
write_file gone.txt '%s\n' 'clean too'
commit 'feat: add files'
rm "$root/gone.txt"
expect "a tracked file deleted from the working tree" 0 "hygiene: ok"

finish hygiene_test
