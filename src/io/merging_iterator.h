#ifndef SHALE_IO_MERGING_ITERATOR_H_
#define SHALE_IO_MERGING_ITERATOR_H_

#include <memory>
#include <vector>

#include "shale/iterator.h"
#include "util/dbformat.h"

namespace shale {

/// Returns an iterator that reads several sorted sequences as one, in the order `cmp` gives. It is
/// what a read of the database is made of: the memtables and the tables of every level, each a
/// sorted sequence of internal keys, merged into the one sequence the read walks.
///
/// Each child must be sorted under `cmp`. An entry that is in two children is returned twice, once
/// from each, and on equal keys the child with the lower index comes first, so that listing the
/// newest source first makes it come first. The iterator does no more than that: it does not
/// drop an older entry that a newer one hides, which is for the layer above.
///
/// An error in a child is not skipped. A child that is invalid because it failed has not run out
/// of entries, and merging on without it would return the others' entries with its own missing,
/// among them a deletion marker that would have hidden an older value. So the iteration stops at
/// the first child found failed, and stays stopped, and a later seek is ignored, as the iterators
/// of blocks and tables behave. status() returns the first error among the children, whether the
/// child is still valid or not.
///
/// Each step looks at every child, which is the right cost for the tens of children a read has
/// and not for thousands. The iterator owns the children. With no children it is an empty
/// iterator, and with one it is that child.
/// @param children the sequences to merge; none may be null.
/// @param cmp the order the children are sorted in.
std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children,
                                             KeyCompareFn cmp);

}  // namespace shale

#endif  // SHALE_IO_MERGING_ITERATOR_H_
