#ifndef SHALE_IO_ITERATOR_UTIL_H_
#define SHALE_IO_ITERATOR_UTIL_H_

// Small iterators the engine builds others from: ones that have nothing to show, ones that carry
// an error, and wrappers that tie an iterator's lifetime to something else's. They are defined in
// iterator.cc.

#include <functional>
#include <memory>

#include "shale/iterator.h"
#include "shale/status.h"

namespace shale {

/// Returns an iterator over no entries. It is never valid, and its status is OK.
std::unique_ptr<Iterator> NewEmptyIterator();

/// Returns an iterator that is never valid and whose status is `status`. It is how a function that
/// must return an iterator reports that it could not make a real one, so that the failure reaches
/// the caller through the iterator's status. `status` should not be OK; with an OK status the
/// result is the same as NewEmptyIterator().
/// @param status the error to report.
std::unique_ptr<Iterator> NewErrorIterator(Status status);

/// Wraps `it`, keeping `pin` alive until the wrapper is destroyed. The wrapper behaves exactly as
/// `it` does. It is for an iterator that points into something it does not own, a cached block
/// for one: the pin keeps that from being freed while the iterator is in use. The wrapped iterator
/// is destroyed before the pin is released, so it may use the pinned object as it is destroyed.
/// @param it the iterator to wrap; must not be null.
/// @param pin whatever has to outlive `it`; shared with its other owners, and may be null.
std::unique_ptr<Iterator> NewPinningIterator(std::unique_ptr<Iterator> it,
                                             std::shared_ptr<const void> pin);

/// Wraps `it`, calling `on_destroy` from the wrapper's destructor. The wrapper behaves exactly as
/// `it` does. The wrapped iterator is destroyed first, so `on_destroy` may release what it points
/// into. It must not throw, and must not use the wrapper.
/// @param it the iterator to wrap; must not be null.
/// @param on_destroy what to run once; may be empty, which runs nothing.
std::unique_ptr<Iterator> NewCleanupIterator(std::unique_ptr<Iterator> it,
                                             std::function<void()> on_destroy);

}  // namespace shale

#endif  // SHALE_IO_ITERATOR_UTIL_H_
