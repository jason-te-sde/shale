#!/usr/bin/env bash
# Runs the shalectl binary against databases it makes in a directory of its own, and checks the
# exit status and the output of every command: what it prints, what it refuses, and that `verify`
# and the dumps leave the files of a database as they found them.
#
# Usage: shalectl_test.sh <path to the shalectl binary>
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/scripts/lib.sh
. "$here/../scripts/lib.sh"

shalectl="${1:?usage: shalectl_test.sh <path to the shalectl binary>}"
if [ ! -x "$shalectl" ]; then
  echo "shalectl_test: $shalectl is not an executable" >&2
  exit 2
fi
tmp="$(mktemp -d "${TMPDIR:-/tmp}/shalectl_test.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

# check <description> <exit status> <stdout, or * for any> <argument>...
# Runs shalectl with the arguments. The status must be as wanted and, unless the third argument is
# *, standard output must be exactly the text given. Standard error is kept in $tmp/err for
# err_has, and standard output in $tmp/out for out_has.
check() {
  local description=$1 want_status=$2 want_out=$3 got
  shift 3
  cases=$((cases + 1))
  "$shalectl" "$@" >"$tmp/out" 2>"$tmp/err"
  got=$?
  if [ "$got" -ne "$want_status" ]; then
    fail "$description: exit status $got, wanted $want_status" \
      "$(printf 'stdout:\n%s\nstderr:\n%s' "$(cat "$tmp/out")" "$(cat "$tmp/err")")"
    return
  fi
  if [ "$want_out" != '*' ]; then
    printf '%s' "$want_out" >"$tmp/want"
    if ! cmp -s "$tmp/want" "$tmp/out"; then
      fail "$description: standard output differs" \
        "$(printf 'wanted:\n%s\ngot:\n%s' "$want_out" "$(cat "$tmp/out")")"
    fi
  fi
}

# out_has / err_has <description> <fragment>: the output of the last check has the fragment.
out_has() {
  cases=$((cases + 1))
  if ! grep -qF -e "$2" "$tmp/out"; then
    fail "$1: standard output lacks '$2'" "$(cat "$tmp/out")"
  fi
}
err_has() {
  cases=$((cases + 1))
  if ! grep -qF -e "$2" "$tmp/err"; then
    fail "$1: standard error lacks '$2'" "$(cat "$tmp/err")"
  fi
}

# err_lacks <description> <fragment>: standard error of the last check does not have the fragment.
err_lacks() {
  cases=$((cases + 1))
  if grep -qF -e "$2" "$tmp/err"; then
    fail "$1: standard error has '$2'" "$(cat "$tmp/err")"
  fi
}

# err_is_empty <description>: the last check wrote nothing to standard error.
err_is_empty() {
  cases=$((cases + 1))
  if [ -s "$tmp/err" ]; then
    fail "$1: standard error is not empty" "$(cat "$tmp/err")"
  fi
}

# expect_eq <description> <wanted> <got>
expect_eq() {
  cases=$((cases + 1))
  if [ "$2" != "$3" ]; then
    fail "$1: wanted '$2', got '$3'"
  fi
}

# files_of <directory> <suffix>: the paths of the directory's files that end in the suffix.
files_of() { find "$1" -maxdepth 1 -type f -name "*$2" | sort; }

# count_of <directory> <suffix>: how many of them there are.
count_of() { files_of "$1" "$2" | wc -l | tr -d ' '; }

# fingerprint <directory>: every file's name, size and checksum, which is what a command that must
# not change a database is compared with before and after.
fingerprint() {
  (cd "$1" && find . -type f | sort | while IFS= read -r file; do
    printf '%s ' "$file"
    cksum <"$file"
  done)
}

# flip_byte <file> <offset>: changes one bit of the byte at the offset, in place.
flip_byte() {
  local byte
  byte="$(dd if="$1" bs=1 skip="$2" count=1 2>/dev/null | od -An -tu1 | tr -d ' ')"
  # shellcheck disable=SC2059  # the format is an octal escape built on the line above
  printf "$(printf '\\%03o' $((byte ^ 64)))" | dd of="$1" bs=1 seek="$2" conv=notrunc 2>/dev/null
}

# flip_middle_byte <file>: flips the byte in the middle of the file.
flip_middle_byte() {
  local size
  size="$(wc -c <"$1" | tr -d ' ')"
  flip_byte "$1" $((size / 2))
}

# --- The first database: puts, gets, scans, and the exit codes. -----------------------------------

