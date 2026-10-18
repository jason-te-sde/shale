#include "core/version_edit.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "shale/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/dbformat.h"

namespace shale {

namespace {

constexpr uint32_t kLogNumberTag = 1;
constexpr uint32_t kNextFileNumberTag = 2;
constexpr uint32_t kLastSequenceTag = 3;
constexpr uint32_t kCompactPointerTag = 4;
constexpr uint32_t kDeletedFileTag = 5;
constexpr uint32_t kNewFileTag = 6;

void PutLevel(std::string* dst, int level) {
  SHALE_CHECK(level >= 0 && level < kNumLevels, "VersionEdit level is out of range");
  PutVarint32(dst, static_cast<uint32_t>(level));
}

Status Truncated(std::string_view field) {
  return Status::Corruption("VersionEdit: truncated " + std::string(field));
}

// A varint that does not fit in 64 bits is reported with the truncated ones. The reader cannot
// tell the two apart, and either way the record is unusable.
bool ReadNumber(std::string_view* input, uint64_t* v) { return GetVarint64(input, v); }

// Reads the level that opens a record into `*level`. `field` names the record for the error. The
// level is read as a varint64 so that a value too big for 32 bits is out of range, as it is, and
// not truncated, as the 32-bit reader would call it.
Status ReadLevel(std::string_view* input, std::string_view field, int* level) {
  uint64_t value = 0;
  if (!ReadNumber(input, &value)) {
    return Truncated(field);
  }
  if (value >= static_cast<uint64_t>(kNumLevels)) {
    return Status::Corruption("VersionEdit: level " + std::to_string(value) + " out of range");
  }
  *level = static_cast<int>(value);
  return Status::OK();
}

bool ReadKey(std::string_view* input, std::string* key) {
  std::string_view view;
  if (!GetLengthPrefixed(input, &view)) {
    return false;
  }
  key->assign(view.data(), view.size());
  return true;
}

// Appends `bytes` to `*out` with everything outside printable ASCII, and the quote and backslash
// that would make the result ambiguous, as \xNN.
void AppendEscaped(std::string* out, std::string_view bytes) {
  constexpr std::string_view kHex = "0123456789abcdef";
  for (const char c : bytes) {
    const auto u = static_cast<uint8_t>(c);
    if (u >= 0x20 && u <= 0x7e && c != '\'' && c != '\\') {
      out->push_back(c);
    } else {
      out->append("\\x");
      out->push_back(kHex[u >> 4]);
      out->push_back(kHex[u & 0xf]);
    }
  }
}

// An internal key as 'user key'@sequence:type, or, for bytes that are not one, the bytes alone
// between "bad:'" and a quote: a damaged key is what this is most needed for.
std::string DebugKey(std::string_view internal_key) {
  std::string out;
  ParsedInternalKey parsed;
  if (!ParseInternalKey(internal_key, &parsed)) {
    out.append("bad:'");
    AppendEscaped(&out, internal_key);
    out.push_back('\'');
    return out;
  }
  out.push_back('\'');
  AppendEscaped(&out, parsed.user_key);
  out.append("'@").append(std::to_string(parsed.sequence));
  out.append(parsed.type == ValueType::kValue ? ":value" : ":deletion");
  return out;
}

}  // namespace

void VersionEdit::EncodeTo(std::string* dst) const {
  if (log_number) {
    PutVarint32(dst, kLogNumberTag);
    PutVarint64(dst, *log_number);
  }
  if (next_file_number) {
    PutVarint32(dst, kNextFileNumberTag);
    PutVarint64(dst, *next_file_number);
  }
  if (last_sequence) {
    PutVarint32(dst, kLastSequenceTag);
    PutVarint64(dst, *last_sequence);
  }
  for (const auto& [level, key] : compact_pointers) {
    PutVarint32(dst, kCompactPointerTag);
    PutLevel(dst, level);
    PutLengthPrefixed(dst, key);
  }
  for (const auto& [level, number] : deleted_files) {
    PutVarint32(dst, kDeletedFileTag);
    PutLevel(dst, level);
    PutVarint64(dst, number);
  }
  for (const auto& [level, file] : new_files) {
    PutVarint32(dst, kNewFileTag);
    PutLevel(dst, level);
    PutVarint64(dst, file.number);
    PutVarint64(dst, file.file_size);
    PutLengthPrefixed(dst, file.smallest);
    PutLengthPrefixed(dst, file.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view src) {
  // Into a local, so that an edit that fails halfway does not leave this one half replaced.
  VersionEdit result;
  std::string_view input = src;
  while (!input.empty()) {
    uint64_t tag = 0;
    if (!ReadNumber(&input, &tag)) {
      return Truncated("tag");
    }
    switch (tag) {
      case kLogNumberTag: {
        uint64_t v = 0;
        if (!ReadNumber(&input, &v)) {
          return Truncated("log_number");
        }
        result.log_number = v;
        break;
      }
      case kNextFileNumberTag: {
        uint64_t v = 0;
        if (!ReadNumber(&input, &v)) {
          return Truncated("next_file_number");
        }
        result.next_file_number = v;
        break;
      }
      case kLastSequenceTag: {
        uint64_t v = 0;
        if (!ReadNumber(&input, &v)) {
          return Truncated("last_sequence");
        }
        result.last_sequence = v;
        break;
      }
      case kCompactPointerTag: {
        int level = 0;
        if (Status s = ReadLevel(&input, "compact_pointer", &level); !s.ok()) {
          return s;
        }
        std::string key;
        if (!ReadKey(&input, &key)) {
          return Truncated("compact_pointer");
        }
        result.compact_pointers.emplace_back(level, std::move(key));
        break;
      }
      case kDeletedFileTag: {
        int level = 0;
        if (Status s = ReadLevel(&input, "deleted_file", &level); !s.ok()) {
          return s;
        }
        uint64_t number = 0;
        if (!ReadNumber(&input, &number)) {
          return Truncated("deleted_file");
        }
        result.deleted_files.emplace(level, number);
        break;
      }
      case kNewFileTag: {
        int level = 0;
        if (Status s = ReadLevel(&input, "new_file", &level); !s.ok()) {
          return s;
        }
        FileMetaData file;
        if (!ReadNumber(&input, &file.number) || !ReadNumber(&input, &file.file_size) ||
            !ReadKey(&input, &file.smallest) || !ReadKey(&input, &file.largest)) {
          return Truncated("new_file");
        }
        result.new_files.emplace_back(level, std::move(file));
        break;
      }
      default:
        return Status::Corruption("VersionEdit: unknown tag " + std::to_string(tag));
    }
  }
  *this = std::move(result);
  return Status::OK();
}

std::string VersionEdit::DebugString() const {
  std::string out = "VersionEdit {";
  if (log_number) {
    out.append("\n  log_number: ").append(std::to_string(*log_number));
  }
  if (next_file_number) {
    out.append("\n  next_file_number: ").append(std::to_string(*next_file_number));
  }
  if (last_sequence) {
    out.append("\n  last_sequence: ").append(std::to_string(*last_sequence));
  }
  for (const auto& [level, key] : compact_pointers) {
    out.append("\n  compact_pointer L").append(std::to_string(level));
    out.append(" ").append(DebugKey(key));
  }
  for (const auto& [level, number] : deleted_files) {
    out.append("\n  delete L").append(std::to_string(level));
    out.append(" #").append(std::to_string(number));
  }
  for (const auto& [level, file] : new_files) {
    out.append("\n  add L").append(std::to_string(level));
    out.append(" #").append(std::to_string(file.number));
    out.append(" size=").append(std::to_string(file.file_size));
    out.append(" smallest=").append(DebugKey(file.smallest));
    out.append(" largest=").append(DebugKey(file.largest));
  }
  out.append("\n}");
  return out;
}

}  // namespace shale
