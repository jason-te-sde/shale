#ifndef SHALE_TOOLS_CLI_ARGS_H_
#define SHALE_TOOLS_CLI_ARGS_H_

// The command line of shalectl taken apart, and the one way its output shows bytes. They are in a
// library of their own so that the rules are tested without running the tool.

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "shale/status.h"

namespace shale {

/// A command line, split into the command, the words after it, and the flags.
struct CliArgs {
  /// The first bare word: the command to run.
  std::string command;
  /// The bare words after the command, in order.
  std::vector<std::string> positional;
  /// The flags, by name without the leading dashes: `--name=value` stores "value" under "name",
  /// and a bare `--name` stores "true". A flag that is given twice keeps the later value.
  std::map<std::string, std::string> flags;
};

/// Splits `argv`, the arguments of a command line without the program name, into a CliArgs.
///
/// A word that starts with `--` is a flag, and it is `--name` or `--name=value`, the value being
/// everything after the first `=` and possibly empty. Any other word is a bare word, a word that
/// starts with a single dash among them, so that `-1` can be a value. The first bare word is the
/// command and the rest are positional. Flags may come before the command, after it, and between
/// the words. A word that is exactly `--` ends the flags: every word after it is a bare word, as
/// it stands, which is how a key or a value that starts with `--` is given.
///
/// Returns InvalidArgument, naming the word, for a flag with an empty name such as `--=x`, and
/// InvalidArgument for a command line with no command. `*out` is left as it was on an error.
/// @param argv the arguments, not including the program name.
/// @param out receives the parts.
Status ParseCliArgs(const std::vector<std::string>& argv, CliArgs* out);

/// Returns `bytes` in a form that is one line of printable ASCII and that says exactly which bytes
/// they were: a byte from 0x20 through 0x7e is itself, except the backslash, which is `\\`, and
/// every other byte is `\x` and two lower-case hexadecimal digits. It is the only way shalectl
/// shows a key or a value, so that no byte of a key can end the line or fool a terminal.
/// @param bytes what to show.
std::string EscapeBytes(std::string_view bytes);

}  // namespace shale

#endif  // SHALE_TOOLS_CLI_ARGS_H_
