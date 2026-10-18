#ifndef SHALE_TESTKIT_TEST_UTIL_H_
#define SHALE_TESTKIT_TEST_UTIL_H_

// Helpers shared by the tests of every layer. They live in the shale_testkit library, which only
// test targets link.

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "shale/status.h"

namespace shale {

class Random;

/// Succeeds if `s` is OK. Otherwise fails with the text of `s`, so that a test that checks a Status
/// reports what went wrong and not just that something did. Use it through ASSERT_OK / EXPECT_OK.
testing::AssertionResult IsOk(const Status& s);

/// Fails the test, and returns from the test body, unless the Status expression `expr` is OK.
#define ASSERT_OK(expr) ASSERT_TRUE(::shale::IsOk(expr))

/// Fails the test, and carries on, unless the Status expression `expr` is OK.
#define EXPECT_OK(expr) EXPECT_TRUE(::shale::IsOk(expr))

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
