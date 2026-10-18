#!/usr/bin/env bash
# Tests for scripts/check-links.sh.
#
# A link checker that is only shown broken links cannot be told from one that rejects everything,
# and one that is only shown good links cannot be told from one that accepts everything, so each
# rule has a fixture that must alarm and, where a neighbouring spelling is legitimate, one that must
# not.
# shellcheck disable=SC2016  # the fixtures are Markdown, and their backticks are not command substitution
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/scripts/lib.sh
. "$here/lib.sh"

# shellcheck disable=SC2034  # read by expect in lib.sh
script="$here/../../scripts/check-links.sh"
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
  mkdir -p "$root/$(dirname "$rel")"
  printf '%s\n' "$@" >"$root/$rel"
}

# --- The basic pair: a tree with one broken link and a clean tree. --------------------------------

new_tree
put README.md 'See [the format](docs/format.md).'
expect "a link to a file that is not there" 1 "README.md:1: broken link: docs/format.md"

new_tree
put README.md 'See [the format](docs/format.md).'
put docs/format.md '# Format'
expect "a link to a file that is there" 0 "ok, 1 relative link(s) in 2 file(s)"

# --- What is not checked. ----------------------------------------------------------------------------

new_tree
put README.md 'A [site](https://example.com/nothing), a [mail](mailto:a@example.com) and [local](a.md).'
put a.md 'text'
expect "external links are not followed" 0 "ok, 1 relative link(s)"

new_tree
put README.md '[ftp](ftp://host/file) and [a](http://host/x#frag).'
expect "a tree of external links only checked nothing" 2 "nothing was checked"

new_tree
put README.md 'Real: [a](a.md).' '```' 'Example: [gone](missing.md)' '```' 'After: [a](a.md).'
put a.md 'text'
expect "a link inside a fenced block is an example" 0

new_tree
put README.md 'Write `[gone](missing.md)` to link, and [a](a.md).'
put a.md 'text'
expect "a link inside a code span is an example" 0

new_tree
put README.md '```' '[gone](missing.md)'
put a.md 'text'
expect "an unclosed fence hides the rest of the file" 2 "nothing was checked"

# --- Spellings of a link. -----------------------------------------------------------------------------

new_tree
put README.md '![diagram](docs/missing.png)'
expect "an image that is not there" 1 "README.md:1: broken link: docs/missing.png"

new_tree
put README.md '![diagram](docs/d.png)'
put docs/d.png 'png'
expect "an image that is there" 0

new_tree
put README.md '[a]: docs/a.md' '' 'text [a]'
expect "a reference definition that is broken" 1 "README.md:1: broken link: docs/a.md"

new_tree
put README.md '[a]: docs/a.md "the title"' '' 'text [a]'
put docs/a.md '# A'
expect "a reference definition with a title" 0

new_tree
put README.md '<a href="docs/gone.md">gone</a> <img src="docs/gone.png">'
expect "raw html links that are broken" 1 "docs/gone.md" "docs/gone.png"

new_tree
put README.md '[a](docs/a.md "the title") and [b](<docs/b.md>)'
put docs/a.md '# A'
put docs/b.md '# B'
expect "a title after the target, and angle brackets around it" 0 "ok, 2 relative link(s)"

new_tree
put README.md '[![CI](https://example.com/badge.svg)](docs/a.md) and [![x](docs/x.png)](docs/a.md)'
put docs/a.md '# A'
expect "an image nested in a link, the image missing" 1 "docs/x.png"

new_tree
put README.md '[a](docs/a.md) [b](docs/b.md) [c](docs/c.md)'
put docs/a.md '# A'
expect "three links on a line, two broken" 1 "docs/b.md" "docs/c.md" "2 broken link(s) among 3"

# --- Where a link is resolved from. ----------------------------------------------------------------

new_tree
put docs/design/0001.md '[up](../architecture.md) and [sideways](0002.md)'
put docs/architecture.md '# Architecture'
put docs/design/0002.md '# Two'
expect "paths from a nested file are taken from its directory" 0

new_tree
put docs/design/0001.md '[up](../architecture.md)'
expect "a path from a nested file that is broken" 1 "docs/design/0001.md:1: broken link: ../architecture.md"

new_tree
put docs/design/0001.md '[root](/README.md)'
put README.md '# shale'
expect "a path that starts with a slash is taken from the root" 0

new_tree
put docs/design/0001.md '[root](/docs/nothing.md)'
put README.md '# shale'
expect "a path that starts with a slash, broken" 1 "/docs/nothing.md"

new_tree
put README.md '[the directory](docs/design/)'
mkdir -p "$root/docs/design"
expect "a link to a directory" 0

# --- Fragments. -----------------------------------------------------------------------------------

new_tree
put README.md '# Title' '## The heading' '[a](#the-heading) and [b](docs/a.md#second-part)'
put docs/a.md '# A' '## Second part'
expect "fragments that name headings, in this file and another" 0

new_tree
put README.md '# Title' '[a](#no-such-heading)'
expect "a fragment that names no heading of the file" 1 "README.md:2: broken link: #no-such-heading"

new_tree
put README.md '[b](docs/a.md#third-part)'
put docs/a.md '# A' '## Second part'
expect "a fragment that names no heading of another file" 1 "#third-part"

new_tree
put README.md '[b](docs/a.md#whats-new-in-v10)'
put docs/a.md "## What's new in \`v1.0\`?"
expect "a heading with punctuation and a code span" 0

new_tree
put README.md '[one](docs/a.md#again) [two](docs/a.md#again-1) [three](docs/a.md#again-2)'
put docs/a.md '## Again' '## Again' '## Again'
expect "a heading that repeats gets -1 and -2" 0

new_tree
put README.md '[one](docs/a.md#again-3)'
put docs/a.md '## Again' '## Again'
expect "a repeat that does not exist" 1 "#again-3"

new_tree
put README.md '[x](docs/a.md#section)'
put docs/a.md '<a id="section"></a>' 'text'
expect "an id attribute names a fragment" 0

new_tree
put README.md '[x](docs/a.md#heading-in-a-fence)'
put docs/a.md '```' '# heading in a fence' '```'
expect "a heading inside a fenced block is not a heading" 1 "#heading-in-a-fence"

new_tree
put README.md '[x](docs/code.cc#L10)'
put docs/code.cc 'int main() {}'
expect "a fragment on a file that is not Markdown is not checked" 0

new_tree
put README.md '[x](docs/a.md?plain=1)'
put docs/a.md '# A'
expect "a query string is not part of the path" 0

# --- Nothing to check. ---------------------------------------------------------------------------

new_tree
put notes.txt 'no markdown here'
expect "a tree with no Markdown file" 2 "no Markdown file"

new_tree
put README.md 'no links at all'
expect "a Markdown file with no link" 2 "nothing was checked"

new_tree
put build/README.md '[gone](missing.md)'
put README.md '[a](a.md)'
put a.md 'text'
expect "files under build are not checked" 0 "in 2 file(s)"

finish links_test
