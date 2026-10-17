#ifndef SHALE_UTIL_CHECK_H_
#define SHALE_UTIL_CHECK_H_

#include <string_view>

namespace shale {
namespace internal {

/// Prints the failed condition with its location and `msg` to stderr, then aborts the process.
/// Reached only through SHALE_CHECK.
[[noreturn]] void CheckFailed(const char* file, int line, const char* cond, std::string_view msg);

}  // namespace internal
}  // namespace shale

/// Aborts the process with `msg` and the location if `cond` is false.
///
/// Unlike assert(), it is active in every build type: it guards invariants whose violation means
/// the data is already wrong or about to be, and continuing would only spread the damage. `cond`
/// is evaluated exactly once; `msg` is evaluated only when the check fails, so it may build a
/// string. Use it for violated internal invariants and misuse by the caller, never to report
/// anything that can happen in a correct program: those return a Status.
#define SHALE_CHECK(cond, msg)                                          \
  do {                                                                  \
    if (!(cond)) [[unlikely]] {                                         \
      ::shale::internal::CheckFailed(__FILE__, __LINE__, #cond, (msg)); \
    }                                                                   \
  } while (false)

#endif  // SHALE_UTIL_CHECK_H_
