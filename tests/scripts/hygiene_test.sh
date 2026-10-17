#!/usr/bin/env bash
# Tests for scripts/check-hygiene.sh.
#
# The scripts under test search for Chinese characters and for AI attribution, and this repository
# must itself pass them. So every needle is assembled at run time from pieces: neither the byte
# sequences nor the attribution strings appear anywhere in this file.
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

# The attribution strings, assembled so that no single line of this file contains one.
co_author_trailer() { printf '%s-%s-%s: %s <noreply@%s.com>' Co Authored By "$1" anthropic; }
generated_line() { printf '%s with %s Code' Generated "$1"; }
written_line() { printf 'This file was %s by %s.' written "$1"; }
robot() { printf '\xf0\x9f\xa4\x96'; }
ai_generated() { printf '%s-generated' AI; }

# --- Clean repositories pass. ------------------------------------------------------------------

new_repo
write_file README.md '%s\n' 'A clean file.'
commit 'chore: add a clean file' 'The body explains why and says what was left out.'
expect "a clean repository" 0 "hygiene: ok"

new_repo
write_file notes.md '%s\n' 'The claude-3 and anthropic SDK names are fine on their own, so is Copilot.'
commit 'docs: mention vendors' "$(printf '%s-%s-%s: %s <jane@example.com>' Co Authored By 'Jane Doe')"
expect "vendor names and a human co-author trailer" 0 "hygiene: ok"

# --- Chinese characters: the edges of U+4E00-U+9FFF. -------------------------------------------

for case_spec in 'U+4E00 \xe4\xb8\x80' 'U+4E2D \xe4\xb8\xad' 'U+5E74 \xe5\xb9\xb4' \
  'U+7684 \xe7\x9a\x84' 'U+9FFF \xe9\xbf\xbf'; do
  new_repo
  write_file doc.md "%b\n" "${case_spec#* }"
  commit 'docs: add a file'
  expect "a file containing ${case_spec%% *}" 1 "Chinese characters" "doc.md"
done

for case_spec in 'U+4DFF \xe4\xb7\xbf' 'U+A000 \xea\x80\x80' 'U+00E9 \xc3\xa9' 'U+2014 \xe2\x80\x94'; do
  new_repo
  write_file doc.md "caf%b\n" "${case_spec#* }"
  commit 'docs: add a file'
  expect "a file containing ${case_spec%% *}, which is not a CJK ideograph" 0 "hygiene: ok"
done

# --- AI attribution in a commit message, anywhere in the history. ------------------------------

new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file' "$(co_author_trailer Claude)"
expect "an AI co-author trailer in the only commit" 1 "commit message"

new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file' "$(co_author_trailer Claude)"
write_file b.txt '%s\n' 'also clean'
commit 'feat: add another file'
expect "an AI co-author trailer in an earlier commit" 1 "commit message"

new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file' "$(generated_line Claude)"
expect "a generated-with line in a commit message" 1 "commit message"

new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file' "$(written_line Claude)"
expect "a written-by line in a commit message" 1 "commit message"

new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file' "$(robot) done"
expect "the robot emoji in a commit message" 1 "commit message"

new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file' "This change is $(ai_generated)."
expect "a machine-authorship mark in a commit message" 1 "commit message"

# --- AI attribution in a tracked file. ---------------------------------------------------------

new_repo
write_file src.cc '// %s\n' "$(written_line Claude)"
commit 'feat: add a file'
expect "a written-by comment in a tracked file" 1 "tracked files" "src.cc"

new_repo
write_file src.cc '// %s\n' "$(co_author_trailer Claude)"
commit 'feat: add a file'
expect "a co-author trailer in a tracked file" 1 "tracked files" "src.cc"

# An untracked file is not part of the repository yet; the check covers what git would commit.
new_repo
write_file a.txt '%s\n' 'clean'
commit 'feat: add a file'
write_file untracked.txt '%s\n' "$(written_line Claude)"
expect "an untracked file with attribution" 0 "hygiene: ok"

# --- The checker must refuse to report success when it checked nothing. ------------------------

case_id=$((case_id + 1))
root="$tmp/not-a-repo"
mkdir -p "$root"
expect "a directory that is not a git repository" 2 "hygiene:"

new_repo
expect "a repository with no tracked files" 2 "hygiene:"

# --- The patterns file is part of the checker. -------------------------------------------------

patterns="$scripts_dir/attribution-patterns.txt"

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
commit 'feat: add a file' "$(co_author_trailer Claude)"

use_script_copy missing
expect "a missing patterns file" 1 "cannot read"

use_script_copy short
head -n 4 "$patterns" >"$copy_dir/attribution-patterns.txt"
expect "a patterns file with four patterns" 1 "at least 5"

use_real_script

finish hygiene_test
