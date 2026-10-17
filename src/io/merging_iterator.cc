#include "io/merging_iterator.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "io/iterator_util.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/dbformat.h"

namespace shale {

namespace {

class MergingIterator final : public Iterator {
 public:
  MergingIterator(std::vector<std::unique_ptr<Iterator>> children, KeyCompareFn cmp)
      : children_(std::move(children)), cmp_(cmp) {}

  bool Valid() const override { return current_ != nullptr; }

  void SeekToFirst() override {
    if (!status_.ok()) {
      return;
    }
    for (const std::unique_ptr<Iterator>& child : children_) {
      child->SeekToFirst();
    }
    FindSmallest(/*first_look=*/true);
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) {
      return;
    }
    for (const std::unique_ptr<Iterator>& child : children_) {
      child->Seek(target);
    }
    FindSmallest(/*first_look=*/true);
  }

  void Next() override {
    SHALE_CHECK(Valid(), "MergingIterator::Next requires Valid");
    // Only the child that was just read from has moved, so it is the only one that can have run
    // out of entries or failed.
    Iterator* const moved = current_;
    moved->Next();
    if (!moved->Valid() && !moved->status().ok()) {
      Stop(moved->status());
      return;
    }
    FindSmallest(/*first_look=*/false);
  }

  std::string_view key() const override {
    SHALE_CHECK(Valid(), "MergingIterator::key requires Valid");
    return current_->key();
  }

  std::string_view value() const override {
    SHALE_CHECK(Valid(), "MergingIterator::value requires Valid");
    return current_->value();
  }

  Status status() const override {
    if (!status_.ok()) {
      return status_;
    }
    for (const std::unique_ptr<Iterator>& child : children_) {
      Status s = child->status();
      if (!s.ok()) {
        return s;
      }
    }
    return Status::OK();
  }

 private:
  // Ends the iteration, with `error` as the reason.
  void Stop(Status error) {
    status_ = std::move(error);
    current_ = nullptr;
  }

  // Makes the child with the smallest key the current one. With `first_look`, the children have
  // all just been positioned, and one that is invalid is either empty or failed, which its status
  // says; after that only the child that moved can have changed, and Next() has looked at it.
  void FindSmallest(bool first_look) {
    Iterator* smallest = nullptr;
    for (const std::unique_ptr<Iterator>& child : children_) {
      if (!child->Valid()) {
        if (first_look && !child->status().ok()) {
          Stop(child->status());
          return;
        }
        continue;
      }
      // Strictly less, so that of equal keys the child met first, the one with the lowest index,
      // stays the smallest.
      if (smallest == nullptr || cmp_(child->key(), smallest->key()) < 0) {
        smallest = child.get();
      }
    }
    current_ = smallest;
  }

  const std::vector<std::unique_ptr<Iterator>> children_;
  const KeyCompareFn cmp_;
  Iterator* current_ = nullptr;  // the child with the smallest key, or null
  Status status_;                // the error that stopped the iteration, which is never cleared
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children,
                                             KeyCompareFn cmp) {
  SHALE_CHECK(cmp != nullptr, "NewMergingIterator needs a comparison function");
  for (const std::unique_ptr<Iterator>& child : children) {
    SHALE_CHECK(child != nullptr, "NewMergingIterator needs every child to be an iterator");
  }
  if (children.empty()) {
    return NewEmptyIterator();
  }
  if (children.size() == 1) {
    return std::move(children[0]);
  }
  return std::make_unique<MergingIterator>(std::move(children), cmp);
}

}  // namespace shale
