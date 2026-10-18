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

# The CRC32C code needs the compiler's ARM intrinsics header. It is neither a standard header nor a
# POSIX one, so it is allowed in util by name; the layers that must stay free of the platform are
# not given it.
new_tree
put src/util/a.cc '#include <arm_acle.h>'
expect "util including the compiler's ARM intrinsics header" 0

new_tree
put src/core/a.cc '#include <arm_acle.h>'
expect "core including the compiler's ARM intrinsics header" 1 "src/core/a.cc:1:"

new_tree
put include/shale/a.h '#include <arm_acle.h>'
expect "public header including the compiler's ARM intrinsics header" 1 "include/shale/a.h:1:"

new_tree
put src/util/a.cc '#include <arm_neon.h>'
expect "util including a compiler header that is not on the list" 1 "src/util/a.cc:1:"

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

# Comment lines do not run. A block comment may say what is banned; code after the comment may not.

new_tree
put src/util/a.cc '/* Never call printf( or puts( from here; std::cout is out too. */' \
  '/**' \
  ' * Writes through std::cerr, fprintf( and puts( are all banned in library code.' \
  ' *' \
  ' */' \
  '/* A comment that goes on over' \
  ' * several lines and mentions printf( on the second one. */' \
  '// Also a line comment: std::cout.'
expect "block and line comments that merely name the banned calls" 0

new_tree
put src/util/a.cc 'void F(int* out) {' '  *out = printf("x");' '}'
expect "a line starting with a dereference is code, not a comment" 1 "src/util/a.cc:2:"

new_tree
put src/util/a.cc '/* a comment */ printf("x");'
expect "code after a block comment closes on the same line" 1 "src/util/a.cc:1:"

new_tree
put src/util/a.cc '/*' ' * a comment' ' */ puts("x");'
expect "code after the closing */ of a multi-line comment" 1 "src/util/a.cc:3:"

# --- No `throw` under src/. ---------------------------------------------------------------------

for token in 'throw std::runtime_error("x");' 'if (bad) throw 1;' '} catch (...) { throw; }'; do
  new_tree
  put src/db/a.cc '#include <string>' "void F(bool bad) { $token }"
  expect "db containing '$token'" 1 "src/db/a.cc:2:" "may throw"
done

new_tree
put src/util/a.cc 'void F() { throw 1; }'
expect "util throwing" 1 "src/util/a.cc:1:"

new_tree
put src/core/a.cc 'void F() { throw 1; }'
expect "core throwing" 1 "src/core/a.cc:1:"

new_tree
put src/io/a.cc '// It must not throw, and nothing here throws.' \
  '/// An iterator that does not throw.' \
  '/* a block comment that says throw */' \
  'int F() { return 1; }  // never throw from here' \
  'const char* kName = "rethrow_exception";' \
  'void G() noexcept {}' \
  'void Rethrown() {}' \
  'void RunsThrowawayTask() {}'
expect "comments, a trailing comment and names that merely contain the word throw" 0

new_tree
put tests/a_test.cc '#include <string>' 'void F() { throw 1; }'
put src/util/a.cc '#include <vector>'
expect "throw in a test, which this ban does not cover" 0

new_tree
put include/shale/a.h 'inline void F() { throw 1; }'
expect "a public header throwing" 1 "include/shale/a.h:1:" "may throw"

new_tree
put include/shale/a.h '/// It does not throw.' 'inline int F() { return 1; }  // never throw here'
expect "a public header that only says it does not throw" 0

# The word in a string or in a comment on a code line is not code; code after them is.

new_tree
put src/util/a.cc 'const char* kMsg = "never throw, and never call rand() or std::random_device";' \
  'const char* kEscaped = "a \" throw \" b";' \
  'int a = 1; /* throw */ int b = 2;' \
  'const char* kInclude = "#include <random>";'
put tests/a_test.cc 'const char* kMsg = "std::rand() is banned";'
expect "banned words in strings and in one-line block comments" 0

new_tree
put src/io/a.cc '/**' ' * It must not throw, and never calls std::rand() or std::random_device.' ' *' ' */' \
  'int F() { return 1; }'
put tests/a_test.cc '/*' ' * std::rand() is banned.' ' */'
expect "the starred lines of a block comment that name what is banned" 0

new_tree
put src/util/a.cc 'void F() { Log("http://example.com"); throw 1; }'
expect "a throw after a string that holds two slashes" 1 "src/util/a.cc:1:"

