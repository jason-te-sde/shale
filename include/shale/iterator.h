#ifndef SHALE_ITERATOR_H_
#define SHALE_ITERATOR_H_

#include <string_view>

#include "shale/status.h"

namespace shale {

/// A cursor over a sorted sequence of key-value entries.
///
/// The database hands one out to read a range of keys, and the engine uses the same interface for
/// its internal sequences, the memtable and the tables among them, whose keys carry a sequence
/// number and a type after the user key. What the keys mean and in what order they come is up to
/// whoever creates the iterator; this interface only walks them forward.
///
/// An iterator is not thread-safe: one thread uses it at a time. A new iterator is not positioned
/// anywhere, so the first call is SeekToFirst() or Seek().
class Iterator {
 public:
  /// Does nothing; an Iterator is destroyed through this base class.
  virtual ~Iterator();

  /// Returns true if the iterator is positioned at an entry. It is false before the first seek,
  /// after Next() has moved past the last entry, after a seek that finds nothing, and when an
  /// error has stopped the iteration, which status() reports.
  virtual bool Valid() const = 0;

  /// Positions the iterator at the first entry, or makes it invalid if there are none.
  virtual void SeekToFirst() = 0;

  /// Positions the iterator at the first entry whose key is not less than `target`, in the
  /// iterator's own order, or makes it invalid if there is none.
  /// @param target a key of the kind the iterator returns from key(); for an iterator over
  ///   internal keys that is an internal key, not a user key.
  virtual void Seek(std::string_view target) = 0;

  /// Moves to the next entry, or becomes invalid after the last. Requires Valid().
  virtual void Next() = 0;

  /// Returns the key of the current entry. Requires Valid(). The view stays valid until the
  /// iterator is moved with SeekToFirst(), Seek() or Next(), or destroyed.
  virtual std::string_view key() const = 0;

  /// Returns the value of the current entry. Requires Valid(). The view stays valid as long as the
  /// key's does.
  virtual std::string_view value() const = 0;

  /// Returns OK, or the error that made the iterator stop. An iterator that is invalid because it
  /// ran out of entries has an OK status; one that is invalid because reading failed does not,
  /// and a caller that only checks Valid() would take the failure for the end of the data.
  virtual Status status() const = 0;
};

}  // namespace shale

#endif  // SHALE_ITERATOR_H_
