#ifndef SHALE_IO_TWO_LEVEL_ITERATOR_H_
#define SHALE_IO_TWO_LEVEL_ITERATOR_H_

#include <functional>
#include <memory>
#include <string_view>

#include "shale/iterator.h"

namespace shale {

/// Makes the iterator over the block an entry of the index names. It is given the index entry's
/// value, whatever the index stores there. It must return an iterator, and one that cannot be
/// made is NewErrorIterator() with the reason, never null.
using BlockFunction = std::function<std::unique_ptr<Iterator>(std::string_view index_value)>;

/// Returns an iterator over the entries of a sequence of blocks, one after another, in the order
/// `index_iter` lists them. The index's keys must be in the order the blocks' keys are, and its
/// key for a block must not be less than the last key in it, which is what lets a Seek() go to
/// the one block that can hold the target; whatever the index says is checked against what the
/// block holds and not believed, so an index that is wrong costs a block read and not a result.
///
/// A block is opened when the iteration reaches it, and only one is open at a time. A block with
/// no entries is passed over. A block that cannot be read, and an index that cannot be, end the
/// iteration, and status() returns the first such error. An error is not forgotten: once there
/// is one, the iterator stays invalid whatever is done to it, as a block iterator does, so that a
/// caller that seeks past the damage is not told by an OK status that the data was all there.
///
/// The iterator owns both. `block_function` is called from whichever thread uses the iterator.
/// @param index_iter an iterator over the index.
/// @param block_function opens the block an index entry's value names.
std::unique_ptr<Iterator> NewTwoLevelIterator(std::unique_ptr<Iterator> index_iter,
                                              BlockFunction block_function);

}  // namespace shale

#endif  // SHALE_IO_TWO_LEVEL_ITERATOR_H_
