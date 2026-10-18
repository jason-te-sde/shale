#!/usr/bin/env bash
# scripts/check-layering.sh [root]
#
# Enforces the layering rules on the #include lines under include/ and src/, the rule that library
# code never writes to stdout or stderr, and two bans on what code says: no `throw` under include/ and
# src/, and no unseeded randomness under include/, src/, tests/ and tools/. `root` defaults to the
# repository root.
#
#   include/shale/*  shale/*, standard headers
#   src/util/*       util/*, shale/*, standard headers, <arm_acle.h>
#   src/io/*         util/*, io/*, shale/*, standard and POSIX headers
#   src/core/*       util/*, core/*, shale/status.h, standard headers, and none of the headers
#                    that bring in threads, clocks, files, streams or randomness
#   src/db/*         anything
#   src/<other>/*    not allowed: a directory that is not one of the four layers has no include
#                    rule, so the check would silently skip it
#
# "Standard" means a C++ library header (no extension) or a C library header such as <stdint.h>.
# Any other angle-bracket header, <unistd.h> and <sys/stat.h> among them, is a POSIX header. The one
# exception is <arm_acle.h>, the compiler's ARM intrinsics header, which src/util/crc32c.cc uses
# for the hardware CRC32C instructions; src/util may include it by name and no other layer is
# given it (src/io and src/db may include anything anyway).
#
# The output rule bans std::cout, std::cerr, printf(, fprintf( and puts( under include/ and src/,
# except in src/util/check.cc, which prints its message right before aborting. snprintf( is fine,
# and so is a line that is only a comment (//, /* ... */, or the lines in between). A violation is
# printed as `path:line: rule`.
#
# The throw ban: nothing under include/ or src/ may contain the word `throw` in code, since errors are
# returned as a Status.
#
# The randomness ban, under include/, src/, tests/ and tools/: std::rand, std::srand, rand(, srand(,
# std::random_device and `#include <random>` (whose distributions are implementation-defined, so a
# seed would not reproduce across standard libraries). All randomness is shale::Random with an
# explicit seed. src/core's own ban on <random> is reported by the include rule above, and not twice.
# tests/ and tools/ are scanned for this ban only: their includes and their output are theirs.
# Both bans read one line at a time. A line that is only a comment does not count, and on any other line
# the string literals, a block comment that opens and closes on the line, and a `//` comment are cut out
# before the line is read, so the word in a message or in a trailing comment is not code. The lines inside
# a multi-line block comment count as comments only when they start with `*`, as in Doxygen and Google
# style; an unstarred one that names a banned call is reported, which is the safe way to be wrong.
#
# Exit status: 0 clean, 1 violations found, 2 nothing to check (a checker that checks nothing must
# not say "ok").
set -uo pipefail
export LC_ALL=C

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$root" 2>/dev/null || {
  echo "check-layering: cannot enter $root" >&2
  exit 2
}

dirs=()
[ -d include ] && dirs+=(include)
[ -d src ] && dirs+=(src)
if [ "${#dirs[@]}" -eq 0 ]; then
  echo "check-layering: neither include/ nor src/ exists under $root" >&2
  exit 2
fi
# Scanned for the randomness ban only.
[ -d tests ] && dirs+=(tests)
[ -d tools ] && dirs+=(tools)

violations=0
report() {
  printf '%s:%s: %s\n' "$1" "$2" "$3"
  violations=$((violations + 1))
}

layer_of() {
  case "$1" in
    include/*) echo public ;;
    src/util/*) echo util ;;
    src/io/*) echo io ;;
    src/core/*) echo core ;;
    src/db/*) echo db ;;
    *) echo unknown ;;
  esac
}

layer_rule() {
  case "$1" in
    public) echo "include/shale may include only shale/* and standard headers" ;;
    util) echo "src/util may include only util/*, shale/* and standard headers" ;;
    io) echo "src/io may include only util/*, io/*, shale/*, standard and POSIX headers" ;;
    core) echo "src/core may include only util/*, core/*, shale/status.h and standard headers" ;;
  esac
}

# Project headers a layer may include, spelled as the path under include/ or src/.
allowed_project_header() {
  case "$1:$2" in
    *..*) return 1 ;; # "util/../io/x.h" would slip past the prefix test
    public:shale/*) return 0 ;;
    util:util/* | util:shale/*) return 0 ;;
    io:util/* | io:io/* | io:shale/*) return 0 ;;
    core:util/* | core:core/* | core:shale/status.h) return 0 ;;
    *) return 1 ;;
  esac
}

is_standard_header() {
  case "$1" in
    */*) return 1 ;;
    *.h)
      case "$1" in
        assert.h | complex.h | ctype.h | errno.h | fenv.h | float.h | inttypes.h | iso646.h | \
          limits.h | locale.h | math.h | setjmp.h | signal.h | stdarg.h | stdbool.h | stddef.h | \
          stdint.h | stdio.h | stdlib.h | string.h | tgmath.h | time.h | uchar.h | wchar.h | \
          wctype.h) return 0 ;;
        *) return 1 ;;
      esac
      ;;
    *) return 0 ;;
  esac
}

# A compiler header that gives access to CPU instructions. It is not part of any operating system's
# interface, so it does not count as POSIX, but it is no standard header either: the list is the
# complete set src/util is allowed.
is_intrinsics_header() {
  case "$1" in
    arm_acle.h) return 0 ;;
    *) return 1 ;;
  esac
}

# stdio.h sits on the list as the C spelling of <cstdio>.
core_banned_header() {
  case "$1" in
    thread | mutex | shared_mutex | condition_variable | atomic | chrono | fstream | cstdio | \
      iostream | future | random | stdio.h) return 0 ;;
    *) return 1 ;;
  esac
}

