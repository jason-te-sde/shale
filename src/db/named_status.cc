#include "db/named_status.h"

#include <string>

#include "shale/status.h"

namespace shale {

Status WithFileName(const Status& s, const std::string& file) {
  if (s.ok() || s.IsIOError()) {
    return s;
  }
  // ToString() is "<code name>: <message>", and the factory below puts the code name back, so what
  // is kept is the message, with the detail it had.
  const std::string text = s.ToString();
  const size_t separator = text.find(": ");
  const std::string message = separator == std::string::npos ? text : text.substr(separator + 2);
  switch (s.code()) {
    case Status::Code::kNotFound:
      return Status::NotFound(message, file);
    case Status::Code::kCorruption:
      return Status::Corruption(message, file);
    case Status::Code::kInvalidArgument:
      return Status::InvalidArgument(message, file);
    case Status::Code::kNotSupported:
      return Status::NotSupported(message, file);
    case Status::Code::kOk:
    case Status::Code::kIOError:
      break;  // returned above
  }
  return s;
}

}  // namespace shale
