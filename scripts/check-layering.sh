#!/usr/bin/env bash
# scripts/check-layering.sh [root]
#
# Enforces the layering rules on the #include lines under include/ and src/, and the rule that
# library code never writes to stdout or stderr. `root` defaults to the repository root.
#
#   include/shale/*  shale/*, standard headers
#   src/util/*       util/*, shale/*, standard headers
#   src/io/*         util/*, io/*, shale/*, standard and POSIX headers
#   src/core/*       util/*, core/*, shale/status.h, standard headers, and none of the headers
#                    that bring in threads, clocks, files, streams or randomness
#   src/db/*         anything
#
# "Standard" means a C++ library header (no extension) or a C library header such as <stdint.h>.
# Any other angle-bracket header, <unistd.h> and <sys/stat.h> among them, is a POSIX header.
#
# The output rule bans std::cout, std::cerr, printf(, fprintf( and puts( under include/ and src/,
# except in src/util/check.cc, which prints its message right before aborting. snprintf( is fine,
# and so is a line that is only a comment. A violation is printed as `path:line: rule`.
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
    *) echo none ;;
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
  local file=$1 layer=$2 lineno text kind spec shown
  case "$layer" in db | none) return ;; esac
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
    report "$file" "$lineno" "$(layer_rule "$layer"): found $shown"
  done < <(grep -nE '^[[:space:]]*#[[:space:]]*include' "$file")
}

output_re='std::(cout|cerr)([^[:alnum:]_]|$)|(^|[^[:alnum:]_])(printf|fprintf|puts)[[:space:]]*\('

check_output() {
  local file=$1 lineno text trimmed
  [ "$file" = src/util/check.cc ] && return
  while IFS=: read -r lineno text; do
    trimmed="${text#"${text%%[![:space:]]*}"}"
    case "$trimmed" in //*) continue ;; esac
    report "$file" "$lineno" "library code must not write to stdout or stderr"
  done < <(grep -nE "$output_re" "$file")
}

checked=0
while IFS= read -r file; do
  checked=$((checked + 1))
  check_includes "$file" "$(layer_of "$file")"
  check_output "$file"
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
