#!/usr/bin/env bash
# Tests for scripts/release-check.sh.
#
# The script is what the release workflow trusts to refuse a tag that does not name the version the
# source says it is, and to say what the release notes are. A check that is only shown matching tags
# cannot be told from one that accepts everything, and one that is only shown wrong tags cannot be
# told from one that refuses everything, so each rule has a case of both kinds. The notes are compared
# byte for byte with what they should be, so that a trailing blank line or a stray link definition
# fails here and not on the release page.
# shellcheck disable=SC2016  # the fixtures are Markdown, and their backticks are not command substitution
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/scripts/lib.sh
. "$here/lib.sh"

source_dir="$(cd "$here/../.." && pwd)"
script="$source_dir/scripts/release-check.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

case_id=0

# new_tree: creates an empty fixture tree and leaves its path in $root.
new_tree() {
  case_id=$((case_id + 1))
  root="$tmp/tree$case_id"
  mkdir -p "$root"
}

# put <relative path> <line>...: writes the lines to a file in the fixture tree.
put() {
  local rel=$1
  shift
  printf '%s\n' "$@" >"$root/$rel"
}

# run <script argument>...: runs the script on $root. Its stdout and stderr, combined, are left in
# $output and its exit status in $status.
run() {
  output="$(bash "$script" --root "$root" "$@" 2>&1)"
  status=$?
}

# attempt <description> <exit status> [output fragment...] -- <script argument>...
attempt() {
  local description=$1 want=$2 fragment
  local fragments=()
  shift 2
  while [ "$#" -gt 0 ] && [ "$1" != -- ]; do
    fragments+=("$1")
    shift
  done
  shift
  cases=$((cases + 1))
  run "$@"
  if [ "$status" -ne "$want" ]; then
    fail "$description: exit status $status, wanted $want" "$output"
    return
  fi
  for fragment in ${fragments[@]+"${fragments[@]}"}; do
    case "$output" in
      *"$fragment"*) ;;
      *) fail "$description: output lacks '$fragment'" "$output" ;;
    esac
  done
}

# prints <description> <expected stdout> <script argument>...: the script exits with status 0 and
# prints exactly the expected text and a newline, and nothing else on stdout.
prints() {
  local description=$1 want=$2 got
  shift 2
  cases=$((cases + 1))
  bash "$script" --root "$root" "$@" >"$tmp/stdout" 2>"$tmp/stderr"
  got=$?
  if [ "$got" -ne 0 ]; then
    fail "$description: exit status $got, wanted 0" "$(cat "$tmp/stderr")"
    return
  fi
  printf '%s\n' "$want" >"$tmp/want"
  if ! cmp -s "$tmp/want" "$tmp/stdout"; then
    fail "$description: wrong output" "$(diff "$tmp/want" "$tmp/stdout")"
  fi
}

# --- The version, read from CMakeLists.txt. -----------------------------------------------------

new_tree
put CMakeLists.txt 'cmake_minimum_required(VERSION 3.25)' '' 'project(shale' '  VERSION 0.1.0' \
  '  DESCRIPTION "An LSM-tree storage engine"' '  LANGUAGES CXX)'
prints "a project call over several lines, after another call that has a VERSION" 0.1.0 version

new_tree
put CMakeLists.txt 'project(shale VERSION 2.10.3 LANGUAGES CXX)'
prints "a project call on one line" 2.10.3 version

new_tree
put CMakeLists.txt 'set(VERSION 9.9.9)' 'Project (shale LANGUAGES CXX VERSION 1.2.3)'
prints "a command name in any case, a space before its parenthesis, VERSION after other keywords" \
  1.2.3 version

new_tree
put CMakeLists.txt '# project(shale VERSION 9.9.9)' 'project(shale' '  VERSION 0.4.0  # the one' ')'
prints "a project call in a comment, and a comment after the version" 0.4.0 version

new_tree
put CMakeLists.txt 'project(shale' '  DESCRIPTION "the VERSION 9.9.9 line # not a comment"' \
  '  VERSION 1.0.0 LANGUAGES CXX)'
prints "VERSION and # inside a quoted description" 1.0.0 version

new_tree
put CMakeLists.txt 'project(shale DESCRIPTION "an (odd) one)" VERSION 3.1.4)'
prints "parentheses inside a quoted description" 3.1.4 version

new_tree
put CMakeLists.txt 'project(shale VERSION "2.0")'
prints "a quoted version" 2.0 version

new_tree
put CMakeLists.txt 'project(shale VERSION 1.2.3.4)'
prints "a version of four numbers" 1.2.3.4 version

new_tree
put CMakeLists.txt 'my_project(other VERSION 9.9.9)' 'ExternalProject_Add(dep VERSION 8.8.8)' \
  'project(shale VERSION 0.1.0)'
prints "commands whose names end in project are not project" 0.1.0 version

new_tree
put CMakeLists.txt 'add_library(shale)'
attempt "no project call" 2 "no project() call" -- version

