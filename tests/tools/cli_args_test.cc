#include "cli_args.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "shale/status.h"
#include "testkit/test_util.h"

namespace shale {
namespace {

using Words = std::vector<std::string>;

// Parses `argv`, which must be accepted.
CliArgs Parse(const Words& argv) {
  CliArgs args;
  EXPECT_OK(ParseCliArgs(argv, &args));
  return args;
}

TEST(CliArgsTest, FlagsBeforeCommand) {
  const CliArgs args = Parse({"--db=x", "put", "k", "v"});
  EXPECT_EQ(args.command, "put");
  EXPECT_EQ(args.positional, (Words{"k", "v"}));
  EXPECT_EQ(args.flags, (std::map<std::string, std::string>{{"db", "x"}}));
}

TEST(CliArgsTest, FlagsAfterCommand) {
  const CliArgs args = Parse({"put", "--db=x", "k", "v"});
  EXPECT_EQ(args.command, "put");
  EXPECT_EQ(args.positional, (Words{"k", "v"}));
  EXPECT_EQ(args.flags, (std::map<std::string, std::string>{{"db", "x"}}));
}

TEST(CliArgsTest, FlagsBetweenAndAfterTheWords) {
  const CliArgs args = Parse({"put", "k", "--db=x", "v", "--create"});
  EXPECT_EQ(args.command, "put");
  EXPECT_EQ(args.positional, (Words{"k", "v"}));
  EXPECT_EQ(args.flags, (std::map<std::string, std::string>{{"db", "x"}, {"create", "true"}}));
}

TEST(CliArgsTest, BareFlagIsTrue) {
  const CliArgs args = Parse({"--create", "get", "k"});
  EXPECT_EQ(args.command, "get");
  EXPECT_EQ(args.positional, (Words{"k"}));
  EXPECT_EQ(args.flags, (std::map<std::string, std::string>{{"create", "true"}}));
}

TEST(CliArgsTest, DoubleDashEndsFlags) {
  const CliArgs args = Parse({"put", "--", "--k", "v"});
  EXPECT_EQ(args.command, "put");
  EXPECT_EQ(args.positional, (Words{"--k", "v"}));
  EXPECT_TRUE(args.flags.empty());
}

TEST(CliArgsTest, DoubleDashMayComeBeforeTheCommand) {
  const CliArgs args = Parse({"--db=x", "--", "put", "--k", "--db=y"});
  EXPECT_EQ(args.command, "put");
  EXPECT_EQ(args.positional, (Words{"--k", "--db=y"}));
  EXPECT_EQ(args.flags, (std::map<std::string, std::string>{{"db", "x"}}));
}

TEST(CliArgsTest, OnlyTheFirstDoubleDashIsTheSeparator) {
  const CliArgs args = Parse({"put", "--", "--", "v"});
  EXPECT_EQ(args.positional, (Words{"--", "v"}));
}

TEST(CliArgsTest, FlagValueMayBeEmptyAndMayHoldEqualsSigns) {
  const CliArgs args = Parse({"scan", "--from=", "--limit=a=b"});
  EXPECT_EQ(args.flags, (std::map<std::string, std::string>{{"from", ""}, {"limit", "a=b"}}));
}

TEST(CliArgsTest, LaterFlagOverridesEarlier) {
  const CliArgs args = Parse({"--db=a", "stats", "--db=b"});
  EXPECT_EQ(args.flags, (std::map<std::string, std::string>{{"db", "b"}}));
}

TEST(CliArgsTest, WordsWithOneDashAreNotFlags) {
  const CliArgs args = Parse({"put", "-k", "-1", "-"});
  EXPECT_EQ(args.command, "put");
  EXPECT_EQ(args.positional, (Words{"-k", "-1", "-"}));
  EXPECT_TRUE(args.flags.empty());
}

TEST(CliArgsTest, EmptyWordIsAWord) {
  const CliArgs args = Parse({"put", "", ""});
  EXPECT_EQ(args.command, "put");
  EXPECT_EQ(args.positional, (Words{"", ""}));

  const CliArgs empty_command = Parse({"", "x"});
  EXPECT_EQ(empty_command.command, "");
  EXPECT_EQ(empty_command.positional, (Words{"x"}));
}

TEST(CliArgsTest, Errors) {
  CliArgs args;
  EXPECT_TRUE(ParseCliArgs({}, &args).IsInvalidArgument());
  EXPECT_TRUE(ParseCliArgs({"--=x", "get"}, &args).IsInvalidArgument());
}

TEST(CliArgsTest, NoCommandIsAnError) {
  CliArgs args;
  EXPECT_TRUE(ParseCliArgs({"--db=x"}, &args).IsInvalidArgument());
  EXPECT_TRUE(ParseCliArgs({"--"}, &args).IsInvalidArgument());
  EXPECT_TRUE(ParseCliArgs({"--bogus-command"}, &args).IsInvalidArgument());
}

TEST(CliArgsTest, EmptyFlagNameIsAnErrorWhereverItIs) {
  CliArgs args;
  EXPECT_TRUE(ParseCliArgs({"get", "--=x"}, &args).IsInvalidArgument());
  EXPECT_TRUE(ParseCliArgs({"get", "k", "--="}, &args).IsInvalidArgument());
  // After "--" the word is no flag, so it is only a word.
  EXPECT_OK(ParseCliArgs({"get", "--", "--=x"}, &args));
  EXPECT_EQ(args.positional, (Words{"--=x"}));
}

TEST(CliArgsTest, ErrorNamesTheWordAndLeavesTheOutputAlone) {
  CliArgs args;
  args.command = "kept";
  args.positional = {"kept"};
  args.flags["kept"] = "kept";
  const Status s = ParseCliArgs({"get", "k", "--=x"}, &args);
  ASSERT_TRUE(s.IsInvalidArgument());
  EXPECT_NE(s.ToString().find("--=x"), std::string::npos) << s.ToString();
  EXPECT_EQ(args.command, "kept");
  EXPECT_EQ(args.positional, (Words{"kept"}));
  EXPECT_EQ(args.flags, (std::map<std::string, std::string>{{"kept", "kept"}}));
}

TEST(CliArgsTest, ErrorShowsTheWordEscaped) {
  // The word came from the command line and goes to a terminal, so a byte that is not printable
  // is not passed on as it is.
  CliArgs args;
  const Status s = ParseCliArgs({"get", "--=\x1b[31m\tx"}, &args);
  ASSERT_TRUE(s.IsInvalidArgument());
  EXPECT_EQ(s.ToString().find('\x1b'), std::string::npos) << s.ToString();
  EXPECT_EQ(s.ToString().find('\t'), std::string::npos) << s.ToString();
  EXPECT_NE(s.ToString().find("--=\\x1b[31m\\x09x"), std::string::npos) << s.ToString();
}

TEST(CliArgsTest, ParsingReplacesWhatTheOutputHeld) {
  CliArgs args;
  args.command = "old";
  args.positional = {"old"};
  args.flags["old"] = "old";
  ASSERT_OK(ParseCliArgs({"get", "k"}, &args));
  EXPECT_EQ(args.command, "get");
  EXPECT_EQ(args.positional, (Words{"k"}));
  EXPECT_TRUE(args.flags.empty());
}

TEST(CliArgsTest, EscapeBytes) {
  EXPECT_EQ(EscapeBytes("a\\b"), "a\\\\b");
  EXPECT_EQ(EscapeBytes(std::string("\0\xff", 2)), "\\x00\\xff");
}

TEST(CliArgsTest, EscapeBytesOfNothingIsNothing) { EXPECT_EQ(EscapeBytes(""), ""); }

TEST(CliArgsTest, EscapeBytesTreatsEveryByteValueAsItsOwnRule) {
  for (int byte = 0; byte < 256; byte++) {
    const std::string input(1, static_cast<char>(byte));
    std::string expected;
    if (byte == '\\') {
      expected = "\\\\";
    } else if (byte >= 0x20 && byte <= 0x7e) {
      expected = input;
    } else {
      char hex[8];
      std::snprintf(hex, sizeof hex, "\\x%02x", static_cast<unsigned>(byte));
      expected = hex;
    }
    EXPECT_EQ(EscapeBytes(input), expected) << "byte " << byte;
  }
}

TEST(CliArgsTest, EscapeBytesNeverLeavesALineBreakOrATab) {
  const std::string shown = EscapeBytes("a\tb\nc\rd e");
  EXPECT_EQ(shown, "a\\x09b\\x0ac\\x0dd e");
}

}  // namespace
}  // namespace shale
