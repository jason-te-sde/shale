#include "shale/iterator.h"

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include "io/iterator_util.h"
#include "shale/status.h"
#include "util/check.h"

namespace shale {

// Defined here, and not inline in the header, so that the class's virtual table has one home.
Iterator::~Iterator() = default;

namespace {

// An iterator with nothing in it, or one that says why it has nothing.
class EmptyIterator final : public Iterator {
 public:
  explicit EmptyIterator(Status status) : status_(std::move(status)) {}

  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view /*target*/) override {}

  void Next() override { SHALE_CHECK(false, "Iterator::Next requires Valid"); }

  std::string_view key() const override {
    SHALE_CHECK(false, "Iterator::key requires Valid");
    return {};
  }

  std::string_view value() const override {
    SHALE_CHECK(false, "Iterator::value requires Valid");
    return {};
  }

  Status status() const override { return status_; }

 private:
  const Status status_;
};

// Passes everything to the iterator it wraps, and on destruction releases what was tied to it.
class WrappingIterator final : public Iterator {
 public:
  WrappingIterator(std::unique_ptr<Iterator> it, std::shared_ptr<const void> pin,
                   std::function<void()> on_destroy)
      : pin_(std::move(pin)), on_destroy_(std::move(on_destroy)), it_(std::move(it)) {
    SHALE_CHECK(it_ != nullptr, "an iterator wrapper needs an iterator to wrap");
  }

  // The checker reads std::function's call operator as able to throw bad_function_call. It cannot
  // here, because the call is guarded by the emptiness test; and a cleanup that throws is a bug
  // that should end the process, which is what an exception leaving a destructor does.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~WrappingIterator() override {
    // The wrapped iterator first, because it may point into what the cleanup and the pin release.
    it_.reset();
    if (on_destroy_) {
      on_destroy_();
    }
  }

  bool Valid() const override { return it_->Valid(); }
  void SeekToFirst() override { it_->SeekToFirst(); }
  void Seek(std::string_view target) override { it_->Seek(target); }
  void Next() override { it_->Next(); }
  std::string_view key() const override { return it_->key(); }
  std::string_view value() const override { return it_->value(); }
  Status status() const override { return it_->status(); }

 private:
  const std::shared_ptr<const void> pin_;
  const std::function<void()> on_destroy_;
  std::unique_ptr<Iterator> it_;
};

}  // namespace

std::unique_ptr<Iterator> NewEmptyIterator() { return std::make_unique<EmptyIterator>(Status()); }

std::unique_ptr<Iterator> NewErrorIterator(Status status) {
  return std::make_unique<EmptyIterator>(std::move(status));
}

std::unique_ptr<Iterator> NewPinningIterator(std::unique_ptr<Iterator> it,
                                             std::shared_ptr<const void> pin) {
  return std::make_unique<WrappingIterator>(std::move(it), std::move(pin), nullptr);
}

std::unique_ptr<Iterator> NewCleanupIterator(std::unique_ptr<Iterator> it,
                                             std::function<void()> on_destroy) {
  return std::make_unique<WrappingIterator>(std::move(it), nullptr, std::move(on_destroy));
}

}  // namespace shale
