#include "shale/status.h"

#include <string>
#include <string_view>

namespace shale {

namespace {

// A value outside the enumeration cannot be produced through the public interface, but the switch
// is not left to fall off its end if one ever is.
const char* CodeName(Status::Code code) {
  switch (code) {
    case Status::Code::kOk:
      return "OK";
    case Status::Code::kNotFound:
      return "NotFound";
    case Status::Code::kCorruption:
      return "Corruption";
    case Status::Code::kInvalidArgument:
      return "Invalid argument";
    case Status::Code::kIOError:
      return "IO error";
    case Status::Code::kNotSupported:
      return "Not supported";
  }
  return "Unknown";
}

}  // namespace

Status::Status(Code code, std::string_view msg, std::string_view detail)
    : code_(code), message_(msg) {
  if (!detail.empty()) {
    message_.append(": ").append(detail);
  }
}

Status Status::OK() noexcept { return Status(); }

Status Status::NotFound(std::string_view msg, std::string_view detail) {
  return Status(Code::kNotFound, msg, detail);
}

Status Status::Corruption(std::string_view msg, std::string_view detail) {
  return Status(Code::kCorruption, msg, detail);
}

Status Status::InvalidArgument(std::string_view msg, std::string_view detail) {
  return Status(Code::kInvalidArgument, msg, detail);
}

Status Status::IOError(std::string_view msg, std::string_view detail) {
  return Status(Code::kIOError, msg, detail);
}

Status Status::NotSupported(std::string_view msg, std::string_view detail) {
  return Status(Code::kNotSupported, msg, detail);
}

bool Status::ok() const noexcept { return code_ == Code::kOk; }

bool Status::IsNotFound() const noexcept { return code_ == Code::kNotFound; }

bool Status::IsCorruption() const noexcept { return code_ == Code::kCorruption; }

bool Status::IsInvalidArgument() const noexcept { return code_ == Code::kInvalidArgument; }

bool Status::IsIOError() const noexcept { return code_ == Code::kIOError; }

bool Status::IsNotSupported() const noexcept { return code_ == Code::kNotSupported; }

Status::Code Status::code() const noexcept { return code_; }

std::string Status::ToString() const {
  if (ok()) {
    return "OK";
  }
  std::string result(CodeName(code_));
  result.append(": ").append(message_);
  return result;
}

}  // namespace shale
