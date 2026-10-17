#include "util/check.h"

#include <gtest/gtest.h>

#include <string>

namespace shale {
namespace {

TEST(CheckDeathTest, FailureNamesFileConditionAndMessage) {
  EXPECT_DEATH(SHALE_CHECK(1 == 2, "boom"), "check_test.cc:[0-9]+.*1 == 2.*boom");
}

TEST(CheckDeathTest, FailureIsReportedAsASingleLine) {
  // One line, so that a log scraper or a test runner that keeps only the last line of stderr still
  // sees the file, the condition and the reason.
  EXPECT_DEATH(SHALE_CHECK(false, "reason"), "^shale: check failed: [^\n]*reason\n$");
}

TEST(CheckTest, PassingCheckEvaluatesOnce) {
  int n = 0;
  SHALE_CHECK(++n == 1, "x");
  EXPECT_EQ(n, 1);
}

TEST(CheckTest, PassingCheckDoesNotEvaluateTheMessage) {
  // Callers may build the message with string operations; a check that holds must not pay for it.
  int built = 0;
  const auto message = [&built] {
    built++;
    return std::string("expensive");
  };
  SHALE_CHECK(true, message());
  EXPECT_EQ(built, 0);
}

TEST(CheckTest, ChecksCompileAsASingleStatement) {
  // A macro that expands to more than one statement would pair the else with the wrong if.
  bool reached_else = false;
  if (false)
    SHALE_CHECK(true, "unreachable");
  else
    reached_else = true;
  EXPECT_TRUE(reached_else);
}

}  // namespace
}  // namespace shale