new_tree
put CMakeLists.txt 'project(shale LANGUAGES CXX)'
attempt "a project call with no VERSION" 2 "has no VERSION" -- version

new_tree
put CMakeLists.txt 'project(shale VERSION ${SHALE_VERSION})'
attempt "a version that is a variable" 2 "not a literal version" -- version

new_tree
put CMakeLists.txt 'project(shale VERSION 1.x)'
attempt "a version that is not numbers" 2 "not a literal version" -- version

new_tree
put CMakeLists.txt 'project(shale VERSION)'
attempt "VERSION with nothing after it" 2 "not a literal version" -- version

new_tree
put CMakeLists.txt 'project(shale VERSION 1.0.0)' 'project(other VERSION 2.0.0)'
attempt "two project calls" 2 "more than one project() call" -- version

new_tree
attempt "no CMakeLists.txt" 2 "cannot read" -- version

# --- The tag against the version. ---------------------------------------------------------------

new_tree
put CMakeLists.txt 'project(shale' '  VERSION 0.1.0' '  LANGUAGES CXX)'
attempt "the tag of the version" 0 "v0.1.0" -- check v0.1.0
attempt "a fake tag is refused, with both versions printed" 1 "v9.9.9" "names version 9.9.9" \
  "VERSION 0.1.0" "must be v0.1.0" -- check v9.9.9
attempt "a patch version behind" 1 "names version 0.1.1" "VERSION 0.1.0" -- check v0.1.1
attempt "a tag without the v" 1 "does not start with v" "must be v0.1.0" -- check 0.1.0
attempt "a capital V" 1 "must be v0.1.0" -- check V0.1.0
attempt "a pre-release suffix" 1 "names version 0.1.0-rc1" -- check v0.1.0-rc1
attempt "a shorter version of the same number" 1 "names version 0.1" -- check v0.1
attempt "a tag with a trailing character" 1 "must be v0.1.0" -- check v0.1.0.
attempt "a tag with the refs prefix" 1 "must be v0.1.0" -- check refs/tags/v0.1.0
attempt "no tag given" 2 "usage" -- check
attempt "an empty tag" 2 "usage" -- check ""

new_tree
put CMakeLists.txt 'project(shale VERSION 1.2.3)'
attempt "a tag of another tree's version" 0 "v1.2.3" -- check v1.2.3
attempt "the old tag of the other tree" 1 "VERSION 1.2.3" -- check v0.1.0

new_tree
put CMakeLists.txt 'project(shale LANGUAGES CXX)'
attempt "a tag when the version cannot be read is not accepted" 2 "has no VERSION" -- check v0.1.0

# --- The notes, cut from CHANGELOG.md. -------------------------------------------------------------

new_tree
put CHANGELOG.md \
  '# Changelog' '' \
  '## [Unreleased]' '' '### Added' '' '- the next thing' '' \
  '## [0.2.0] - 2026-11-01' '' 'Second.' '' '### Fixed' '' '- a fix' '' \
  '## [0.1.0] - 2026-10-18' '' 'First.' '' \
  '[Unreleased]: https://example.com/compare/v0.2.0...HEAD' \
  '[0.2.0]: https://example.com/compare/v0.1.0...v0.2.0' \
  '[0.1.0]: https://example.com/releases/tag/v0.1.0'
prints "a section in the middle ends at the next version, and keeps its subsections" \
  $'Second.\n\n### Fixed\n\n- a fix' notes 0.2.0
prints "the last section ends at the link definitions" 'First.' notes 0.1.0

new_tree
put CHANGELOG.md '## [1.0.0] - 2026-01-01' '' '' 'Body.' '' '' '## [0.9.0] - 2025-01-01' 'old'
prints "blank lines at both ends are not part of the notes" 'Body.' notes 1.0.0

new_tree
put CHANGELOG.md '## [1.0.0]' 'No date on the heading.' '## [0.9.0]' 'old'
prints "a heading with no date" 'No date on the heading.' notes 1.0.0

new_tree
put CHANGELOG.md '## [1.0.0] - 2026-01-01' 'Text.' '```' '## not a heading' '[ref]: not a definition' \
  '```' 'After.' '## [0.9.0] - 2025-01-01' 'old'
prints "a heading and a definition inside a fenced block are text" \
  $'Text.\n```\n## not a heading\n[ref]: not a definition\n```\nAfter.' notes 1.0.0

new_tree
put CHANGELOG.md '## [0.1.0-rc1] - a' 'rc.' '## [10.1.0] - b' 'ten.' '## [0.1.0] - c' 'right.' \
  '## [0x1y0] - d' 'a dot is not a pattern.' '## [0.1.01] - e' 'longer.'
prints "only the heading that names the version exactly" 'right.' notes 0.1.0

new_tree
printf '## [1.0.0] - x\r\nBody.\r\nMore.\r\n## [0.9.0] - y\r\nold\r\n' >"$root/CHANGELOG.md"
prints "lines that end in carriage return and line feed" $'Body.\nMore.' notes 1.0.0

