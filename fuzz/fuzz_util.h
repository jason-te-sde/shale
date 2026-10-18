#ifndef SHALE_FUZZ_FUZZ_UTIL_H_
#define SHALE_FUZZ_FUZZ_UTIL_H_

// What the fuzz targets share: the check that stops a run, and a way to put input bytes where the
// engine reads files from. Nothing here touches the real filesystem.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#include "io/env_util.h"
#include "shale/env.h"

/// Stops the run when `cond` is false. libFuzzer reports the trap as a crash and saves the input,
/// where an assertion that a build can compile out, or a return that a target might ignore, would
/// let a violated invariant go by. The condition is printed first, because the trap says nothing.
#define SHALE_FUZZ_CHECK(cond)                                                            \
  do {                                                                                    \
    if (!(cond)) {                                                                        \
      std::fprintf(stderr, "fuzz check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      __builtin_trap();                                                                   \
    }                                                                                     \
  } while (false)

namespace shale::fuzz {

/// Returns an in-memory Env that holds one file, `name`, whose contents are `data`.
inline std::unique_ptr<Env> EnvWithFile(const std::string& name, std::string_view data) {
  std::unique_ptr<Env> env = NewMemEnv();
  const Status s = WriteStringToFile(env.get(), data, name, /*sync=*/false);
  SHALE_FUZZ_CHECK(s.ok());
  return env;
}

/// Reads every byte of `bytes`, so that AddressSanitizer sees a view that points outside what it
/// should: a decoder that returns a bad pointer is caught only when someone reads through it.
inline void Touch(std::string_view bytes) {
  uint8_t sum = 0;
  for (const char c : bytes) {
    sum = static_cast<uint8_t>(sum ^ static_cast<uint8_t>(c));
  }
  // The write to a volatile is what keeps the compiler from dropping the loads above.
  const volatile uint8_t sink = sum;
  static_cast<void>(sink);
}

}  // namespace shale::fuzz

#endif  // SHALE_FUZZ_FUZZ_UTIL_H_
