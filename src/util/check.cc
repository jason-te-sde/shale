#include "util/check.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace shale::internal {

void CheckFailed(const char* file, int line, const char* cond, std::string_view msg) {
  // The one place in the library that writes to stderr. The message is a single line so that a log
  // reader that keeps only the last line of a crash still has the location, the condition and the
  // reason. stderr is unbuffered, so nothing is lost to the abort that follows.
  std::fprintf(stderr, "shale: check failed: %s:%d: %s: %.*s\n", file, line, cond,
               static_cast<int>(msg.size()), msg.data());
  std::abort();
}

}  // namespace shale::internal