new_tree
printf '## [1.0.0] - x\nBody with no newline at the end.' >"$root/CHANGELOG.md"
prints "a last line with no newline" 'Body with no newline at the end.' notes 1.0.0

new_tree
put CHANGELOG.md '## [Unreleased]' 'Next.' '## [0.1.0] - x' 'First.'
attempt "a version that has no section" 1 "no section for 0.3.0" -- notes 0.3.0

new_tree
put CHANGELOG.md '## [0.1.0] - x' '## [0.0.9] - y' 'old'
attempt "a section with nothing in it" 1 "section for 0.1.0 is empty" -- notes 0.1.0

new_tree
put CHANGELOG.md '## [0.1.0] - x' '' '   ' '' '[0.1.0]: https://example.com/'
attempt "a section of blank lines and a definition" 1 "section for 0.1.0 is empty" -- notes 0.1.0

new_tree
put CHANGELOG.md '## [0.1.0] - x' 'One.' '## [0.0.9] - y' 'old' '## [0.1.0] - z' 'Two.'
attempt "the same version twice" 1 "more than one section for 0.1.0" -- notes 0.1.0

new_tree
put CHANGELOG.md '## [0.1.0] - x' 'One.'
attempt "a tag given for a version" 2 "not a version number" -- notes v0.1.0
attempt "a word given for a version" 2 "not a version number" -- notes Unreleased
attempt "no version given" 2 "usage" -- notes

new_tree
attempt "no CHANGELOG.md" 2 "cannot read" -- notes 0.1.0

# --- Links in the notes. ------------------------------------------------------------------------------
# A release page is not a file of the repository, so a link relative to the repository is a dead
# one there. With --link-base the relative links are made absolute and nothing else changes.

new_tree
put CHANGELOG.md '## [1.0.0] - x' \
  'See [the guide](docs/guide.md#part), [site](https://example.com/a), [here](#local),' \
  '![img](docs/i.png), [dir](docs/design/), [root](/README.md) and [mail](mailto:a@example.com).' \
  '```' '[example](docs/example.md)' '```' \
  '[a](a.md) and [b](<b.md>) on a line, [c](c.md "title")'
base=https://github.com/o/r/blob/v1.0.0
prints "relative links are made absolute, and the others are left alone" \
  "See [the guide]($base/docs/guide.md#part), [site](https://example.com/a), [here](#local),
![img]($base/docs/i.png), [dir]($base/docs/design/), [root]($base/README.md) and [mail](mailto:a@example.com).
\`\`\`
[example](docs/example.md)
\`\`\`
[a]($base/a.md) and [b](<$base/b.md>) on a line, [c]($base/c.md \"title\")" \
  --link-base "$base/" notes 1.0.0

new_tree
put CHANGELOG.md '## [1.0.0] - x' 'See [the guide](docs/guide.md).'
prints "a base with no slash at the end gets one" 'See [the guide](https://x.example/p/docs/guide.md).' \
  --link-base https://x.example/p notes 1.0.0
prints "without --link-base the links are as they were written" 'See [the guide](docs/guide.md).' \
  notes 1.0.0
attempt "--link-base with no value" 2 "usage" -- --link-base
attempt "an option after the command" 2 "usage" -- notes 1.0.0 --link-base
attempt "an option the script does not have" 2 "usage" -- --no-such-option notes 1.0.0
attempt "a command the script does not have" 2 "usage" -- publish 1.0.0
attempt "no command" 2 "usage" --

# --- The tree this test is in. ------------------------------------------------------------------------
# RELEASING.md says that the version and its changelog entry are changed together, and that a tag is
# v plus the version. The tree is held to both, so that a version that is bumped alone fails here and
# not on the release day.

root="$source_dir"
cases=$((cases + 1))
run version
real_version="$output"
if [ "$status" -ne 0 ] || ! [[ $real_version =~ ^[0-9]+(\.[0-9]+)*$ ]]; then
  fail "the version of this tree: wanted a version number, got status $status" "$output"
elif ! grep -Eq "^[[:space:]]*VERSION[[:space:]]+$real_version([[:space:]]|\$)" "$source_dir/CMakeLists.txt"; then
  fail "the version of this tree: CMakeLists.txt does not say VERSION $real_version"
fi

attempt "this tree's own tag" 0 "v$real_version" -- check "v$real_version"
attempt "a fake tag against this tree" 1 "v9.9.9" "names version 9.9.9" "VERSION $real_version" \
  "must be v$real_version" -- check v9.9.9

cases=$((cases + 1))
run notes "$real_version"
if [ "$status" -ne 0 ]; then
  fail "the changelog has no entry for the version of this tree, $real_version" "$output"
elif grep -Eq '^(## |\[[^]]+\]: )' <<<"$output"; then
  fail "the notes of $real_version run past the end of its section" "$output"
fi

finish release_check_test