db="$tmp/db"
check "put with --create" 0 '' --db="$db" --create put a 1
err_is_empty "put with --create"
check "get prints the value and a newline" 0 $'1\n' --db="$db" get a
check "get of a key that is not there" 1 '' --db="$db" get missing
check "scan prints key, tab, value" 0 $'a\t1\n' --db="$db" scan
check "verify of a database that works" 0 $'ok\n' --db="$db" verify

check "flags may come after the command" 0 '' put --db="$db" b 2
check "flags may come between the words" 0 '' put c --db="$db" 3
check "delete" 0 '' --db="$db" delete b
check "a deleted key is not found" 1 '' --db="$db" get b
check "scan skips what was deleted" 0 $'a\t1\nc\t3\n' --db="$db" scan
check "scan --from starts at the first key not before it" 0 $'c\t3\n' --db="$db" scan --from=b
check "scan --from=c starts at c" 0 $'c\t3\n' --db="$db" scan --from=c
check "scan --limit stops after that many" 0 $'a\t1\n' --db="$db" scan --limit=1
check "scan --limit=0 shows nothing" 0 '' --db="$db" scan --limit=0
check "scan --from past the last key shows nothing" 0 '' --db="$db" scan --from=z
check "put overwrites" 0 '' --db="$db" put a 11
check "get sees the overwrite" 0 $'11\n' --db="$db" get a
check "delete of a key that is not there" 0 '' --db="$db" delete never-written

check "stats" 0 '*' --db="$db" stats
out_has "stats shows the levels" "Level 0:"

# --- Keys and values are shown with EscapeBytes, and only so. ------------------------------------

check "put a key with a backslash" 0 '' --db="$db" put 'back\slash' 'v w'
check "a backslash is doubled in scan" 0 '*' --db="$db" scan --from='back'
out_has "scan doubles the backslash" $'back\\\\slash\tv w'
check "put a key with a tab and a value with a newline" 0 '' --db="$db" put $'tab\there' $'line\nbreak'
check "get shows a newline in a value as hex" 0 $'line\\x0abreak\n' --db="$db" get $'tab\there'
check "scan shows a tab in a key as hex" 0 '*' --db="$db" scan --from=tab --limit=1
out_has "scan escapes the tab in the key" $'tab\\x09here\tline\\x0abreak'
check "an empty value" 0 '' --db="$db" put empty ''
check "get of an empty value" 0 $'\n' --db="$db" get empty
check "a key that starts with dashes, after --" 0 '' --db="$db" put -- --dashes v
check "get of it" 0 $'v\n' --db="$db" get -- --dashes
check "a high byte is shown as hex" 0 '' --db="$db" put $'\xc3\xa9' x
check "scan from a high byte" 0 '*' --db="$db" scan --from=$'\xc3'
out_has "scan shows the bytes of the key" $'\\xc3\\xa9\tx'

# --- Usage errors are exit status 2, and say how to use the tool. --------------------------------

check "--bogus-command" 2 '' --bogus-command
err_has "--bogus-command" "usage"
check "no arguments" 2 ''
err_has "no arguments" "usage"
check "an unknown command" 2 '' --db="$db" frobnicate
err_has "an unknown command" "frobnicate"
check "a command that needs a database, without --db" 2 '' get a
err_has "get without --db" "--db"
check "get without a key" 2 '' --db="$db" get
check "get with two keys" 2 '' --db="$db" get a b
check "put without a value" 2 '' --db="$db" put a
check "put with three words" 2 '' --db="$db" put a 1 2
check "delete without a key" 2 '' --db="$db" delete
check "scan with a word" 2 '' --db="$db" scan extra
check "stats with a word" 2 '' --db="$db" stats extra
check "verify with a word" 2 '' --db="$db" verify extra
check "a flag the command does not take" 2 '' --db="$db" stats --from=a
check "a flag nobody takes" 2 '' --db="$db" stats --bogus=1
check "--limit that is not a number" 2 '' --db="$db" scan --limit=many
check "--limit that is negative" 2 '' --db="$db" scan --limit=-1
check "--limit that is empty" 2 '' --db="$db" scan --limit=
check "--create that is not a boolean" 2 '' --db="$db" --create=maybe get a
check "an empty flag name" 2 '' --db="$db" --=x get a
check "--db without a value" 2 '' --db get a
check "--from without a value" 2 '' --db="$db" scan --from
check "--limit without a value" 2 '' --db="$db" scan --limit
check "--db= with no path" 2 '' --db= get a
err_has "--db= with no path" "--db"
check "--db= with no path, for verify" 2 '' --db= verify
check "flags the dumps do not take" 2 '' --db="$db" dump-sst "$tmp/none.sst"
check "dump-sst without a file" 2 '' dump-sst
check "dump-wal with two files" 2 '' dump-wal a b
check "dump-manifest without a file" 2 '' dump-manifest

