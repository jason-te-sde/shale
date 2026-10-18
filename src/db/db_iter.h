#ifndef SHALE_DB_DB_ITER_H_
#define SHALE_DB_DB_ITER_H_

// The iterator a caller of DB::NewIterator() gets: the entries of the database as the user sees
// them, made from the internal entries of the memtables and tables, which hold every write
// that is not yet dropped and every deletion.

#include <memory>

#include "shale/iterator.h"
#include "util/dbformat.h"

namespace shale {

/// Returns the user view of `internal_iter` as of `sequence`: of the entries for each user key, the
/// newest one that is numbered at or below `sequence`, and nothing for a key whose newest such
/// entry is a deletion. Writes numbered above `sequence` are not there for it. The iterator takes
/// each user key once, in ascending bytewise order, with the value of that entry.
///
/// `internal_iter` must be over internal keys in CompareInternalKey() order, as the merge of the
/// memtables and tables is, and is owned by the result. The iterator it returns walks forward
/// only, like every Iterator of the engine. Seek() takes a user key, not an internal one, and
/// finds the first key that is not less than it; key() returns a user key. The views key() and
/// value() return are into `internal_iter`, and last until the next move, as Iterator says.
///
/// Failures are not swallowed, and are not passed over:
///  - An internal key that does not parse, which is shorter than its 8-byte tag or has a type that
///    does not exist, ends the iteration, with status Corruption("corrupted internal key in DB
///    iterator"). A later seek does nothing, so that the key is not skipped by seeking past it.
///  - A failure of `internal_iter`, which makes it invalid, ends the iteration, and status()
///    returns it. The iterator then is not Valid(), so a caller that only checks Valid() takes the
///    failure for the end of the data, which is what status() is there to tell apart.
///
/// Aborts if `sequence` is above kMaxSequenceNumber, or `internal_iter` is null.
/// @param internal_iter the internal entries; owned.
/// @param sequence the sequence number to read as of: the newest write that is seen.
std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> internal_iter,
                                        SequenceNumber sequence);

}  // namespace shale

#endif  // SHALE_DB_DB_ITER_H_
