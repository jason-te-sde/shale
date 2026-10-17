#include "util/filename.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace shale {

namespace {

// Returns "<dbname>/<name>".
std::string Join(const std::string& dbname, std::string_view name) {
  std::string path;
  path.reserve(dbname.size() + 1 + name.size());
  path.append(dbname).append("/").append(name);
  return path;
}

// Returns `number` in decimal, padded with zeros on the left to six digits.
std::string PaddedNumber(uint64_t number) {
  const std::string digits = std::to_string(number);
  constexpr size_t kMinWidth = 6;
  std::string padded;
  if (digits.size() < kMinWidth) {
    padded.assign(kMinWidth - digits.size(), '0');
  }
  return padded.append(digits);
}

std::string NumberedName(uint64_t number, std::string_view suffix) {
  return PaddedNumber(number).append(suffix);
}

// Reads the decimal digits at the front of `*input` into `*value` and removes them. Returns false
// if there is no digit at the front or the number does not fit in 64 bits.
bool ConsumeDecimalNumber(std::string_view* input, uint64_t* value) {
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  uint64_t result = 0;
  size_t digits = 0;
  while (digits < input->size() && (*input)[digits] >= '0' && (*input)[digits] <= '9') {
    const uint64_t digit = static_cast<uint64_t>((*input)[digits] - '0');
    if (result > kMax / 10 || (result == kMax / 10 && digit > kMax % 10)) {
      return false;
    }
    result = result * 10 + digit;
    digits++;
  }
  if (digits == 0) {
    return false;
  }
  input->remove_prefix(digits);
  *value = result;
  return true;
}

}  // namespace

std::string LogFileName(const std::string& dbname, uint64_t number) {
  return Join(dbname, NumberedName(number, ".log"));
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return Join(dbname, NumberedName(number, ".sst"));
}

std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
  return Join(dbname, "MANIFEST-" + PaddedNumber(number));
}

std::string CurrentFileName(const std::string& dbname) { return Join(dbname, "CURRENT"); }

std::string LockFileName(const std::string& dbname) { return Join(dbname, "LOCK"); }

std::string InfoLogFileName(const std::string& dbname) { return Join(dbname, "LOG"); }

std::string OldInfoLogFileName(const std::string& dbname) { return Join(dbname, "LOG.old"); }

std::string TempFileName(const std::string& dbname, uint64_t number) {
  return Join(dbname, NumberedName(number, ".dbtmp"));
}

bool ParseFileName(std::string_view filename, uint64_t* number, FileType* type) {
  if (filename == "CURRENT") {
    *number = 0;
    *type = FileType::kCurrentFile;
    return true;
  }
  if (filename == "LOCK") {
    *number = 0;
    *type = FileType::kLockFile;
    return true;
  }
  if (filename == "LOG" || filename == "LOG.old") {
    *number = 0;
    *type = FileType::kInfoLogFile;
    return true;
  }

  constexpr std::string_view kManifestPrefix = "MANIFEST-";
  if (filename.starts_with(kManifestPrefix)) {
    std::string_view rest = filename.substr(kManifestPrefix.size());
    uint64_t parsed = 0;
    if (!ConsumeDecimalNumber(&rest, &parsed) || !rest.empty()) {
      return false;
    }
    *number = parsed;
    *type = FileType::kDescriptorFile;
    return true;
  }

  std::string_view rest = filename;
  uint64_t parsed = 0;
  if (!ConsumeDecimalNumber(&rest, &parsed)) {
    return false;
  }
  FileType parsed_type;
  if (rest == ".log") {
    parsed_type = FileType::kLogFile;
  } else if (rest == ".sst") {
    parsed_type = FileType::kTableFile;
  } else if (rest == ".dbtmp") {
    parsed_type = FileType::kTempFile;
  } else {
    return false;
  }
  *number = parsed;
  *type = parsed_type;
  return true;
}

}  // namespace shale
