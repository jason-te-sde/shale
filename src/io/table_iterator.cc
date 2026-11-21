#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "io/block.h"
#include "io/table.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/dbformat.h"
#include "util/named_status.h"

namespace shale {

// Iterates a table: the index, and the data block its current entry names, one block open at a
// time. It makes every check the table's structure allows, as it moves, and ends the iteration at
// the first that fails:
//
//   - a data block that cannot be read, or whose entries are damaged, ends it with that error;
//   - a key after the last key the block's index entry names ends it with Corruption, because a
//     lookup for that key would have been sent to the next block;
//   - a key that is not after the one before it ends it with Corruption, because no TableBuilder
//     can be given such a sequence;
//   - a seek that lands before its target ends it with Corruption, because a scan that begins
//     before the key it was asked to begin at returns what it was not asked for.
//
// Like the block iterators it is sticky: once it has failed it stays invalid. The error is named
// with the table's file once, when it is recorded.
class Table::Iter final : public Iterator {
 public:
  Iter(std::shared_ptr<const Table> table, const TableReadOptions& options)
      : table_(std::move(table)),
        options_(options),
        index_(table_->index_block_, CompareInternalKey, kInternalKeyTagSize),
        data_(CompareInternalKey, kInternalKeyTagSize) {}

  bool Valid() const override { return status_.ok() && block_open_ && data_.Valid(); }

  void SeekToFirst() override {
    if (!status_.ok()) {
      return;
    }
    index_.SeekToFirst();
    if (Enter(Entry::kFresh)) {
      data_.SeekToFirst();
    }
    Settle(Mode::kPositioning);
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) {
      return;
    }
    index_.Seek(target);
    if (Enter(Entry::kFresh)) {
      data_.Seek(target);
    }
    Settle(Mode::kPositioning);
    if (Valid() && CompareInternalKey(data_.key(), target) < 0) {
      Fail(Status::Corruption("seek in the table landed before its target"));
    }
  }

  void Next() override {
    SHALE_CHECK(Valid(), "table iterator: Next requires Valid");
    data_.Next();
    Settle(Mode::kStepping);
  }

  std::string_view key() const override {
    SHALE_CHECK(Valid(), "table iterator: key requires Valid");
    return data_.key();
  }

  std::string_view value() const override {
    SHALE_CHECK(Valid(), "table iterator: value requires Valid");
    return data_.value();
  }

  Status status() const override { return status_; }

 private:
  // How the iterator got to where it is: by a seek, or by a step from the entry before.
  enum class Mode : std::uint8_t { kPositioning, kStepping };
  // Whether the block opened is the first after a seek, or the next after the one just left, whose
  // last key is then the key before the block's first.
  enum class Entry : std::uint8_t { kFresh, kContinue };

  // Moves on from whatever `data_` has been asked to do, until it is at an entry that passes the
  // checks, at the end of the table, or failed.
  void Settle(Mode mode) {
    while (block_open_) {
      if (data_.Valid()) {
        // The bound is checked in place, against the index key, with no copy of either.
        if (CompareInternalKey(data_.key(), index_.key()) > 0) {
          Fail(Status::Corruption(
              "data block holds a key after the last key its index entry names"));
        } else if (mode == Mode::kStepping && data_.has_previous_key() &&
                   CompareInternalKey(data_.previous_key(), data_.key()) >= 0) {
          Fail(Status::Corruption("keys in the table are not in increasing order"));
        }
        return;
      }
      if (!data_.status().ok()) {
        Fail(data_.status());  // a block that failed is not skipped
        return;
      }
      index_.Next();
      if (!Enter(mode == Mode::kStepping ? Entry::kContinue : Entry::kFresh)) {
        return;
      }
      data_.SeekToFirst();
    }
  }

  // Opens the block the index is at. Returns false if there is none: the index has run out, which
  // is the end of the table, or is damaged, or the block cannot be read, which ends the iteration.
  bool Enter(Entry how) {
    block_open_ = false;
    if (!index_.Valid()) {
      if (!index_.status().ok()) {
        Fail(index_.status());
      }
      return false;
    }
    std::shared_ptr<const Block> block;
    Status s = table_->ReadDataBlock(options_, index_.value(), &block);
    if (!s.ok()) {
      Fail(std::move(s));
      return false;
    }
    if (how == Entry::kContinue) {
      data_.Continue(std::move(block));
    } else {
      data_.Reset(std::move(block));
    }
    block_open_ = true;
    return true;
  }

  // Records the first error, with the file it is in.
  void Fail(Status s) {
    block_open_ = false;
    if (status_.ok()) {
      status_ = table_->name_.empty() ? std::move(s) : WithFileName(s, table_->name_);
    }
  }

  const std::shared_ptr<const Table> table_;
  const TableReadOptions options_;
  BlockIterator index_;
  BlockIterator data_;
  bool block_open_ = false;  // `data_` is over the block the index is at
  Status status_;
};

std::unique_ptr<Iterator> Table::NewIterator(const TableReadOptions& options) const {
  return std::make_unique<Iter>(shared_from_this(), options);
}

}  // namespace shale
