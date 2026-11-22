#ifndef SHALE_UTIL_KEY_BUFFER_H_
#define SHALE_UTIL_KEY_BUFFER_H_

#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>

#include "util/check.h"

namespace shale {

/// A byte string that is kept in place, inside the object, up to kInline bytes, and on the heap
/// beyond. The keys an iterator rebuilds from their shared prefixes are almost all short, and a
/// std::string that holds one takes an allocation as soon as the key outgrows its small-string
/// buffer, which is a few bytes less than a key and its tag. The buffer keeps what it grew to, so a
/// key that is long once does not allocate again for the next.
///
/// Not thread-safe, and not copyable: it is a working buffer, and a view of it is valid until the
/// next call that changes it.
class KeyBuffer {
 public:
  /// The most bytes kept in place.
  static constexpr size_t kInline = 64;

  KeyBuffer() = default;
  KeyBuffer(const KeyBuffer&) = delete;
  KeyBuffer& operator=(const KeyBuffer&) = delete;
  ~KeyBuffer() = default;

  /// Returns the bytes held.
  std::string_view view() const { return {Data(), size_}; }

  /// Makes the contents the first `n` bytes of `source`, which may be this buffer's own contents.
  /// Aborts if `source` is shorter than `n`.
  void AssignPrefix(std::string_view source, size_t n) {
    SHALE_CHECK(n <= source.size(),
                "KeyBuffer::AssignPrefix: the prefix is longer than the source");
    if (source.data() == Data()) {
      size_ = n;  // the prefix of what is already here
      return;
    }
    size_ = 0;
    const std::string_view prefix = source.substr(0, n);
    Append(prefix.data(), prefix.size());
  }

  /// Adds the `n` bytes at `p` to the end.
  void Append(const char* p, size_t n) {
    if (n == 0) {
      return;
    }
    if (size_ + n > capacity_) {
      Grow(size_ + n);
    }
    std::memcpy(Data() + size_, p, n);
    size_ += n;
  }

  /// Makes the buffer empty. What it grew to is kept.
  void Clear() { size_ = 0; }

 private:
  char* Data() { return heap_ != nullptr ? heap_.get() : inline_; }
  const char* Data() const { return heap_ != nullptr ? heap_.get() : inline_; }

  // Moves to a buffer of at least `needed` bytes, at least twice as many as there are room for,
  // keeping the bytes held.
  void Grow(size_t needed) {
    const size_t grown = needed > 2 * capacity_ ? needed : 2 * capacity_;
    std::unique_ptr<char[]> bigger = std::make_unique_for_overwrite<char[]>(grown);
    if (size_ > 0) {
      std::memcpy(bigger.get(), Data(), size_);
    }
    heap_ = std::move(bigger);
    capacity_ = grown;
  }

  char inline_[kInline];  // not set to anything: only the first size_ bytes are ever read
  std::unique_ptr<char[]> heap_;
  size_t size_ = 0;
  size_t capacity_ = kInline;
};

}  // namespace shale

#endif  // SHALE_UTIL_KEY_BUFFER_H_
