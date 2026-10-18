#include "cli_args.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shale/status.h"

namespace shale {

Status ParseCliArgs(const std::vector<std::string>& argv, CliArgs* out) {
  CliArgs parsed;
  bool have_command = false;
  bool flags_ended = false;
  for (const std::string& word : argv) {
    if (!flags_ended && word.starts_with("--")) {
      if (word.size() == 2) {
        flags_ended = true;
        continue;
      }
      const size_t equals = word.find('=', 2);
      const size_t name_length = equals == std::string::npos ? word.size() - 2 : equals - 2;
      if (name_length == 0) {
        // The word came from the command line and is going to be shown, so it is shown escaped.
        return Status::InvalidArgument("a flag has no name", EscapeBytes(word));
      }
      parsed.flags[word.substr(2, name_length)] =
          equals == std::string::npos ? "true" : word.substr(equals + 1);
      continue;
    }
    if (!have_command) {
      parsed.command = word;
      have_command = true;
    } else {
      parsed.positional.push_back(word);
    }
  }
  if (!have_command) {
    return Status::InvalidArgument("no command given");
  }
  *out = std::move(parsed);
  return Status::OK();
}

std::string EscapeBytes(std::string_view bytes) {
  constexpr std::string_view kHexDigits = "0123456789abcdef";
  std::string shown;
  shown.reserve(bytes.size());
  for (const char c : bytes) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '\\') {
      shown.append("\\\\");
    } else if (byte >= 0x20 && byte <= 0x7e) {
      shown.push_back(c);
    } else {
      shown.append("\\x");
      shown.push_back(kHexDigits[byte >> 4]);
      shown.push_back(kHexDigits[byte & 0x0f]);
    }
  }
  return shown;
}

}  // namespace shale
