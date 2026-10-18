#include "db/db_iter.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "shale/iterator.h"
#include "shale/status.h"
#include "util/check.h"
#include "util/dbformat.h"

namespace shale {

namespace {

class DBIter final : public Iterator {
 public:
  DBIter(std::unique_ptr<Iterator> internal_iter, SequenceNumber sequence)
      : iter_(std::move(internal_iter)), sequence_(sequence) {
    SHALE_CHECK(iter_ != nullptr, "NewDBIterator needs an internal iterator");
    SHALE_CHECK(sequence_ <= kMaxSequenceNumber,
                "NewDBIterator: the sequence number is above any a write can have");
  }

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    if (!status_.ok()) {
      return;
    }
    iter_->SeekToFirst();
    FindNextUserEntry(/*skipping=*/false);
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) {
      return;
    }
    // The newest entry of `target` that is at or below the sequence sorts first among those that
    // `target` may show, and every entry of a smaller user key sorts before it.
    const LookupKey lookup(target, sequence_);
    iter_->Seek(lookup.internal_key());
    FindNextUserEntry(/*skipping=*/false);
  }

  void Next() override {
    SHALE_CHECK(valid_, "DBIter::Next requires Valid");
    // The internal iterator is at the entry that is shown, the newest of its key that may be seen,
    // and what follows it for that key is older and hidden by it. The key is copied: the view
    // that key() returned is into the internal iterator and ends with the move.
    saved_key_.assign(ExtractUserKey(iter_->key()));
    iter_->Next();
    FindNextUserEntry(/*skipping=*/true);
  }

  std::string_view key() const override {
    SHALE_CHECK(valid_, "DBIter::key requires Valid");
    return ExtractUserKey(iter_->key());
  }

  std::string_view value() const override {
    SHALE_CHECK(valid_, "DBIter::value requires Valid");
    return iter_->value();
  }

  Status status() const override {
    // The internal iterator's, when the user view ran out because it did.
    return status_.ok() ? iter_->status() : status_;
  }

 private:
  // Moves the internal iterator forward to the entry that the user view shows next, from where it
  // is, and makes the iterator valid there, or invalid if there is none. With `skipping`, every
  // entry of the user key in `saved_key_` is passed over: they are what a newer entry hides.
  //
  // A value that may be seen is shown unless it is hidden. A deletion that may be seen is a hidden
  // key from there on: its own entry is passed over, and so are the older entries of its key that
  // follow. An entry above the sequence is passed over whatever it is, since the iterator does not
  // see it, and the older entries of its key that come after are the ones that count.
  void FindNextUserEntry(bool skipping) {
    valid_ = false;
    while (iter_->Valid()) {
      ParsedInternalKey parsed;
      if (!ParseInternalKey(iter_->key(), &parsed)) {
        // The key could be for any user key and sequence, so nothing after it can be told to be
        // the next entry of the view: the iteration ends rather than step over it.
        status_ = Status::Corruption("corrupted internal key in DB iterator");
        return;
      }
      if (parsed.sequence <= sequence_) {
        switch (parsed.type) {
          case ValueType::kDeletion:
            saved_key_.assign(parsed.user_key);
            skipping = true;
            break;
          case ValueType::kValue:
            if (!skipping || parsed.user_key != saved_key_) {
              valid_ = true;
              return;
            }
            break;
        }
      }
      iter_->Next();
    }
  }

  const std::unique_ptr<Iterator> iter_;
  const SequenceNumber sequence_;
  std::string saved_key_;  // the user key whose entries are being passed over
  Status status_;          // the corruption that ended the iteration, or OK
  bool valid_ = false;
};

}  // namespace

std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> internal_iter,
                                        SequenceNumber sequence) {
  return std::make_unique<DBIter>(std::move(internal_iter), sequence);
}

}  // namespace shale