# --- What came from the command line is shown escaped in an error, whatever it holds. -----------

esc=$'\x1b'
check "an unknown command with an escape byte" 2 '' --db="$db" "bad${esc}cmd"
err_has "the command is shown escaped" 'bad\x1bcmd'
err_lacks "the command is not shown raw" "$esc"
check "a flag with an escape byte in its name" 2 '' --db="$db" stats "--no${esc}pe=1"
err_has "the flag is shown escaped" 'no\x1bpe'
err_lacks "the flag is not shown raw" "$esc"
check "--limit with an escape byte" 2 '' --db="$db" scan "--limit=${esc}"
err_has "--limit is shown escaped" '\x1b'
err_lacks "--limit is not shown raw" "$esc"
check "--create with an escape byte" 2 '' --db="$db" "--create=${esc}" get a
err_has "--create is shown escaped" '\x1b'
err_lacks "--create is not shown raw" "$esc"
check "an empty flag name with an escape byte" 2 '' --db="$db" "--=${esc}" get a
err_has "the flag is shown escaped" '\x1b'
err_lacks "the empty flag is not shown raw" "$esc"

# --- The usage says what each command does to a database. ----------------------------------------

check "the usage" 2 ''
err_has "the usage says which commands write" "opens the database for writing"
err_has "the usage points at a copy" "on a copy"
err_has "the usage says verify reads no log" "does not read the write-ahead logs"
err_has "the usage says verify needs write access" "write access"
err_has "the usage says verify makes a LOCK" "empty LOCK"
err_has "the usage says what a damaged database is" "exit status 3"
err_has "the usage says how to pass binary keys" "\\xNN"
err_has "the usage says a NUL cannot be passed" "NUL"

# --- A database that is not there is an engine error, and nothing is made for it. ----------------

check "get without --create on a path with no database" 3 '' --db="$tmp/nodb" get a
err_has "get on no database" "Invalid argument"
expect_eq "get makes no directory" "no" "$([ -e "$tmp/nodb" ] && echo yes || echo no)"
check "verify on a path with no database" 3 '' --db="$tmp/nodb" verify
err_has "verify on no database" "nodb"
expect_eq "verify makes no directory" "no" "$([ -e "$tmp/nodb" ] && echo yes || echo no)"
mkdir "$tmp/emptydir"
check "verify on an empty directory" 3 '' --db="$tmp/emptydir" verify
expect_eq "verify makes no file in an empty directory" "" "$(ls -A "$tmp/emptydir")"

# --- flush, and the files the dumps read: a database whose contents are known. -------------------

# Every shalectl command opens the database, and an open turns the write-ahead log it finds into a
# table, so a log holds only what the last command wrote.
db3="$tmp/db3"
check "put" 0 '' --db="$db3" --create put a 1
expect_eq "the write is in a log and in no table" "0" "$(count_of "$db3" .sst)"
expect_eq "the log is the only one" "1" "$(count_of "$db3" .log)"
check "dump-wal of a put" 0 $'batch seq=1 count=1\n  put a 1\n' dump-wal "$(files_of "$db3" .log)"

check "delete" 0 '' --db="$db3" delete a
check "dump-wal of a delete" 0 $'batch seq=2 count=1\n  del a\n' dump-wal "$(files_of "$db3" .log)"

check "flush" 0 '' --db="$db3" flush
err_is_empty "flush"
expect_eq "the writes are in two tables" "2" "$(count_of "$db3" .sst)"
check "the deleted key stays deleted" 1 '' --db="$db3" get a
sst1="$(files_of "$db3" .sst | sed -n 1p)"
sst2="$(files_of "$db3" .sst | sed -n 2p)"
check "dump-sst of the table with the put" 0 $'a seq=1 type=put value=1\n' dump-sst "$sst1"
check "dump-sst of the table with the delete" 0 $'a seq=2 type=del value=\n' dump-sst "$sst2"

manifest="$db3/$(head -n 1 "$db3/CURRENT")"
check "dump-manifest" 0 '*' dump-manifest "$manifest"
out_has "dump-manifest shows the edit" "VersionEdit {"
out_has "dump-manifest shows a table" "add L0 #"

# The dumps read the files as they are, and a file they cannot read is exit status 3.
check "dump-sst of a file that is not there" 3 '' dump-sst "$tmp/none.sst"
err_has "dump-sst of a missing file" "none.sst"
check "dump-sst of a file that is not a table" 3 '' dump-sst "$db3/CURRENT"
err_has "dump-sst of CURRENT" "Corruption"
check "dump-wal of a file that is not there" 3 '' dump-wal "$tmp/none.log"
check "dump-manifest of a file that is not there" 3 '' dump-manifest "$tmp/none.manifest"

