#!/usr/bin/env bash
# Runs the commands of the README's "Try it" again, and compares what they print with the transcript.
#
# The transcript is the block after `<!-- transcript:try-it -->` in README.md: lines that start with
# "$ " are commands, and every other line is the output of the command above it. The commands are run
# in order, in a directory of their own, with `shalectl` standing for the binary under test, and the
# output of the run, with the commands in front of it, must equal the block. The README's own
# directory, /tmp/shale-demo, is replaced by a new one for the run and put back in what is compared.
# `echo $?` shows the status of the command above it, as it does in a terminal. The file numbers in the
# transcript are a function of the sequence of commands, so a change to what an open allocates shows up
# here as a difference.
#
# Why the README's sequence is what it is. An open that recovers a log writes a level-0 table, and the
# open that makes the fourth one reaches l0_compaction_trigger, so the database starts a compaction on
# its background thread (a trivial move) while the command goes on to print. Whether the move has
# finished when `stats` reads the levels is a race, and an earlier sequence, with four writes and a
# `stats` at the end, printed "Level 0: 4 files" on two runs in three and "Level 0: 3 files, Level 1:
# 1 files" on the rest. The sequence now has three writes, so no open sees more than three level-0
# tables, no background work starts, and the output cannot vary. This test refuses a transcript that
# shows four, so that an edit of the README that brings the race back fails here, with this reason, and
# not at random in CI.
#
# Usage: readme_transcript_test.sh <path to the shalectl binary>
# shellcheck disable=SC2016,SC2028  # the script written below is text, with its own quoting
set -uo pipefail
export LC_ALL=C

here="$(cd "$(dirname "$0")" && pwd)"
readme="$here/../../README.md"
shalectl="${1:?usage: readme_transcript_test.sh <path to the shalectl binary>}"
[ -x "$shalectl" ] || {
  echo "readme_transcript_test: $shalectl is not an executable" >&2
  exit 2
}
# The link below must not depend on the directory this runs in.
shalectl="$(cd "$(dirname "$shalectl")" && pwd)/$(basename "$shalectl")"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/readme_transcript.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/bin"
ln -s "$shalectl" "$tmp/bin/shalectl"

# The block: from the fence after the marker to the closing fence.
awk '
  /^<!-- transcript:try-it -->$/ { marked = 1; next }
  marked && /^```/ { if (inside) exit; inside = 1; next }
  marked && inside { print }
' "$readme" >"$tmp/expected"
if [ ! -s "$tmp/expected" ]; then
  echo "readme_transcript_test: README.md has no non-empty block after '<!-- transcript:try-it -->'" >&2
  exit 1
fi

# The script that makes the same transcript.
{
  echo 'run() { printf "\$ %s\n" "$*"; "$@" 2>&1; }'
  while IFS= read -r line; do
    case "$line" in
      '$ echo $?')
        echo 'printf "\$ echo \$?\n%s\n" "$status"'
        ;;
      '$ '*)
        command="${line#\$ }"
        echo "run ${command//\/tmp\/shale-demo/$tmp/demo}"
        echo 'status=$?'
        ;;
    esac
  done <"$tmp/expected"
} >"$tmp/script.sh"

# The levels the transcript shows. Four files in level 0 is the trigger: see the top of this file.
if grep -E 'Level 0: ([4-9]|[1-9][0-9]+) files' "$tmp/expected" >/dev/null; then
  echo "readme_transcript_test: the transcript shows four or more files in level 0, which starts a" >&2
  echo "compaction in the background of the command that prints it, so its output is not the same on" >&2
  echo "every run. Shape the sequence of commands so that no open sees more than three level-0 tables." >&2
  exit 1
fi

commands="$(grep -c '^run ' "$tmp/script.sh")"
if [ "$commands" -lt 5 ]; then
  echo "readme_transcript_test: only $commands commands found in the transcript" >&2
  exit 1
fi

(cd "$tmp" && PATH="$tmp/bin:$PATH" bash "$tmp/script.sh") 2>&1 | sed "s#$tmp/demo#/tmp/shale-demo#g" >"$tmp/actual"

if ! diff -u "$tmp/expected" "$tmp/actual"; then
  echo "readme_transcript_test: the README's transcript is not what its commands print" >&2
  exit 1
fi
echo "readme_transcript_test: $commands commands print what the README says"