include_re='^[[:space:]]*#[[:space:]]*include[[:space:]]*(["<])([^">]+)[">]'

check_includes() {
  local file=$1 layer=$2 lineno text kind spec shown top
  case "$layer" in
    db) return ;;
    unknown)
      top="${file#src/}"
      top="${top%%/*}"
      report "$file" 1 \
        "unknown layer '$top': src/ may contain only util, io, core and db; add the layer and its include rules to scripts/check-layering.sh"
      return
      ;;
  esac
  while IFS=: read -r lineno text; do
    [[ $text =~ $include_re ]] || continue
    kind="${BASH_REMATCH[1]}"
    spec="${BASH_REMATCH[2]}"
    if [ "$kind" = '<' ]; then shown="<$spec>"; else shown="\"$spec\""; fi
    if allowed_project_header "$layer" "$spec"; then
      continue
    fi
    if [ "$kind" = '<' ] && is_standard_header "$spec"; then
      if [ "$layer" = core ] && core_banned_header "$spec"; then
        report "$file" "$lineno" \
          "src/core must not include $shown: the core has no threads, clocks, files, streams or randomness"
      fi
      continue
    fi
    if [ "$kind" = '<' ] && [ "$layer" = io ]; then
      continue
    fi
    if [ "$kind" = '<' ] && [ "$layer" = util ] && is_intrinsics_header "$spec"; then
      continue
    fi
    report "$file" "$lineno" "$(layer_rule "$layer"): found $shown"
  done < <(grep -nE '^[[:space:]]*#[[:space:]]*include' "$file")
}

# is_comment_line <line, without its leading whitespace>: true if nothing on the line can run. A
# line that opens a block comment is one only while the comment stays open or closes at the very
# end; "/* x */ printf(" and "*/ puts(" have code after the close. A line that starts with "*"
# followed by anything but a space, such as "*out = printf(", is a dereference, not a comment.
# The check reads one line at a time, so the lines inside a block comment count as comment only
# when they start with "*", as in Doxygen and Google style. An unstarred continuation line that
# names a banned call is reported, which is the safe way to be wrong.
is_comment_line() {
  case "$1" in
    //*) return 0 ;;
    '/*'*)
      case "$1" in
        *'*/') return 0 ;;
        *'*/'*) return 1 ;;
        *) return 0 ;;
      esac
      ;;
    '*' | '* '* | '*/') return 0 ;;
  esac
  return 1
}

output_re='std::(cout|cerr)([^[:alnum:]_]|$)|(^|[^[:alnum:]_])(printf|fprintf|puts)[[:space:]]*\('

check_output() {
  local file=$1 lineno text trimmed
  [ "$file" = src/util/check.cc ] && return
  while IFS=: read -r lineno text; do
    trimmed="${text#"${text%%[![:space:]]*}"}"
    trimmed="${trimmed%"${trimmed##*[![:space:]]}"}"
    is_comment_line "$trimmed" && continue
    report "$file" "$lineno" "library code must not write to stdout or stderr"
  done < <(grep -nE "$output_re" "$file")
}

# The bans on what code says. `throw` is banned under include/ and src/; the randomness bans hold
# wherever C++ is scanned.
throw_re='(^|[^[:alnum:]_])throw([^[:alnum:]_]|$)'
random_re='std::(rand|srand|random_device)([^[:alnum:]_]|$)|(^|[^[:alnum:]_.>])s?rand[[:space:]]*\('
random_include_re='^[[:space:]]*#[[:space:]]*include[[:space:]]*<random>'

# code_of <line>: the line with its string literals, its one-line block comments and its // comment
# cut out. Only lines that a ban has found are passed through it, so the cost is nothing.
code_of() {
  sed -E -e 's/"([^"\\]|\\.)*"//g' -e 's,/\*([^*]|\*[^/])*\*/,,g' -e 's,//.*$,,' <<<"$1"
}

# scan_banned <file> <regex> <what is wrong>: reports each line of the file whose code matches.
scan_banned() {
  local file=$1 re=$2 message=$3 lineno text trimmed code
  while IFS=: read -r lineno text; do
    trimmed="${text#"${text%%[![:space:]]*}"}"
    is_comment_line "$trimmed" && continue
    code="$(code_of "$text")"
    [[ $code =~ $re ]] || continue
    report "$file" "$lineno" "$message"
  done < <(grep -nE "$re" "$file")
}

check_banned() {
  local file=$1
  case "$file" in
    include/* | src/*)
      scan_banned "$file" "$throw_re" "nothing under include/ or src/ may throw: errors are returned as a Status"
      ;;
  esac
  scan_banned "$file" "$random_re" \
    "std::rand, srand and std::random_device are banned: all randomness is shale::Random with an explicit seed"
  case "$file" in
    src/core/*) ;; # check_includes has reported <random> here already
    *)
      scan_banned "$file" "$random_include_re" \
        "<random> is banned: its distributions are implementation-defined, so a seed would not reproduce across standard libraries"
      ;;
  esac
}

checked=0
while IFS= read -r file; do
  checked=$((checked + 1))
  case "$file" in
    include/* | src/*)
      check_includes "$file" "$(layer_of "$file")"
      check_output "$file"
      ;;
  esac
  check_banned "$file"
done < <(find "${dirs[@]}" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' \) | sort)

if [ "$checked" -eq 0 ]; then
  echo "check-layering: no C++ files under ${dirs[*]} in $root" >&2
  exit 2
fi
if [ "$violations" -ne 0 ]; then
  echo "check-layering: $violations violation(s) in $checked files" >&2
  exit 1
fi
echo "check-layering: ok ($checked files)"