# --- verify changes no file of a database that has a LOCK, and finds damage. -----------------------------------------------------------------------

before="$(fingerprint "$db3")"
check "verify of a flushed database" 0 $'ok\n' --db="$db3" verify
check "verify, a second time" 0 $'ok\n' --db="$db3" verify
check "dump-sst" 0 '*' dump-sst "$sst1"
check "dump-manifest" 0 '*' dump-manifest "$manifest"
expect_eq "verify and the dumps changed no file" "$before" "$(fingerprint "$db3")"

# A database that has no LOCK, a copy made without it, is checked, and gets the empty file that
# taking the lock needs.
rm "$db3/LOCK"
check "verify of a database that has no LOCK" 0 $'ok\n' --db="$db3" verify
expect_eq "verify made an empty LOCK" "0" "$(wc -c <"$db3/LOCK" | tr -d ' ')"

db2="$tmp/db2"
check "put in a database to damage" 0 '' --db="$db2" --create put a 1
check "flush it" 0 '' --db="$db2" flush
expect_eq "one table to damage" "1" "$(count_of "$db2" .sst)"
table="$(files_of "$db2" .sst)"
flip_middle_byte "$table"
damaged="$(fingerprint "$db2")"
check "verify of a damaged table" 3 '*' --db="$db2" verify
out_has "verify says Corruption" "Corruption"
out_has "verify names the table" "$(basename "$table")"
expect_eq "verify of a damaged database changed no file" "$damaged" "$(fingerprint "$db2")"
check "get from a damaged table is an error, not a miss" 3 '' --db="$db2" get a
err_has "get from a damaged table" "Corruption"
err_has "get names the damaged table" "$(basename "$table")"
# Nothing is shown: the failure is found where the scan positions on the table. The error names the
# table, which the table's own iterator does not.
check "scan over a damaged table" 3 '' --db="$db2" scan
err_has "scan over a damaged table" "Corruption"
err_has "scan names the damaged table" "$(basename "$table")"
check "dump-sst of a damaged table" 3 '*' dump-sst "$table"
err_has "dump-sst of a damaged table" "Corruption"

# On one stream, the problems come before the line that counts them, as on a terminal.
"$shalectl" --db="$db2" verify >"$tmp/both" 2>&1
cases=$((cases + 1))
case "$(head -n 1 "$tmp/both")" in
  Corruption*) ;;
  *) fail "verify: the first line of the one stream is not a problem" "$(cat "$tmp/both")" ;;
esac
cases=$((cases + 1))
case "$(tail -n 1 "$tmp/both")" in
  *"verify found 1 problem(s)") ;;
  *) fail "verify: the last line of the one stream is not the count" "$(cat "$tmp/both")" ;;
esac

# Output that cannot be written is an error, not a success that lost the output. The reader of the
# pipe is gone before the scan prints, and with SIGPIPE ignored the write fails instead of ending
# the process. A reader that is slow to start could still be there, so the case is tried again, and
# a tool that ignores the failure fails every try.
cases=$((cases + 1))
for _ in 1 2 3; do
  { trap '' PIPE; "$shalectl" --db="$db" scan 2>"$tmp/err"; } | true
  got=${PIPESTATUS[0]}
  [ "$got" -eq 3 ] && break
done
if [ "$got" -ne 3 ]; then
  fail "scan into a closed pipe: exit status $got, wanted 3" "$(cat "$tmp/err")"
fi
cases=$((cases + 1))
if ! grep -qF -e "standard output" "$tmp/err"; then
  fail "scan into a closed pipe: standard error does not say what failed" "$(cat "$tmp/err")"
fi

# A damaged log and a damaged MANIFEST: the dumps say so and exit with status 3.
db4="$tmp/db4"
check "put for a damaged log" 0 '' --db="$db4" --create put a 1
log4="$(files_of "$db4" .log)"
flip_middle_byte "$log4"
check "dump-wal of a damaged log" 3 '*' dump-wal "$log4"
err_has "dump-wal of a damaged log" "Corruption"

manifest4="$db4/$(head -n 1 "$db4/CURRENT")"
flip_middle_byte "$manifest4"
check "dump-manifest of a damaged MANIFEST" 3 '*' dump-manifest "$manifest4"
err_has "dump-manifest of a damaged MANIFEST" "Corruption"
check "verify of a damaged MANIFEST" 3 '*' --db="$db4" verify
out_has "verify of a damaged MANIFEST" "Corruption"

finish shalectl_test
