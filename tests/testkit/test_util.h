#ifndef SHALE_TESTKIT_TEST_UTIL_H_
#define SHALE_TESTKIT_TEST_UTIL_H_

// Helpers shared by the tests of every layer. They live in the shale_testkit library, which only
// test targets link.

#include <cstddef>
#include <string>

namespace shale {

class Random;

/// Returns `len` bytes drawn from `*rnd`, each a printable ASCII character (0x20 through 0x7e).
std::string RandomString(Random* rnd, size_t len);

/// Returns `len` bytes drawn from `*rnd`, each of any value from 0x00 through 0xff. It makes
/// tests that round-trip data check that nothing treats a zero byte, a newline or a byte above
/// 0x7f as special.
std::string RandomBinaryString(Random* rnd, size_t len);

/// Creates a new, empty directory named `prefix` followed by random characters under
/// `testing::TempDir()` and returns its path, without a trailing slash. Aborts if the directory
/// cannot be created. The caller removes it; the helper does not, so that a failing test leaves
/// its files behind to be looked at.
std::string NewTempDir(const std::string& prefix);

}  // namespace shale

#endif  // SHALE_TESTKIT_TEST_UTIL_H_
