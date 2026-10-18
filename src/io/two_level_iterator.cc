#include "io/two_level_iterator.h"

#include <memory>
#include <string_view>
#include <utility>

#include "shale/iterator.h"
#include "shale/status.h"
#include "util/check.h"

namespace shale {

namespace {

class TwoLevelIterator final : public Iterator {
 public:
  TwoLevelIterator(std::unique_ptr<Iterator> index_iter, BlockFunction block_function)
      : index_iter_(std::move(index_iter)), block_function_(std::move(block_function)) {
    SHALE_CHECK(index_iter_ != nullptr, "NewTwoLevelIterator needs an index iterator");
    SHALE_CHECK(block_function_ != nullptr, "NewTwoLevelIterator needs a block function");
  }

  bool Valid() const override { return data_iter_ != nullptr && data_iter_->Valid(); }

  void SeekToFirst() override {
    if (!status_.ok()) {
      return;
    }
    index_iter_->SeekToFirst();
    OpenBlock();
    if (data_iter_ != nullptr) {
      data_iter_->SeekToFirst();
    }
    SkipPastBlocksWithNoEntry();
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) {
      return;
    }
    index_iter_->Seek(target);
    OpenBlock();
    if (data_iter_ != nullptr) {
      data_iter_->Seek(target);
    }
    SkipPastBlocksWithNoEntry();
  }

  void Next() override {
    SHALE_CHECK(Valid(), "TwoLevelIterator::Next requires Valid");
    data_iter_->Next();
    SkipPastBlocksWithNoEntry();
  }

  std::string_view key() const override {
    SHALE_CHECK(Valid(), "TwoLevelIterator::key requires Valid");
    return data_iter_->key();
  }

  std::string_view value() const override {
    SHALE_CHECK(Valid(), "TwoLevelIterator::value requires Valid");
    return data_iter_->value();
  }

  Status status() const override {
    // An index that failed before it was ever asked for anything still has its error to report.
    return status_.ok() ? index_iter_->status() : status_;
  }

 private:
  // Remembers `s` if it is the first error, and returns whether it is one.
  bool Fail(const Status& s) {
    if (s.ok()) {
      return false;
    }
    if (status_.ok()) {
      status_ = s;
    }
    return true;
  }

  // Opens the block the index iterator is at, for the caller to position, or leaves no block open
  // if the index has no entry there. An index that ran out is the end of the data, and an index
  // that stopped on an error is the end of the iteration, which is what its status says.
  void OpenBlock() {
    data_iter_.reset();
    if (!index_iter_->Valid()) {
      Fail(index_iter_->status());
      return;
    }
    data_iter_ = block_function_(index_iter_->value());
    SHALE_CHECK(data_iter_ != nullptr, "the block function of a TwoLevelIterator returned null");
  }

  // Called with the open block just positioned, or moved on. While there is no entry to show,
  // moves to the first entry of the next block. Stops with no block open at the end of the index,
  // and at a block that failed, which is not passed over as an empty one is.
  void SkipPastBlocksWithNoEntry() {
    while (data_iter_ != nullptr && !data_iter_->Valid()) {
      if (Fail(data_iter_->status())) {
        data_iter_.reset();
        return;
      }
      index_iter_->Next();
      OpenBlock();
      if (data_iter_ != nullptr) {
        data_iter_->SeekToFirst();
      }
    }
  }

  const std::unique_ptr<Iterator> index_iter_;
  const BlockFunction block_function_;
  std::unique_ptr<Iterator> data_iter_;  // the open block, or null
  Status status_;                        // the first error, which is never cleared
};

}  // namespace

std::unique_ptr<Iterator> NewTwoLevelIterator(std::unique_ptr<Iterator> index_iter,
                                              BlockFunction block_function) {
  return std::make_unique<TwoLevelIterator>(std::move(index_iter), std::move(block_function));
}

}  // namespace shale
