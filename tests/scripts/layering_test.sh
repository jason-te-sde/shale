#!/usr/bin/env bash
# Tests for scripts/check-layering.sh.
#
# Each rule has a fixture that must alarm and, where a neighbouring spelling is legitimate, a
# fixture that must not.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/scripts/lib.sh
. "$here/lib.sh"

# shellcheck disable=SC2034  # read by expect in lib.sh
script="$here/../../scripts/check-layering.sh"
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

# --- The basic pair: a tree with one violation and a clean tree. ----------------------------------

new_tree
put src/core/x.cc '#include <thread>'
expect "core including <thread>" 1 "src/core/x.cc:1:"

new_tree
put src/core/y.cc '#include "util/coding.h"' '#include <vector>'
expect "core including only util and the standard library" 0

# --- include/shale: only shale/* and standard headers. -----------------------------------------

new_tree
put include/shale/a.h '#include <cstdint>' '#include "util/coding.h"'
expect "public header including an internal header" 1 "include/shale/a.h:2:"

new_tree
put include/shale/a.h '#include <unistd.h>'
expect "public header including a POSIX header" 1 "include/shale/a.h:1:"

new_tree
put include/shale/a.h '#include "shale/status.h"' '#include <shale/options.h>' '#include <cstdint>' \
  '#include <stdint.h>' '#include <string_view>'
expect "public header including shale headers and standard headers" 0

# --- src/util: util/*, shale/*, standard headers. ----------------------------------------------

new_tree
put src/util/a.cc '#include "io/env.h"'
expect "util including io" 1 "src/util/a.cc:1:"

new_tree
put src/util/a.cc '#include "core/key.h"'
expect "util including core" 1 "src/util/a.cc:1:"

new_tree
put src/util/a.cc '#include <sys/stat.h>'
expect "util including a POSIX header" 1 "src/util/a.cc:1:"

new_tree
put src/util/a.cc '#include "../io/env.h"'
expect "util reaching io through a relative path" 1 "src/util/a.cc:1:"

new_tree
put src/util/a.cc '#include "util/coding.h"' '#include "shale/options.h"' '#include <atomic>' \
  '#include <thread>' '#include <stdint.h>' '#include <string.h>'
expect "util including util, shale and standard headers" 0

# --- src/io: util/*, io/*, shale/*, standard and POSIX headers. --------------------------------

new_tree
put src/io/a.cc '#include "core/key.h"'
expect "io including core" 1 "src/io/a.cc:1:"

new_tree
put src/io/a.cc '#include "db/db_impl.h"'
expect "io including db" 1 "src/io/a.cc:1:"

new_tree
put src/io/a.cc '#include "util/coding.h"' '#include "io/env.h"' '#include "shale/status.h"' \
  '#include <unistd.h>' '#include <sys/stat.h>' '#include <fcntl.h>' '#include <thread>' \
  '#include <dirent.h>'
expect "io including util, io, shale, POSIX and standard headers" 0

# --- src/core: util/*, core/*, shale/status.h, standard headers, minus the banned ones. --------

new_tree
put src/core/a.cc '#include "io/env.h"'
expect "core including io" 1 "src/core/a.cc:1:"

new_tree
put src/core/a.cc '#include "shale/options.h"'
expect "core including a shale header other than status.h" 1 "src/core/a.cc:1:"

new_tree
put src/core/a.cc '#include <unistd.h>'
expect "core including a POSIX header" 1 "src/core/a.cc:1:"

new_tree
put src/core/a.cc '#include "shale/status.h"' '#include "core/key.h"' '#include "util/coding.h"' \
  '#include <string_view>' '#include <cstdint>' '#include <algorithm>'
expect "core including shale/status.h, core, util and standard headers" 0

for header in thread mutex shared_mutex condition_variable atomic chrono fstream cstdio iostream \
  future random stdio.h; do
  new_tree
  put src/core/a.cc '#include <vector>' "#include <$header>"
  expect "core including <$header>" 1 "src/core/a.cc:2:" "<$header>"
done

# --- src/db: anything. --------------------------------------------------------------------------

new_tree
put src/db/a.cc '#include "core/key.h"' '#include "io/env.h"' '#include "util/coding.h"' \
  '#include "db/version_set.h"' '#include "shale/options.h"' '#include <thread>' '#include <mutex>' \
  '#include <unistd.h>'
expect "db including anything under src and include" 0

# --- No output to stdout or stderr from src/ or include/. --------------------------------------

for token in 'std::cout << 1;' 'std::cerr << 1;' 'printf("x");' 'std::printf("x");' \
  'fprintf(stderr, "x");' 'puts("x");' 'puts ("x");'; do
  new_tree
  put src/db/a.cc '#include <string>' "void F() { $token }"
  expect "db writing with '$token'" 1 "src/db/a.cc:2:"
done

new_tree
put include/shale/a.h 'inline void F() { std::cout << 1; }'
expect "public header writing to std::cout" 1 "include/shale/a.h:1:"

new_tree
put src/io/check.cc 'void F() { fprintf(stderr, "x"); }'
expect "a check.cc outside src/util" 1 "src/io/check.cc:1:"

new_tree
put src/util/check.cc '#include <cstdio>' 'void F() { std::fprintf(stderr, "boom"); }'
expect "src/util/check.cc printing before it aborts" 0

new_tree
put src/util/a.cc 'int F(char* b) { return snprintf(b, 8, "%06d", 1); }' \
  'int G(char* b, va_list a) { return vsnprintf(b, 8, "%d", a); }' \
  '// Never call printf( or puts( from here; std::cout is out too.' \
  'const char* kName = "printf";'
expect "snprintf, a comment and a string that merely name the banned calls" 0

# --- Two violations in one file are both reported, in path:line: form. -------------------------

new_tree
put src/core/a.cc '#include <thread>' '#include "util/coding.h"' '#include <mutex>'
expect "core with two banned headers" 1 "src/core/a.cc:1:" "src/core/a.cc:3:"

# --- A checker that finds nothing to check must not report success. ----------------------------

new_tree
expect "a tree with neither src/ nor include/" 2 "check-layering"

finish layering_test