new_tree
put src/util/a.cc 'void F() { Log("x"); throw 1; }'
expect "a throw after a string" 1 "src/util/a.cc:1:"

new_tree
put src/util/a.cc 'void F() { /* a comment */ throw 1; }'
expect "a throw after a one-line block comment" 1 "src/util/a.cc:1:"

new_tree
put tests/a_test.cc 'void F() { Log("x"); int y = std::rand(); }'
put src/util/a.cc '#include <vector>'
expect "std::rand() after a string" 1 "tests/a_test.cc:1:"

# --- No unseeded randomness under include/, src/, tests/ or tools/. -------------------------------

for token in 'int x = std::rand();' 'std::srand(1);' 'std::random_device rd;' 'int x = rand();' \
  'srand(7);' 'unsigned x = ::rand ();'; do
  for dir in src/db src/util include/shale tests/db tools; do
    new_tree
    ext=cc
    [ "$dir" = include/shale ] && ext=h
    put "$dir/a.$ext" '#include <string>' "void F() { $token }"
    [ -d "$root/src" ] || put src/util/ok.cc '#include <vector>'
    expect "$dir containing '$token'" 1 "$dir/a.$ext:2:" "shale::Random"
  done
done

for dir in src/db src/util src/io include/shale tests/db tools; do
  new_tree
  ext=cc
  [ "$dir" = include/shale ] && ext=h
  put "$dir/a.$ext" '#include <random>'
  [ -d "$root/src" ] || put src/util/ok.cc '#include <vector>'
  expect "$dir including <random>" 1 "$dir/a.$ext:1:" "<random> is banned"
done

new_tree
put tests/a_test.cc '  #  include   <random>'
put src/util/a.cc '#include <vector>'
expect "an #include <random> spelled with extra spaces" 1 "tests/a_test.cc:1:"

new_tree
put src/util/a.cc '#include <vector>'
put tests/a_test.cc '#include "util/random.h"' '// std::rand() and std::random_device are banned; use shale::Random.' \
  'int operand(int x) { return x; }' 'int y = operand(1);' 'shale::Random rnd(301);' \
  'int z = rnd.Uniform(5); int w = rnd.rand();' 'int strand(int);' 'int Rand(int) { return 0; }' \
  'int v = 1;  // std::rand and <random> in a trailing comment'
put tools/a.cc '// #include <random>' '/// Not <random>: a Random has a seed.'
expect "Random, comments and names that merely contain rand" 0

new_tree
put src/core/a.cc '#include <vector>' '#include <random>'
expect "<random> in src/core is reported once, by the include rule" 1 "src/core/a.cc:2:" \
  "src/core must not include <random>" "1 violation(s) in 1 files"

new_tree
put src/util/a.cc '#include <vector>'
put fuzz/a.cc '#include <random>'
put bench/a.cc 'int x = rand();'
expect "fuzz/ and bench/ are not scanned" 0

new_tree
put tests/a_test.cc '#include "util/coding.h"' '#include <thread>' 'void F() { printf("x"); }'
put src/util/a.cc '#include <vector>'
expect "tests and tools are held to the randomness ban only, not to the include and output rules" 0

# --- Only the four layers exist under src/. -------------------------------------------------------

new_tree
put src/sim/a.cc '#include <vector>'
expect "a source file in a directory that is no layer" 1 "src/sim/a.cc:1:" "unknown layer"

new_tree
put src/util/a.cc '#include <vector>'
put src/main.cc 'int main() { return 0; }'
expect "a source file directly under src/" 1 "src/main.cc:1:" "unknown layer"

new_tree
put src/util/a.cc '#include <vector>'
put src/CMakeLists.txt 'add_library(x a.cc)'
put src/sim/CMakeLists.txt 'add_library(y b.cc)'
expect "files under src/ that are not C++ sources" 0

# --- Two violations in one file are both reported, in path:line: form. -------------------------

new_tree
put src/core/a.cc '#include <thread>' '#include "util/coding.h"' '#include <mutex>'
expect "core with two banned headers" 1 "src/core/a.cc:1:" "src/core/a.cc:3:"

# --- A checker that finds nothing to check must not report success. ----------------------------

new_tree
expect "a tree with neither src/ nor include/" 2 "check-layering"

finish layering_test
