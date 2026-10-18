#ifndef SHALE_STATUS_H_
#define SHALE_STATUS_H_

#include <string>
#include <string_view>

namespace shale {

/// The result of an operation that can fail.
///
/// shale reports every error by returning a Status; nothing under `src/` throws. A
/// default-constructed Status is OK. The class is `[[nodiscard]]`, so a caller that drops a
/// returned Status gets a compile error rather than a silently ignored failure.
class [[nodiscard]] Status {
 public:
  /// The category of a Status.
  enum class Code : unsigned char {
    kOk = 0,           ///< The operation succeeded.
    kNotFound,         ///< The requested key, file or object does not exist.
    kCorruption,       ///< Stored data failed a checksum or format check.
    kInvalidArgument,  ///< The caller passed an argument that is not allowed.
    kIOError,          ///< The operating system reported a failure.
    kNotSupported,     ///< The operation is not implemented.
  };

  /// Constructs an OK status.
  Status() noexcept = default;

  /// Returns an OK status.
  static Status OK() noexcept { return Status(); }

  /// Returns a NotFound status.
  /// @param msg what was not found.
  /// @param detail optional context, appended to the message after `": "`.
  static Status NotFound(std::string_view msg, std::string_view detail = {});

  /// Returns a Corruption status.
  /// @param msg what is wrong with the data.
  /// @param detail optional context, such as the file name, appended after `": "`.
  static Status Corruption(std::string_view msg, std::string_view detail = {});

  /// Returns an InvalidArgument status.
  /// @param msg which argument is invalid and why.
  /// @param detail optional context, appended to the message after `": "`.
  static Status InvalidArgument(std::string_view msg, std::string_view detail = {});

  /// Returns an IOError status.
  /// @param msg the operation that failed.
  /// @param detail optional context, such as the file name or the errno text.
  static Status IOError(std::string_view msg, std::string_view detail = {});

  /// Returns a NotSupported status.
  /// @param msg what is not supported.
  /// @param detail optional context, appended to the message after `": "`.
  static Status NotSupported(std::string_view msg, std::string_view detail = {});

  /// Returns true if the operation succeeded.
  bool ok() const noexcept { return code_ == Code::kOk; }

  /// Returns true if code() is Code::kNotFound.
  bool IsNotFound() const noexcept { return code_ == Code::kNotFound; }

  /// Returns true if code() is Code::kCorruption.
  bool IsCorruption() const noexcept { return code_ == Code::kCorruption; }

  /// Returns true if code() is Code::kInvalidArgument.
  bool IsInvalidArgument() const noexcept { return code_ == Code::kInvalidArgument; }

  /// Returns true if code() is Code::kIOError.
  bool IsIOError() const noexcept { return code_ == Code::kIOError; }

  /// Returns true if code() is Code::kNotSupported.
  bool IsNotSupported() const noexcept { return code_ == Code::kNotSupported; }

  /// Returns the category of this status.
  Code code() const noexcept { return code_; }

  /// Returns a human-readable description: `"OK"`, or the code name followed by the message and,
  /// when one was given, the detail, for example `"Corruption: bad block: 000007.sst"`.
  std::string ToString() const;

 private:
  Status(Code code, std::string_view msg, std::string_view detail);

  Code code_ = Code::kOk;
  std::string message_;
};

}  // namespace shale

#endif  // SHALE_STATUS_H_
